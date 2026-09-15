#include "CanDecoder.h"

#include <QReadLocker>
#include <QWriteLocker>

#include <limits>

// ------------------------------------------------------------- the bit math

quint64 CanDecoder::extractRaw(const quint8 *data, int dlc,
                               int startBit, int bitLength, bool intel)
{
    quint64 raw = 0;

    if (intel) {
        // Intel / little endian: bit i of the signal (i = 0 is the LSB) lives
        // at absolute bit startBit + i, counting from bit 0 of byte 0.
        for (int i = 0; i < bitLength; ++i) {
            const int position = startBit + i;
            const int byteIndex = position >> 3;
            if (byteIndex >= dlc)
                break;
            if ((data[byteIndex] >> (position & 7)) & 1)
                raw |= (1ull << i);
        }
    } else {
        // Motorola / big endian: startBit is the MSB of the signal. Walk down
        // inside the byte, then continue at bit 7 of the next byte - the
        // "sawtooth" order .dbc uses. This mirrors motorolaBitIndex() in the
        // QML encoder, so what the sender packs is what the decoder reads.
        int byteIndex = startBit >> 3;
        int bitIndex = startBit & 7;
        for (int i = 0; i < bitLength; ++i) {
            if (byteIndex >= dlc)
                break;
            if ((data[byteIndex] >> bitIndex) & 1)
                raw |= (1ull << (bitLength - 1 - i));
            if (bitIndex == 0) {
                bitIndex = 7;
                ++byteIndex;
            } else {
                --bitIndex;
            }
        }
    }

    return raw;
}

double CanDecoder::toPhysical(quint64 raw, int bitLength, bool isSigned,
                              double factor, double offset)
{
    double value = 0.0;

    if (isSigned && bitLength > 0 && bitLength < 64) {
        const quint64 signBit = 1ull << (bitLength - 1);
        if (raw & signBit)
            value = double(qint64(raw) - qint64(1ull << bitLength));   // two's complement
        else
            value = double(raw);
    } else if (isSigned && bitLength == 64) {
        value = double(qint64(raw));
    } else {
        value = double(raw);
    }

    return value * factor + offset;
}

// ----------------------------------------------------------- the encoder

quint64 CanDecoder::toRaw(double physical, int bitLength, bool isSigned,
                          double factor, double offset)
{
    if (bitLength <= 0 || bitLength > 64)
        return 0;
    if (qFuzzyIsNull(factor))
        factor = 1.0;

    const double scaled = (physical - offset) / factor;
    qint64 raw = qint64(scaled < 0 ? scaled - 0.5 : scaled + 0.5);   // round half away from zero

    if (isSigned) {
        const qint64 lo = -(qint64(1) << (bitLength - 1));
        const qint64 hi = (qint64(1) << (bitLength - 1)) - 1;
        raw = qBound(lo, raw, hi);
        if (raw < 0)
            return quint64(raw + (qint64(1) << bitLength));          // two's complement
        return quint64(raw);
    }

    const qint64 hi = (bitLength == 64) ? std::numeric_limits<qint64>::max()
                                        : ((qint64(1) << bitLength) - 1);
    raw = qBound<qint64>(0, raw, hi);
    return quint64(raw);
}

void CanDecoder::insertRaw(quint8 *data, int dlc,
                           int startBit, int bitLength, bool intel, quint64 raw)
{
    if (intel) {
        for (int i = 0; i < bitLength; ++i) {
            if (!((raw >> i) & 1))
                continue;
            const int position = startBit + i;
            const int byteIndex = position >> 3;
            if (byteIndex >= dlc)
                break;
            data[byteIndex] |= quint8(1u << (position & 7));
        }
    } else {
        int byteIndex = startBit >> 3;
        int bitIndex = startBit & 7;
        for (int i = 0; i < bitLength; ++i) {
            if (byteIndex >= dlc)
                break;
            if ((raw >> (bitLength - 1 - i)) & 1)
                data[byteIndex] |= quint8(1u << bitIndex);
            if (bitIndex == 0) {
                bitIndex = 7;
                ++byteIndex;
            } else {
                --bitIndex;
            }
        }
    }
}

// ------------------------------------------------------------- the table

void CanDecoder::setDatabase(const DbcDatabase &database)
{
    QHash<quint32, QVector<SignalInfo>> byFrame;
    QVector<SignalDescriptor> descriptors;
    QHash<quint64, int> keyByPosition;
    QHash<quint32, QString> messageNames;

    int key = 0;
    for (int m = 0; m < database.messages.size(); ++m) {
        const DbcMessageData &message = database.messages.at(m);
        QVector<SignalInfo> infos;
        infos.reserve(message.signalList.size());

        for (int s = 0; s < message.signalList.size(); ++s) {
            const DbcSignalData &source = message.signalList.at(s);

            SignalInfo info;
            info.key = key;
            info.startBit = quint16(source.startBit);
            info.bitLength = quint16(qBound(1, source.bitLength, 64));
            info.intel = source.byteOrder != 0;          // @1 = Intel, @0 = Motorola
            info.isSigned = source.valueType == 1;
            info.factor = source.factor != 0.0 ? source.factor : 1.0;
            info.offset = source.offset;
            infos.append(info);

            SignalDescriptor descriptor;
            descriptor.signalName = source.name;
            descriptor.messageName = message.name;
            descriptor.unit = source.unit;
            descriptor.minimum = source.minimum;
            descriptor.maximum = source.maximum;
            descriptor.factor = info.factor;      // already zero-guarded above
            descriptor.offset = info.offset;
            descriptor.valueDescriptions = source.valueDescriptions;
            descriptors.append(descriptor);

            keyByPosition.insert((quint64(m) << 32) | quint32(s), key);
            ++key;
        }

        const quint32 key = frameKey(message.id, message.extended);
        byFrame.insert(key, infos);
        messageNames.insert(key, message.name);
    }

    QWriteLocker locker(&m_lock);
    m_byFrame = std::move(byFrame);
    m_descriptors = std::move(descriptors);
    m_keyByPosition = std::move(keyByPosition);
    m_messageNames = std::move(messageNames);
}

