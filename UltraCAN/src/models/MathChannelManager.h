#pragma once

#include <QHash>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonObject>
#include <QJSValue>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

/*!
    \class MathChannelManager
    \brief User-defined signals computed from decoded ones - "Power = RPM * Torque / 9550".

    \section1 What it knows and what it does not

    It knows formulas, variable names and how to turn a set of numbers into one
    number. It knows nothing about CAN, decoders or the graph. A binding names
    its input as a (database, message, signal) TRIPLE OF STRINGS and it is
    \c GraphDataModel that resolves those to decoder keys and feeds the values
    back in. That split is what keeps the evaluator testable without a bus, and
    what lets a channel survive a database being reloaded in a different order.

    \section1 Evaluation runs on the GUI thread, at repaint rate

    \c QJSEngine is not thread-safe and must be used from the thread that owns
    it. Frames are decoded on the DEVICE thread, so evaluating there is not an
    option even if it were fast. \c GraphDataModel therefore stores the latest
    value of each input as frames arrive, and calls \l evaluate() once per
    refresh - 60 Hz, and only for channels whose inputs actually moved. A
    1 kHz bus does not become 1 kHz of JavaScript.

    \section1 The formula is compiled ONCE

    \c QJSEngine::evaluate() on a string parses it every time. Each channel is
    instead wrapped once into a function -
    \c {(function(A,B){ return (A*B)/9550; })} - and the resulting \c QJSValue
    is \c call()ed per sample. That is the difference between a parse and a
    call on every point.

    \section1 Why a whitelist and not a sandbox

    The formula is a string a user types, compiled into the process and run on
    the GUI thread sixty times a second. \c {while(true){}} would freeze the
    application with no way back, and QJSEngine offers no timeout that does not
    need a watchdog thread. Rather than contain a hostile expression, \l
    validate() refuses to compile one: the only tokens that survive are numbers,
    the variables actually mapped, a short list of \c Math functions, and
    arithmetic operators. No assignment, no statements, no identifiers we did
    not put there - so there is no loop to run away and no global to reach.

    This is deliberately stricter than "valid JavaScript". A formula that a
    calculator would accept, this accepts; anything else is a typo or an
    attempt, and both deserve the same answer.
*/
class MathChannelManager : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

    /*! Every channel, for the sidebar:
        [{ id, name, unit, expression, valid, error, bindings: [{variable, dbc,
        message, signal}] }] */
    Q_PROPERTY(QVariantList channels READ channels NOTIFY channelsChanged FINAL)
    Q_PROPERTY(int count READ count NOTIFY channelsChanged FINAL)
    /*! Functions a formula may call, for the dialog's help line. */
    Q_PROPERTY(QStringList allowedFunctions READ allowedFunctions CONSTANT FINAL)

