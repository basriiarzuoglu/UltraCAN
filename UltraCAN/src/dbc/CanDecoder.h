#pragma once

#include <QHash>
#include <QReadWriteLock>
#include <QString>
#include <QVector>

#include "core/CanFrame.h"
#include "dbc/DbcParser.h"
#include "dbc/ValueFormat.h"

/*!
    \brief One decoded value: which signal, when, what.

    An integer \c key, not a name. A QString per sample would allocate on the
    driver thread thousands of times a second; the key indexes a flat table the
    UI can resolve at its own leisure.
*/
struct DecodedSample
{
    int key = -1;
    qint64 timestampNs = 0;
    double value = 0.0;
};

Q_DECLARE_TYPEINFO(DecodedSample, Q_PRIMITIVE_TYPE);

/*!
    \brief Turns raw CAN payloads into physical values using the loaded DBC.

    Built once per database load into a flat, cache-friendly table:
      - \c m_byId maps a frame key (id + extended flag) to the signals in it,
        so decoding a frame is one hash lookup plus a loop;
      - each signal carries a stable integer \c key, which is its index into
        the flat table. Consumers (the graph) subscribe by key.

    Thread-safe: \l setDatabase() takes the write lock (rare, on DBC load) and
    \l decodeBatch() takes the read lock ONCE per batch, not per frame. Decoding
    deliberately runs on the acquisition thread - it is pure arithmetic, and
    doing it there keeps the GUI thread free for painting.
*/
class CanDecoder
{
public:
    CanDecoder() = default;

    /*! Rebuilds the decode table. Safe to call while decoding is in flight. */
    void setDatabase(const DbcDatabase &database);
    void clear();

    /*! Flat index of a signal, or -1. Indices follow DbcModel's own ordering. */
    int signalKey(int messageIndex, int signalIndex) const;
    int signalCount() const;

    QString signalName(int key) const;
    /*! Name of the message carrying \a id, or empty. Used to label recorded frames. */
    QString messageNameForFrame(quint32 id, bool extended) const;
    QString messageName(int key) const;
    QString unit(int key) const;
    /*!
        Display text for a decoded value: \c{"32 (P)"} when the raw code behind
        \a physical is named by the signal's VAL_ table, otherwise the plain
        three-decimal number. See ValueFormat.h for the rule itself, which the
        DBC Editor shares.
    */
    QString formatValue(int key, double physical) const;
    /*! True when \a physical maps to a VAL_ entry - lets a caller drop the
        unit, since a named enumeration member does not have one. */
    bool isNamedValue(int key, double physical) const;
    double minimum(int key) const;
    double maximum(int key) const;

    /*! Appends the signals found in \a frame to \a out. Returns how many. */
    int decode(const CanFrame &frame, QVector<DecodedSample> &out) const;
    int decodeBatch(const QVector<CanFrame> &frames, QVector<DecodedSample> &out) const;

    /*!
        Raw bit extraction, exposed for testing and for the encoder.
        \a data must hold at least \a dlc bytes.
    */
    static quint64 extractRaw(const quint8 *data, int dlc,
                              int startBit, int bitLength, bool intel);
    /*! Applies sign, factor and offset: phys = raw * factor + offset. */
    static double toPhysical(quint64 raw, int bitLength, bool isSigned,
                             double factor, double offset);

    // -- the encoder half -----------------------------------------------------
    /*!
        Inverse of toPhysical(): raw = clamp(round((phys - offset) / factor)),
        two's complement when \a isSigned.
    */
    static quint64 toRaw(double physical, int bitLength, bool isSigned,
                         double factor, double offset);
    /*!
        Inverse of extractRaw(): writes \a raw into \a data at the same bit
        positions the decoder reads it from. Keeping both halves in one place is
        what guarantees a frame this app sends decodes back to the value the
        user typed.
    */
    static void insertRaw(quint8 *data, int dlc,
                          int startBit, int bitLength, bool intel, quint64 raw);

private:
    struct SignalInfo
    {
        int key = -1;
        quint16 startBit = 0;
        quint16 bitLength = 1;
        bool intel = true;
        bool isSigned = false;
        double factor = 1.0;
        double offset = 0.0;
    };

    struct SignalDescriptor
    {
        QString signalName;
        QString messageName;
        QString unit;
        double minimum = 0.0;
        double maximum = 0.0;
        /*! Scaling is duplicated from SignalInfo so that formatting a value is
            a single lookup by KEY. SignalInfo is indexed by frame, and the
            display side only ever has a key. */
        double factor = 1.0;
        double offset = 0.0;
        /*! VAL_ table, raw code -> display text. Empty for most signals. */
        QMap<int, QString> valueDescriptions;
    };

    static quint32 frameKey(quint32 id, bool extended)
    {
        return extended ? (id | 0x80000000u) : id;
    }

    mutable QReadWriteLock m_lock;
    QHash<quint32, QVector<SignalInfo>> m_byFrame;   //!< frame key -> its signals
    QHash<quint32, QString> m_messageNames;          //!< frame key -> message name
    QVector<SignalDescriptor> m_descriptors;         //!< flat, indexed by signal key
    QHash<quint64, int> m_keyByPosition;             //!< (message<<32|signal) -> key
};
