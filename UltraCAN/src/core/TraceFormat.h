#pragma once

#include <QByteArray>
#include <QStringList>

#include "core/CanFrame.h"

/*!
    \namespace TraceFormat
    \brief The three on-disk trace formats, in one place.

    \section1 Why this is a header and not private to LogExporter

    These are the formats \c CanPlaybackModel reads back, so a trace this
    application records is a trace it can replay - and there are now TWO
    writers: \c LogExporter, which formats a finished snapshot, and
    \c TraceStreamWriter, which appends frame by frame while the bus runs.

    A second copy of the format rules would drift from the first, silently and
    in exactly the way that is hardest to notice: a file that opens, parses,
    and is subtly wrong. The 11-bit-id-in-an-extended-frame case alone depends
    on three separate details agreeing - the \c x suffix in .asc, the 8-digit
    field in .trc, and the Std/Ext column in CSV. So the rules live here and
    both writers call them.

    Anything changed here needs the matching reader in \c CanPlaybackModel
    changed with it.
*/
namespace TraceFormat {

/*! Flush the accumulator once it passes this, instead of building the whole
    file in RAM. A 500k-frame .asc is tens of megabytes of text; there is no
    reason for all of it to exist at once. */
inline constexpr int ChunkFlushBytes = 512 * 1024;

void appendPayload(QByteArray &out, const CanFrame &frame);

/*! Our own CSV. Identical to CanLogModel::toCsv() and to what
    CanPlaybackModel::readCsv() expects, header included. */
void appendCsvHeader(QByteArray &out);
void appendCsvFrame(QByteArray &out, const CanFrame &frame, const QStringList &channelNames);

/*! Vector .asc. The header lines are the ones real tools look for; our reader
    skips anything starting with date/base/Begin/End or "//". */
void appendAscHeader(QByteArray &out);
void appendAscFrame(QByteArray &out, const CanFrame &frame);
void appendAscFooter(QByteArray &out);

/*! PCAN .trc, 2.0 dialect. Every header line starts with ';' and is skipped on
    the way back in. Times are milliseconds here, not seconds.

    \a number is the running message counter, 1-based and continuous across
    the whole file - which is why the streaming writer has to carry it between
    batches rather than restarting it per write. */
void appendTrcHeader(QByteArray &out);
void appendTrcFrame(QByteArray &out, const CanFrame &frame, int number);

}   // namespace TraceFormat
