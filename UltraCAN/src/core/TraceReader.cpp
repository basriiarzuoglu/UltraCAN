#include "TraceReader.h"

#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>
#include <QStringDecoder>

namespace {
constexpr qint64 NsPerSecond = 1'000'000'000LL;
}

namespace TraceReader {

bool readFile(const QUrl &fileUrl, const QStringList &channelNames,
              std::vector<CanFrame> &out, QStringList &warnings, QString &error)
{
    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QObject::tr("Cannot open %1: %2").arg(path, file.errorString());
        return false;
    }
    const QByteArray raw = file.readAll();
    file.close();

    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8(raw);
    if (utf8.hasError())
        text = QString::fromLatin1(raw);          // trace tools are rarely UTF-8

    const QString suffix = QFileInfo(path).suffix().toLower();
    bool ok = false;

    // Extension first, then sniff: a .txt from a logger is still a trace.
    if (suffix == QLatin1String("csv")) {
        ok = readCsv(text, channelNames, out, warnings);
    } else if (suffix == QLatin1String("trc")) {
        ok = readTrc(text, out, warnings);
    } else if (suffix == QLatin1String("asc")) {
        ok = readAsc(text, out, warnings);
    } else {
        ok = readCsv(text, channelNames, out, warnings);
        if (!ok || out.empty()) {
            out.clear(); warnings.clear();
            ok = readTrc(text, out, warnings);
        }
        if (!ok || out.empty()) {
            out.clear(); warnings.clear();
            ok = readAsc(text, out, warnings);
        }
    }

    if (!ok || out.empty()) {
        error = QObject::tr("%1 contains no readable frames "
                            "(tried CSV, PCAN .trc and Vector .asc).")
                .arg(QFileInfo(path).fileName());
        return false;
    }
    error.clear();
    return true;
}

int parseHexBytes(const QStringList &tokens, int from,
                                    std::array<quint8, 64> &out)
{
    int written = 0;
    for (int i = from; i < tokens.size() && written < int(out.size()); ++i) {
        bool ok = false;
        const uint value = tokens.at(i).toUInt(&ok, 16);
        if (!ok)
            break;
        out[size_t(written++)] = quint8(value & 0xFF);
    }
    return written;
}

/*! Our own Logger export: Time,Dir,Ch,ID,Type,DLC,Data */
bool readCsv(const QString &text, const QStringList &channelNames,
             std::vector<CanFrame> &out, QStringList &warnings)
{
    static const QRegularExpression lineBreak(QStringLiteral("[\r\n]"));
    const QStringList lines = text.split(lineBreak, Qt::SkipEmptyParts);
    if (lines.isEmpty())
        return false;

    out.reserve(size_t(lines.size()));
    bool sawHeader = false;

    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;
        if (trimmed.startsWith(QLatin1String("Time,"), Qt::CaseInsensitive)) {
            sawHeader = true;
            continue;
        }

        const QStringList fields = trimmed.split(QLatin1Char(','));
        if (fields.size() < 7)
            continue;

        bool timeOk = false;
        const double seconds = fields.at(0).toDouble(&timeOk);
        if (!timeOk)
            continue;

        CanFrame frame;
        frame.timestampNs = qint64(seconds * double(NsPerSecond));
        frame.direction = fields.at(1).trimmed().compare(QLatin1String("Tx"), Qt::CaseInsensitive) == 0
                          ? CanFrame::Tx : CanFrame::Rx;
        frame.channel = quint8(qMax(0, channelNames.indexOf(fields.at(2).trimmed())));

        QString idText = fields.at(3).trimmed();
        idText.remove(QLatin1String("0x"), Qt::CaseInsensitive);
        bool idOk = false;
        frame.id = idText.toUInt(&idOk, 16);
        if (!idOk)
            continue;

        frame.extended = fields.at(4).trimmed().compare(QLatin1String("Ext"), Qt::CaseInsensitive) == 0
                         || frame.id > 0x7FFu;

        static const QRegularExpression spaces(QStringLiteral("\\s+"));
        const QStringList bytes = fields.at(6).trimmed().split(spaces, Qt::SkipEmptyParts);
        const int written = parseHexBytes(bytes, 0, frame.payload);
        frame.dlc = quint8(qBound(0, fields.at(5).trimmed().toInt(), 64));
        if (written < frame.dlc)
            frame.dlc = quint8(written);

        out.push_back(frame);
    }

    if (!sawHeader && !out.empty())
        warnings << QObject::tr("No CSV header found; assumed Time,Dir,Ch,ID,Type,DLC,Data.");
    return !out.empty();
}

