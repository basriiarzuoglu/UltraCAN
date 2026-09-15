#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QJSEngine>
#include <QQmlEngine>
#include <QUrl>
#include <QVariantList>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include "dbc/DbcParser.h"

/*!
    \brief One signal, exposed to QML as an object with notifying properties.

    The QML written in phase 2 does things like
    \c{DbcModel.signalAt(m, s).factor} and expects the value to update when the
    editor writes it back. A QVariantMap snapshot could not do that, so each
    signal is a real QObject. The property names are exactly the role names the
    views already use.
*/
class DbcSignalObject : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged FINAL)
    Q_PROPERTY(int startBit READ startBit WRITE setStartBit NOTIFY startBitChanged FINAL)
    Q_PROPERTY(int bitLength READ bitLength WRITE setBitLength NOTIFY bitLengthChanged FINAL)
    Q_PROPERTY(int byteOrder READ byteOrder WRITE setByteOrder NOTIFY byteOrderChanged FINAL)
    Q_PROPERTY(int valueType READ valueType WRITE setValueType NOTIFY valueTypeChanged FINAL)
    Q_PROPERTY(double factor READ factor WRITE setFactor NOTIFY factorChanged FINAL)
    Q_PROPERTY(double offset READ offset WRITE setOffset NOTIFY offsetChanged FINAL)
    Q_PROPERTY(double minimum READ minimum WRITE setMinimum NOTIFY minimumChanged FINAL)
    Q_PROPERTY(double maximum READ maximum WRITE setMaximum NOTIFY maximumChanged FINAL)
    Q_PROPERTY(QString unit READ unit WRITE setUnit NOTIFY unitChanged FINAL)
    Q_PROPERTY(QString receiver READ receiver WRITE setReceiver NOTIFY receiverChanged FINAL)
    Q_PROPERTY(QString comment READ comment WRITE setComment NOTIFY commentChanged FINAL)
    /*!
        The VAL_ table as a list of \c{{value, description}} maps, sorted by
        value - directly usable as a Repeater / TableView model.

        A list rather than a QVariantMap because a QVariantMap keyed on a
        NUMBER round-trips through QML as string keys ("10" sorts before "9")
        and loses the ordering the dialog depends on.
    */
    Q_PROPERTY(QVariantList valueDescriptions READ valueDescriptions
               WRITE setValueDescriptions NOTIFY valueDescriptionsChanged FINAL)
    /*! Convenience for a binding that only wants to know whether to show the
        enumeration text at all. */
    Q_PROPERTY(bool hasValueDescriptions READ hasValueDescriptions
               NOTIFY valueDescriptionsChanged FINAL)

