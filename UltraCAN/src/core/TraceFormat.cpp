#include "TraceFormat.h"

#include <QDateTime>

namespace TraceFormat {

const char HexDigits[] = "0123456789ABCDEF";

void appendHexByte(QByteArray &out, quint8 value)
{
    out += HexDigits[(value >> 4) & 0x0F];
    out += HexDigits[value & 0x0F];
}

/*! Zero-padded uppercase hex, \a width digits. */
QByteArray hexId(quint32 id, int width)
{
    return QByteArray::number(qulonglong(id), 16).toUpper().rightJustified(width, '0');
}

void appendPayload(QByteArray &out, const CanFrame &frame)
{
    for (quint8 i = 0; i < frame.dlc && i < frame.payload.size(); ++i) {
        if (i > 0)
            out += ' ';
        appendHexByte(out, frame.payload[i]);
    }
}

// ------------------------------------------------------------------ formats
//
// All three are the formats CanPlaybackModel reads, so a trace this
// application records is a trace it can replay. Any change here needs the
// matching reader changed with it.

/*! Our own CSV. Identical to CanLogModel::toCsv() and to what
    CanPlaybackModel::readCsv() expects, header included. */
void appendCsvHeader(QByteArray &out)
{
    out += "Time,Dir,Ch,ID,Type,DLC,Data\n";
}

void appendCsvFrame(QByteArray &out, const CanFrame &frame, const QStringList &channelNames)
{
    out += QByteArray::number(frame.seconds(), 'f', 6);
    out += ',';
    out += (frame.direction == CanFrame::Tx ? "Tx" : "Rx");
    out += ',';
    out += channelNames.value(frame.channel, QStringLiteral("CAN?")).toUtf8();
    out += ",0x";
    out += hexId(frame.id, frame.extended ? 8 : 3);
    out += ',';
    out += (frame.extended ? "Ext" : "Std");
    out += ',';
    out += QByteArray::number(int(frame.dlc));
    out += ',';
    appendPayload(out, frame);
    out += '\n';
}

/*! Vector .asc. The header lines are the ones real tools look for; our reader
    skips anything starting with date/base/Begin/End or "//". */
void appendAscHeader(QByteArray &out)
{
    out += "date ";
    out += QDateTime::currentDateTime().toString(QStringLiteral("ddd MMM d hh:mm:ss yyyy")).toUtf8();
    out += "\nbase hex  timestamps absolute\n";
    out += "// exported by UltraCAN\n";
    out += "Begin Triggerblock ";
    out += QDateTime::currentDateTime().toString(QStringLiteral("ddd MMM d hh:mm:ss yyyy")).toUtf8();
    out += '\n';
}

void appendAscFrame(QByteArray &out, const CanFrame &frame)
{
    out += QByteArray::number(frame.seconds(), 'f', 6).rightJustified(12, ' ');
    out += ' ';
    // .asc channels are 1-based.
    out += QByteArray::number(int(frame.channel) + 1);
    out += "  ";
    // A trailing 'x' is what marks an extended identifier in this format.
    QByteArray id = hexId(frame.id, frame.extended ? 8 : 3);
    if (frame.extended)
        id += 'x';
    out += id.leftJustified(12, ' ');
    out += (frame.direction == CanFrame::Tx ? "Tx   " : "Rx   ");
    out += "d ";
    out += QByteArray::number(int(frame.dlc));
    out += ' ';
    appendPayload(out, frame);
    out += '\n';
}

void appendAscFooter(QByteArray &out)
{
    out += "End TriggerBlock\n";
}

/*! PCAN .trc, 2.0 dialect. Every header line starts with ';' and is skipped on
    the way back in. Times are milliseconds here, not seconds. */
void appendTrcHeader(QByteArray &out)
{
    out += ";$FILEVERSION=2.0\n";
    out += ";$STARTTIME=";
    out += QByteArray::number(QDateTime::currentDateTime().toSecsSinceEpoch());
    out += "\n;$COLUMNS=N,O,T,I,d,l,D\n;\n";
    out += ";   Message Number\n";
    out += ";   |         Time Offset (ms)\n";
    out += ";   |         |        Type\n";
    out += ";   |         |        |    ID (hex)\n";
    out += ";   |         |        |    |        Direction\n";
    out += ";   |         |        |    |        |  Data Length\n";
    out += ";   |         |        |    |        |  |  Data Bytes (hex)\n";
    out += ";---+-- ------+------ -+-- -+------ -+- +- -+ -- -- -- -- -- -- --\n";
}

void appendTrcFrame(QByteArray &out, const CanFrame &frame, int number)
{
    out += QByteArray::number(number).rightJustified(6, ' ');
    out += ' ';
    out += QByteArray::number(frame.seconds() * 1000.0, 'f', 3).rightJustified(12, ' ');
    out += " DT ";
    // Width matters: the reader treats more than four hex digits as extended,
    // which is how an 11-bit id sent in an extended frame survives the trip.
    out += hexId(frame.id, frame.extended ? 8 : 4);
    out += ' ';
    out += (frame.direction == CanFrame::Tx ? "Tx " : "Rx ");
    out += QByteArray::number(int(frame.dlc)).rightJustified(2, ' ');
    out += "  ";
    appendPayload(out, frame);
    out += '\n';
}

}   // namespace TraceFormat