/*!
    PCAN .trc, both dialects:
      1.x:  "     1)      1000.0  0300  8  01 02 .."          (ms, no direction)
      2.x:  "     1      1000.000 DT 0300 Rx 8  01 02 .."     (ms, typed)
    Lines starting with ';' are headers/comments.
*/
bool readTrc(const QString &text, std::vector<CanFrame> &out, QStringList &warnings)
{
    static const QRegularExpression lineBreak(QStringLiteral("[\r\n]"));
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    const QStringList lines = text.split(lineBreak, Qt::SkipEmptyParts);
    if (lines.isEmpty())
        return false;

    out.reserve(size_t(lines.size()));
    int skipped = 0;

    for (const QString &line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char(';')))
            continue;

        // The message number may be "1)" or "1"; drop the bracket either way.
        trimmed.replace(QLatin1Char(')'), QLatin1Char(' '));
        const QStringList tokens = trimmed.split(spaces, Qt::SkipEmptyParts);
        if (tokens.size() < 4) {
            ++skipped;
            continue;
        }

        bool timeOk = false;
        const double milliseconds = tokens.at(1).toDouble(&timeOk);
        if (!timeOk) {
            ++skipped;
            continue;
        }

        int cursor = 2;
        CanFrame frame;
        frame.direction = CanFrame::Rx;

        // 2.x carries a type token (DT/FD/ER/ST...) and then a direction.
        static const QStringList typeTokens { QStringLiteral("DT"), QStringLiteral("FD"),
                                              QStringLiteral("RR"), QStringLiteral("ST"),
                                              QStringLiteral("ER"), QStringLiteral("EC") };
        if (cursor < tokens.size() && typeTokens.contains(tokens.at(cursor).toUpper())) {
            if (tokens.at(cursor).compare(QLatin1String("ER"), Qt::CaseInsensitive) == 0
                || tokens.at(cursor).compare(QLatin1String("EC"), Qt::CaseInsensitive) == 0) {
                continue;                        // error/status frames are not traffic
            }
            ++cursor;
        }

        if (cursor >= tokens.size()) { ++skipped; continue; }
        QString idText = tokens.at(cursor++);
        idText.remove(QLatin1String("0x"), Qt::CaseInsensitive);
        bool idOk = false;
        frame.id = idText.toUInt(&idOk, 16);
        if (!idOk) { ++skipped; continue; }
        frame.extended = idText.size() > 4 || frame.id > 0x7FFu;

        if (cursor < tokens.size()) {
            const QString token = tokens.at(cursor);
            if (token.compare(QLatin1String("Rx"), Qt::CaseInsensitive) == 0) {
                ++cursor;
            } else if (token.compare(QLatin1String("Tx"), Qt::CaseInsensitive) == 0) {
                frame.direction = CanFrame::Tx;
                ++cursor;
            }
        }

        if (cursor >= tokens.size()) { ++skipped; continue; }
        bool dlcOk = false;
        const int dlc = tokens.at(cursor++).toInt(&dlcOk);
        if (!dlcOk) { ++skipped; continue; }

        const int written = parseHexBytes(tokens, cursor, frame.payload);
        frame.dlc = quint8(qBound(0, qMin(dlc, written), 64));
        frame.timestampNs = qint64(milliseconds * 1'000'000.0);   // ms -> ns

        out.push_back(frame);
    }

    if (skipped > 0)
        warnings << QObject::tr("%1 .trc lines were not understood and were skipped.").arg(skipped);
    return !out.empty();
}

/*! Vector .asc: "0.123456 1  100  Rx d 8 01 02 .." (seconds). */
bool readAsc(const QString &text, std::vector<CanFrame> &out, QStringList &warnings)
{
    static const QRegularExpression lineBreak(QStringLiteral("[\r\n]"));
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    const QStringList lines = text.split(lineBreak, Qt::SkipEmptyParts);
    if (lines.isEmpty())
        return false;

    out.reserve(size_t(lines.size()));
    int skipped = 0;

    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('/'))
            || trimmed.startsWith(QLatin1String("date"), Qt::CaseInsensitive)
            || trimmed.startsWith(QLatin1String("base"), Qt::CaseInsensitive)
            || trimmed.startsWith(QLatin1String("Begin"), Qt::CaseInsensitive)
            || trimmed.startsWith(QLatin1String("End"), Qt::CaseInsensitive)) {
            continue;
        }

        const QStringList tokens = trimmed.split(spaces, Qt::SkipEmptyParts);
        if (tokens.size() < 6) { ++skipped; continue; }

        bool timeOk = false;
        const double seconds = tokens.at(0).toDouble(&timeOk);
        if (!timeOk) { ++skipped; continue; }

        CanFrame frame;
        frame.timestampNs = qint64(seconds * double(NsPerSecond));
        frame.channel = quint8(qMax(0, tokens.at(1).toInt() - 1));

        // An extended id carries a trailing 'x' in .asc.
        QString idText = tokens.at(2);
        frame.extended = idText.endsWith(QLatin1Char('x'), Qt::CaseInsensitive);
        if (frame.extended)
            idText.chop(1);
        bool idOk = false;
        frame.id = idText.toUInt(&idOk, 16);
        if (!idOk) { ++skipped; continue; }
        frame.extended = frame.extended || frame.id > 0x7FFu;

        int cursor = 3;
        if (cursor < tokens.size()) {
            const QString direction = tokens.at(cursor);
            if (direction.compare(QLatin1String("Tx"), Qt::CaseInsensitive) == 0) {
                frame.direction = CanFrame::Tx;
                ++cursor;
            } else if (direction.compare(QLatin1String("Rx"), Qt::CaseInsensitive) == 0) {
                ++cursor;
            }
        }
        if (cursor < tokens.size() && tokens.at(cursor).compare(QLatin1String("d"),
                                                                Qt::CaseInsensitive) == 0) {
            ++cursor;                      // the "data frame" marker
        }

        if (cursor >= tokens.size()) { ++skipped; continue; }
        bool dlcOk = false;
        const int dlc = tokens.at(cursor++).toInt(&dlcOk);
        if (!dlcOk) { ++skipped; continue; }

        const int written = parseHexBytes(tokens, cursor, frame.payload);
        frame.dlc = quint8(qBound(0, qMin(dlc, written), 64));

        out.push_back(frame);
    }

    if (skipped > 0)
        warnings << QObject::tr("%1 .asc lines were not understood and were skipped.").arg(skipped);
    return !out.empty();
}

}   // namespace TraceReader
