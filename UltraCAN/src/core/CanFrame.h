#pragma once

#include <QMetaType>
#include <QString>
#include <QtGlobal>

#include <array>

/*!
    \brief One CAN frame as it travels through the backend.

    Deliberately a plain, copyable, allocation-free struct:
      - the payload is a fixed std::array (CAN FD maximum), not a QByteArray,
        so producing a frame on the driver thread never touches the heap;
      - the timestamp is integer nanoseconds from the one bus clock
        (CanBusManager), never a wall-clock or per-module counter;
      - the display strings (hex id, hex payload) are formatted on demand in
        the model's data(), i.e. only for the rows actually on screen.

    \c name is the DBC message name; it is an implicitly shared copy of the
    string held in the database table, so copying a frame stays cheap.
*/
struct CanFrame
{
    enum Direction : quint8 {
        Rx = 0,
        Tx = 1
    };

    qint64 timestampNs = 0;      //!< since CanBusManager's clock epoch
    quint32 id = 0;
    QString name;                //!< DBC message name, empty when unmapped
    quint8 channel = 0;          //!< index into CanBusManager::channelNames()
    quint8 dlc = 0;
    Direction direction = Rx;
    bool extended = false;
    std::array<quint8, 64> payload {};

    qreal seconds() const { return double(timestampNs) / 1'000'000'000.0; }

    QString idText() const
    {
        const QString hex = QStringLiteral("%1")
                            .arg(id, extended ? 8 : 3, 16, QLatin1Char('0')).toUpper();
        return QLatin1String("0x") + hex;
    }

    QString payloadText() const
    {
        QString out;
        out.reserve(dlc * 3);
        for (quint8 i = 0; i < dlc && i < payload.size(); ++i) {
            if (i > 0)
                out += QLatin1Char(' ');
            out += QStringLiteral("%1").arg(payload[i], 2, 16, QLatin1Char('0')).toUpper();
        }
        return out;
    }
};

Q_DECLARE_TYPEINFO(CanFrame, Q_RELOCATABLE_TYPE);
Q_DECLARE_METATYPE(CanFrame)