public:
    explicit DbcSignalObject(QObject *parent = nullptr) : QObject(parent) {}

    const DbcSignalData &data() const { return m_data; }
    void setData(const DbcSignalData &data);

    QString name() const { return m_data.name; }
    int startBit() const { return m_data.startBit; }
    int bitLength() const { return m_data.bitLength; }
    int byteOrder() const { return m_data.byteOrder; }
    int valueType() const { return m_data.valueType; }
    double factor() const { return m_data.factor; }
    double offset() const { return m_data.offset; }
    double minimum() const { return m_data.minimum; }
    double maximum() const { return m_data.maximum; }
    QString unit() const { return m_data.unit; }
    QString receiver() const { return m_data.receivers.join(QStringLiteral(", ")); }
    QString comment() const { return m_data.comment; }
    QVariantList valueDescriptions() const;
    bool hasValueDescriptions() const { return !m_data.valueDescriptions.isEmpty(); }

    /*!
        Replaces the whole table.

        Whole-list replacement, not incremental edits: the values dialog is a
        cancellable form, so it needs to build a candidate table and commit it
        in one go. Entries may arrive as \c{{value, description}} maps or as
        two-element arrays; anything whose value is not an integer is dropped.
    */
    void setValueDescriptions(const QVariantList &entries);

    /*! Adds or replaces one entry. Returns false if \a value is already used
        and \a replace is false, which is what makes the dialog's '+' button
        able to refuse a duplicate instead of silently overwriting. */
    Q_INVOKABLE bool setValueDescription(int value, const QString &description,
                                         bool replace = true);
    Q_INVOKABLE void removeValueDescription(int value);
    Q_INVOKABLE void clearValueDescriptions();

    /*! Display text for a RAW (pre-scaling) value, empty when unmapped. */
    Q_INVOKABLE QString describeRaw(int raw) const
    { return m_data.valueDescriptions.value(raw); }

    /*!
        Display text for a PHYSICAL (scaled) value, empty when unmapped.

        Inverts factor/offset first, because VAL_ is keyed on the raw integer.
        Reading the table with the scaled number would work only while factor
        is 1 and offset 0, and would break the moment someone set a scale -
        the bug being that it keeps working for the common case.
    */
    Q_INVOKABLE QString describeValue(double physical) const;

    /*!
        The value AND its name: \c{"32 (P)"} when the raw code is mapped,
        \c{"32.000"} when it is not.

        Shares ValueFormat.h with CanDecoder, so the Raw Analyzer and this
        editor cannot end up formatting the same signal differently.
    */
    Q_INVOKABLE QString formattedValue(double physical) const;

    /*!
        Physical range the signal can actually represent, from its width,
        signedness, factor and offset - what the "Auto" buttons next to Min and
        Max fill in.

        A negative factor inverts the ends, which is why these are computed as
        a pair and then ordered, rather than each end being derived on its own.
        Above 53 bits the raw span exceeds what a double can count exactly and
        these become approximate; a signal that wide has no meaningful
        engineering range anyway.
    */
    Q_INVOKABLE double autoMinimum() const;
    Q_INVOKABLE double autoMaximum() const;

    void setName(const QString &value);
    void setStartBit(int value);
    void setBitLength(int value);
    void setByteOrder(int value);
    void setValueType(int value);
    void setFactor(double value);
    void setOffset(double value);
    void setMinimum(double value);
    void setMaximum(double value);
    void setUnit(const QString &value);
    void setReceiver(const QString &value);
    void setComment(const QString &value);

signals:
    void nameChanged();
    void startBitChanged();
    void bitLengthChanged();
    void byteOrderChanged();
    void valueTypeChanged();
    void factorChanged();
    void offsetChanged();
    void minimumChanged();
    void maximumChanged();
    void unitChanged();
    void receiverChanged();
    void commentChanged();
    void valueDescriptionsChanged();
    /*! Any property changed - the owning model turns this into dataChanged(). */
    void edited();

private:
    DbcSignalData m_data;
};

/*!
    \brief The signals of one message.

    Doubles as a QAbstractListModel (so a Repeater can use it straight away)
    and as a ListModel look-alike (\c count, \c get(), \c setProperty()), which
    is what keeps DbcExplorer.qml working unchanged.
*/
class DbcSignalListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS

    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        StartBitRole, BitLengthRole, ByteOrderRole, ValueTypeRole,
        FactorRole, OffsetRole, MinimumRole, MaximumRole,
        UnitRole, ReceiverRole, CommentRole, ValueDescriptionsRole
    };
    Q_ENUM(Role)

    explicit DbcSignalListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return m_signals.size(); }

    /*! ListModel-compatible accessors used by the existing QML. */
    Q_INVOKABLE DbcSignalObject *get(int row) const;
    Q_INVOKABLE void setProperty(int row, const QString &role, const QVariant &value);

    void applyData(const QVector<DbcSignalData> &data);
    QVector<DbcSignalData> toData() const;

    void appendSignal(const DbcSignalData &data);
    void removeSignal(int row);

signals:
    void countChanged();
    void edited();

private:
    void connectSignalObject(DbcSignalObject *object);

    QVector<DbcSignalObject *> m_signals;
    // Emitting dataChanged() between beginResetModel() and endResetModel() is
    // illegal, and applyData() does trigger the edited() path while rebuilding.
    bool m_resetting = false;
};