void CanDecoder::clear()
{
    QWriteLocker locker(&m_lock);
    m_byFrame.clear();
    m_descriptors.clear();
    m_keyByPosition.clear();
    m_messageNames.clear();
}

// ------------------------------------------------------------- lookups

int CanDecoder::signalKey(int messageIndex, int signalIndex) const
{
    QReadLocker locker(&m_lock);
    return m_keyByPosition.value((quint64(messageIndex) << 32) | quint32(signalIndex), -1);
}

int CanDecoder::signalCount() const
{
    QReadLocker locker(&m_lock);
    return m_descriptors.size();
}

QString CanDecoder::signalName(int key) const
{
    QReadLocker locker(&m_lock);
    return (key >= 0 && key < m_descriptors.size()) ? m_descriptors.at(key).signalName : QString();
}

QString CanDecoder::messageNameForFrame(quint32 id, bool extended) const
{
    QReadLocker locker(&m_lock);
    return m_messageNames.value(frameKey(id, extended));
}

QString CanDecoder::messageName(int key) const
{
    QReadLocker locker(&m_lock);
    return (key >= 0 && key < m_descriptors.size()) ? m_descriptors.at(key).messageName : QString();
}

QString CanDecoder::unit(int key) const
{
    QReadLocker locker(&m_lock);
    return (key >= 0 && key < m_descriptors.size()) ? m_descriptors.at(key).unit : QString();
}

QString CanDecoder::formatValue(int key, double physical) const
{
    QReadLocker locker(&m_lock);
    if (key < 0 || key >= m_descriptors.size())
        return QString::number(physical, 'f', UltraCAN::ValueDecimals);
    const SignalDescriptor &d = m_descriptors.at(key);
    return UltraCAN::formatSignalValue(physical, d.factor, d.offset, d.valueDescriptions);
}

bool CanDecoder::isNamedValue(int key, double physical) const
{
    QReadLocker locker(&m_lock);
    if (key < 0 || key >= m_descriptors.size())
        return false;
    const SignalDescriptor &d = m_descriptors.at(key);
    return UltraCAN::isNamedValue(physical, d.factor, d.offset, d.valueDescriptions);
}

double CanDecoder::minimum(int key) const
{
    QReadLocker locker(&m_lock);
    return (key >= 0 && key < m_descriptors.size()) ? m_descriptors.at(key).minimum : 0.0;
}

double CanDecoder::maximum(int key) const
{
    QReadLocker locker(&m_lock);
    return (key >= 0 && key < m_descriptors.size()) ? m_descriptors.at(key).maximum : 0.0;
}

// ------------------------------------------------------------- decoding

int CanDecoder::decode(const CanFrame &frame, QVector<DecodedSample> &out) const
{
    QReadLocker locker(&m_lock);

    const auto it = m_byFrame.constFind(frameKey(frame.id, frame.extended));
    if (it == m_byFrame.constEnd())
        return 0;

    for (const SignalInfo &info : *it) {
        const quint64 raw = extractRaw(frame.payload.data(), frame.dlc,
                                       info.startBit, info.bitLength, info.intel);
        DecodedSample sample;
        sample.key = info.key;
        sample.timestampNs = frame.timestampNs;
        sample.value = toPhysical(raw, info.bitLength, info.isSigned, info.factor, info.offset);
        out.append(sample);
    }
    return it->size();
}

int CanDecoder::decodeBatch(const QVector<CanFrame> &frames, QVector<DecodedSample> &out) const
{
    // One read lock for the whole batch, not one per frame.
    QReadLocker locker(&m_lock);

    int decoded = 0;
    for (const CanFrame &frame : frames) {
        const auto it = m_byFrame.constFind(frameKey(frame.id, frame.extended));
        if (it == m_byFrame.constEnd())
            continue;

        for (const SignalInfo &info : *it) {
            const quint64 raw = extractRaw(frame.payload.data(), frame.dlc,
                                           info.startBit, info.bitLength, info.intel);
            DecodedSample sample;
            sample.key = info.key;
            sample.timestampNs = frame.timestampNs;
            sample.value = toPhysical(raw, info.bitLength, info.isSigned,
                                      info.factor, info.offset);
            out.append(sample);
            ++decoded;
        }
    }
    return decoded;
}