public:
    explicit MathChannelManager(QObject *parent = nullptr);

    /*! One input of a channel, named the way a person names it. */
    struct Binding
    {
        QString variable;      //!< as it appears in the formula, e.g. "A"
        QString dbcName;
        QString messageName;
        QString signalName;
    };

    int count() const { return int(m_order.size()); }
    QVariantList channels() const;
    QStringList allowedFunctions() const;

    // -- authoring, from QML ---------------------------------------------------
    /*!
        The identifiers a formula uses, in first-appearance order, so the dialog
        can grow one mapping row per variable as the user types.

        Names that are part of the language - the \c Math functions - are not
        variables and are left out.
    */
    Q_INVOKABLE QStringList variablesIn(const QString &expression) const;

    /*!
        Empty when \a expression is acceptable, otherwise a sentence naming what
        is wrong. Safe to call on every keystroke; it compiles nothing.
    */
    Q_INVOKABLE QString validate(const QString &expression,
                                 const QStringList &variables) const;

    /*! Creates a channel. \a bindings is a list of
        \c {{variable, dbc, message, signal}} maps. Returns the id, or -1 with
        \l lastError set. */
    Q_INVOKABLE int createChannel(const QString &name, const QString &unit,
                                  const QString &expression,
                                  const QVariantList &bindings);
    Q_INVOKABLE bool removeChannel(int channelId);
    /*! Drops every channel. One channelsChanged(), not one per channel:
        GraphDataModel untracks on that signal, and N emissions would make it
        walk the track list N times to remove one trace each. */
    Q_INVOKABLE void clearChannels();
    Q_INVOKABLE QVariantMap channelAt(int channelId) const;
    /*! Name -> id, for a config import naming channels rather than ids. */
    Q_INVOKABLE int channelIdFor(const QString &name) const;

    QString lastError() const { return m_lastError; }

    // -- session persistence ---------------------------------------------------
    /*!
        Every channel as \c {[{ name, unit, formula, bindings: [{ variable,
        dbc, message, signal }] }]}.

        Names, not ids or indices: an id is a counter private to one run of the
        program, and a signal index is a position in a database that the far
        end may have loaded in a different order. A name survives both.
    */
    QJsonArray toJson() const;

    /*!
        Replaces every channel with the contents of \a array. Returns the
        number that could NOT be restored; \l lastError() names the last one.

        A channel is rebuilt through createChannel(), so an imported formula
        goes through exactly the same whitelist and compile as a typed one - a
        hand-edited session file gets no privileges the dialog does not have.
        A channel that fails is skipped and counted, never half-created.
    */
    int fromJson(const QJsonArray &array);

    // -- evaluation, from GraphDataModel (C++, GUI thread) ---------------------
    QVector<Binding> bindingsOf(int channelId) const;
    QString nameOf(int channelId) const;
    QString unitOf(int channelId) const;
    bool isValid(int channelId) const;

    /*!
        Runs the channel's formula over \a arguments, in the order
        \l bindingsOf() returned.

        Returns false - and leaves \a out untouched - when the channel is
        unknown, the argument count is wrong, the call threw, or the result is
        not finite. \b {Non-finite is a failure, not a value}: a divide by zero
        yields Infinity, and one Infinity in a series collapses the chart's
        auto-scale so nothing else on the pane can be read. Dropping the sample
        leaves a visible gap, which is the honest rendering of "this could not
        be computed".
    */
    bool evaluate(int channelId, const QVector<double> &arguments, double *out);

    /*!
        Announces a computed sample as \l virtualSignalUpdated().

        Separate from \l evaluate() on purpose: evaluate() is a pure function
        and is also used to probe a formula, while a SAMPLE is a thing that
        happened at a time, and only the caller feeding the inputs knows which
        timestamp it happened at. Called once per new value, not once per pane
        plotting it.
    */
    void publishSample(int channelId, double value, double timestampSeconds);

signals:
    void channelsChanged();
    /*! A channel's inputs changed, so GraphDataModel must re-resolve them. */
    void bindingsChanged();
    /*!
        One virtual sample, for anything that wants the value stream without
        owning a chart - a readout, a logger, a future alarm rule.

        \a timestamp is in SECONDS on the bus clock, matching the graph's x
        axis rather than the ns the frames carry: a double holding nanoseconds
        loses integer precision after a few months of uptime, and every
        consumer of this signal plots or prints seconds anyway.
    */
    void virtualSignalUpdated(const QString &name, double value, double timestamp);

private:
    struct Channel
    {
        int id = -1;
        QString name;
        QString unit;
        QString expression;
        QVector<Binding> bindings;
        QJSValue function;         //!< compiled once; called per sample
        bool valid = false;
        QString error;
    };

    /*! Splits \a expression into tokens and rejects anything not on the list.
        Returns an error sentence, or empty. \a variables are the identifiers
        the caller has declared; any other identifier is an error. */
    QString scan(const QString &expression, const QStringList &variables) const;
    bool compile(Channel &channel);
    void setLastError(const QString &message);

    mutable QJSEngine m_engine;
    QHash<int, Channel> m_channels;
    QVector<int> m_order;          //!< creation order, for a stable sidebar
    int m_nextId = 1;
    QString m_lastError;
};