/*! \brief One message, with notifying properties and its signal list. */
class DbcMessageObject : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged FINAL)
    Q_PROPERTY(quint32 canId READ canId WRITE setCanId NOTIFY canIdChanged FINAL)
    Q_PROPERTY(bool extended READ extended WRITE setExtended NOTIFY extendedChanged FINAL)
    Q_PROPERTY(int dlc READ dlc WRITE setDlc NOTIFY dlcChanged FINAL)
    Q_PROPERTY(QString transmitter READ transmitter WRITE setTransmitter NOTIFY transmitterChanged FINAL)
    Q_PROPERTY(int cycleMs READ cycleMs WRITE setCycleMs NOTIFY cycleMsChanged FINAL)
    Q_PROPERTY(QString comment READ comment WRITE setComment NOTIFY commentChanged FINAL)
    /*! Pure UI state: whether the explorer shows this message expanded. */
    Q_PROPERTY(bool expanded READ expanded WRITE setExpanded NOTIFY expandedChanged FINAL)
    Q_PROPERTY(DbcSignalListModel *signalList READ signalList CONSTANT FINAL)

public:
    explicit DbcMessageObject(QObject *parent = nullptr);

    QString name() const { return m_name; }
    quint32 canId() const { return m_canId; }
    bool extended() const { return m_extended; }
    int dlc() const { return m_dlc; }
    QString transmitter() const { return m_transmitter; }
    int cycleMs() const { return m_cycleMs; }
    QString comment() const { return m_comment; }
    bool expanded() const { return m_expanded; }
    DbcSignalListModel *signalList() const { return m_signalList; }

    void setName(const QString &value);
    void setCanId(quint32 value);
    void setExtended(bool value);
    void setDlc(int value);
    void setTransmitter(const QString &value);
    void setCycleMs(int value);
    void setComment(const QString &value);
    void setExpanded(bool value);

    void applyData(const DbcMessageData &data);
    DbcMessageData toData() const;

signals:
    void nameChanged();
    void canIdChanged();
    void extendedChanged();
    void dlcChanged();
    void transmitterChanged();
    void cycleMsChanged();
    void commentChanged();
    void expandedChanged();
    void edited();

private:
    QString m_name;
    quint32 m_canId = 0;
    bool m_extended = false;
    int m_dlc = 8;
    QString m_transmitter;
    int m_cycleMs = 0;
    QString m_comment;
    bool m_expanded = true;
    DbcSignalListModel *m_signalList = nullptr;
};

/*!
    \brief The CAN database, backed by the real DbcParser.

    Drop-in replacement for the QML DbcDummyModel singleton: same property
    names, same function names, same numeric conventions. \c messages returns
    the model itself, so \c{DbcModel.messages.count} / \c{.get(i)} /
    \c{.setProperty(i, role, value)} all behave like the old ListModel while
    a ListView can bind to it directly.

    Message objects are REUSED across loads (only the surplus is discarded), so
    a QML binding that captured \c{DbcModel.message(2)} keeps pointing at a live
    object after a new file is parsed instead of at a deleted one.
*/
class DbcModel : public QAbstractListModel
{
    Q_OBJECT
    // NOT QML_ELEMENT / QML_SINGLETON: this instance is created and owned by
    // main() and published with qmlRegisterSingletonInstance(). Letting the
    // engine register the type as well would give QML a SECOND, unwired
    // instance - which is exactly what happened before.
    // QML_ANONYMOUS keeps the metatype registered (needed wherever this type is
    // returned from a Q_PROPERTY) without claiming the type NAME.
    QML_ANONYMOUS

