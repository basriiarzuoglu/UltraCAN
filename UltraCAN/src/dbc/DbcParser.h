#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

/*!
    \brief One signal of a CAN message, as written in a Vector .dbc file.

    Field semantics follow the file format exactly, so a round trip through the
    parser and the writer changes nothing:
      - \c byteOrder is the \c @0 / \c @1 token: 0 = Motorola (big endian),
        1 = Intel (little endian);
      - \c valueType is the sign token: 0 = unsigned (+), 1 = signed (-).
    These are the same numbers the QML has used since phase 2.
*/
struct DbcSignalData
{
    QString name;
    int startBit = 0;
    int bitLength = 1;
    int byteOrder = 1;          //!< 0 = Motorola/BE, 1 = Intel/LE
    int valueType = 0;          //!< 0 = unsigned, 1 = signed
    double factor = 1.0;
    double offset = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    QString unit;
    QStringList receivers;
    QString comment;
    QString multiplexer;        //!< "", "M" (multiplexor) or "m<n>" (multiplexed)
    /*!
        VAL_ table: RAW value -> display text ("1" -> "TEST").

        Keyed on the raw, pre-scaling integer, because that is what the file
        stores and what the bus carries. Applying factor/offset first would
        make the mapping depend on scaling that the editor can change
        afterwards, silently orphaning every entry.

        QMap and not QHash: the dialog lists these, and a list of enumeration
        values that reorders itself between openings is unusable. QMap iterates
        in key order for free.
    */
    QMap<int, QString> valueDescriptions;
};

/*! \brief One CAN message (BO_) with its signals. */
struct DbcMessageData
{
    quint32 id = 0;             //!< identifier WITHOUT the extended flag bit
    bool extended = false;
    QString name;
    int dlc = 8;
    QString transmitter;
    int cycleMs = 0;            //!< from BA_ "GenMsgCycleTime"
    QString comment;
    // NOTE: cannot be called "signals" - that is a Qt keyword macro.
    QVector<DbcSignalData> signalList;
};

/*! \brief A parsed database. */
struct DbcDatabase
{
    QString name;               //!< file name, for display
    QStringList nodes;          //!< BU_ node list
    QVector<DbcMessageData> messages;
    QStringList warnings;       //!< lines the parser could not make sense of

    bool isEmpty() const { return messages.isEmpty(); }
};

/*!
    \brief Line-oriented reader/writer for Vector .dbc files.

    Deliberately tolerant: unknown sections are skipped rather than treated as
    errors, because real databases carry vendor extensions no parser covers
    completely. Anything skipped that looked like it mattered lands in
    DbcDatabase::warnings so the UI can say so.

    Supported: BU_ (nodes), BO_ (messages), SG_ (signals incl. the multiplexer
    token), CM_ (message and signal comments, single or multi line),
    BA_ "GenMsgCycleTime" (cycle time), VAL_ (value description tables).
    Skipped: SIG_VALTYPE_ (float/double signals), attribute definitions,
    signal groups.
*/
class DbcParser
{
public:
    static DbcDatabase parseFile(const QString &filePath, QString *errorMessage = nullptr);
    static DbcDatabase parseText(const QString &text, const QString &databaseName = QString());

    /*! Minimal but loadable .dbc text: BU_, BO_/SG_, CM_ and cycle times. */
    static QString serialize(const DbcDatabase &database);
};
