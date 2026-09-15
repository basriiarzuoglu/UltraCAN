#pragma once

#include <QStringList>
#include <QUrl>

#include <array>
#include <vector>

#include "core/CanFrame.h"

/*!
    \namespace TraceReader
    \brief Reads a recorded trace file into frames.

    Lifted out of CanPlaybackModel when a SECOND consumer appeared. The Graph
    Plot replays a log into the application; the Sender replays one onto the
    wire. Those two must not share an object - one of them can drive hardware
    and the other must never be able to - but they parse identical files, and a
    second copy of three trace parsers is a second place for a dialect bug to
    be fixed only once.

    So the parsing is here and the policy is in the models. Same split as
    TraceFormat, which does this for the writers.
*/
namespace TraceReader {

/*! Reads \a fileUrl, choosing a reader by extension and then by sniffing.

    \a channelNames maps a CSV channel column back to an index; pass the
    consumer's own list. Returns false and fills \a error on failure; \a
    warnings collects recoverable complaints (unparsed lines, missing header)
    and is worth showing even on success. */
bool readFile(const QUrl &fileUrl, const QStringList &channelNames,
              std::vector<CanFrame> &out, QStringList &warnings, QString &error);

bool readCsv(const QString &text, const QStringList &channelNames,
             std::vector<CanFrame> &out, QStringList &warnings);
bool readTrc(const QString &text, std::vector<CanFrame> &out, QStringList &warnings);
bool readAsc(const QString &text, std::vector<CanFrame> &out, QStringList &warnings);

int parseHexBytes(const QStringList &tokens, int from, std::array<quint8, 64> &out);

}   // namespace TraceReader