    Q_PROPERTY(DbcModel *messages READ messages CONSTANT FINAL)
    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(QString databaseName READ databaseName WRITE setDatabaseName NOTIFY databaseNameChanged FINAL)
    Q_PROPERTY(bool modified READ modified WRITE setModified NOTIFY modifiedChanged FINAL)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged FINAL)
    /*! The file this database came from, empty for the demo content. Drives
        the "reopen last database" restore in SettingsManager. */
    Q_PROPERTY(QUrl sourceUrl READ sourceUrl NOTIFY sourceUrlChanged FINAL)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY loaded FINAL)
    Q_PROPERTY(QStringList nodeNames READ nodeNames NOTIFY nodeNamesChanged FINAL)
    Q_PROPERTY(QStringList byteOrderNames READ byteOrderNames CONSTANT FINAL)
    Q_PROPERTY(QStringList valueTypeNames READ valueTypeNames CONSTANT FINAL)
    Q_PROPERTY(int byteOrderMotorola READ byteOrderMotorola CONSTANT FINAL)
    Q_PROPERTY(int byteOrderIntel READ byteOrderIntel CONSTANT FINAL)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        CanIdRole, ExtendedRole, DlcRole, TransmitterRole,
        CycleMsRole, CommentRole, ExpandedRole, SignalListRole
    };
    Q_ENUM(Role)

    explicit DbcModel(QObject *parent = nullptr);

    static DbcModel *instance();
    // No create() hook: main() publishes this instance with
    // qmlRegisterSingletonInstance(); see the note above QML_ANONYMOUS.

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // -- ListModel look-alike --------------------------------------------------
    DbcModel *messages() { return this; }
    int count() const { return m_messages.size(); }
    Q_INVOKABLE DbcMessageObject *get(int row) const;
    Q_INVOKABLE void setProperty(int row, const QString &role, const QVariant &value);

    // -- the DbcDummyModel API the views already call --------------------------
    Q_INVOKABLE int messageCount() const { return m_messages.size(); }
    Q_INVOKABLE DbcMessageObject *message(int index) const;
    Q_INVOKABLE DbcSignalListModel *signalsOf(int messageIndex) const;
    Q_INVOKABLE DbcSignalObject *signalAt(int messageIndex, int signalIndex) const;

    Q_INVOKABLE void setMessageProperty(int messageIndex, const QString &role, const QVariant &value);
    Q_INVOKABLE void setSignalProperty(int messageIndex, int signalIndex,
                                       const QString &role, const QVariant &value);

    Q_INVOKABLE int addMessage();
    Q_INVOKABLE int addSignal(int messageIndex);
    Q_INVOKABLE void removeMessage(int messageIndex);
    Q_INVOKABLE void removeSignal(int messageIndex, int signalIndex);
    Q_INVOKABLE void toggleExpanded(int messageIndex);
    Q_INVOKABLE void setExpandedAll(bool expanded);

    Q_INVOKABLE QString formatCanId(quint32 id, bool extended = false) const;

    // -- file I/O --------------------------------------------------------------
    /*! Parses \a fileUrl (a QML FileDialog url or a plain path). */
    Q_INVOKABLE bool loadDbc(const QUrl &fileUrl);
    /*! Writes the current database back out. */
    Q_INVOKABLE bool saveDbc(const QUrl &fileUrl);
    /*! The phase-2 demo content, so the UI is not empty before a file is loaded. */
    Q_INVOKABLE void loadDemo();

    // -- plain accessors -------------------------------------------------------
    QString databaseName() const { return m_databaseName; }
    void setDatabaseName(const QString &name);
    bool modified() const { return m_modified; }
    void setModified(bool modified);
    QString lastError() const { return m_lastError; }
    QUrl sourceUrl() const { return m_sourceUrl; }
    QStringList warnings() const { return m_warnings; }
    QStringList nodeNames() const { return m_nodeNames; }
    QStringList byteOrderNames() const;
    QStringList valueTypeNames() const;
    int byteOrderMotorola() const { return 0; }
    int byteOrderIntel() const { return 1; }

    /*! The whole database as plain structs - for the C++ decoder, later. */
    DbcDatabase database() const;

signals:
    void countChanged();
    void databaseNameChanged();
    void modifiedChanged();
    void lastErrorChanged();
    void sourceUrlChanged();
    void nodeNamesChanged();
    /*! A whole database was parsed in. */
    void loaded();
    /*!
        The database content changed in ANY way - loaded, message or signal
        added or removed, or a property edited. Consumers that cache a compiled
        view of the database (CanDecoder) must rebuild on this, not on loaded():
        editing a start bit in the editor changes decoding just as much as
        opening a new file does.
    */
    void databaseChanged();

private:
    void applyDatabase(const DbcDatabase &database);
    void setLastError(const QString &message);
    void connectMessageObject(DbcMessageObject *object, int row);

    static DbcModel *s_instance;

    QVector<DbcMessageObject *> m_messages;
    QString m_databaseName;
    QStringList m_nodeNames;
    QStringList m_warnings;
    QString m_lastError;
    QUrl m_sourceUrl;
    bool m_modified = false;
    bool m_resetting = false;      //!< see DbcSignalListModel::m_resetting
};
