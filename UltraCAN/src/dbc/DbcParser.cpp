#include "DbcParser.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringDecoder>

#include <algorithm>

namespace {

// BO_ 256 EngineData: 8 ECU
const QRegularExpression &messageRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"(^BO_\s+(\d+)\s+([A-Za-z_][\w]*)\s*:\s*(\d+)\s+(\S+))"));
    return re;
}

//  SG_ EngineSpeed : 0|16@1+ (0.125,0) [0|8000] "rpm" Dashboard,VCU
const QRegularExpression &signalRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"(^\s*SG_\s+([A-Za-z_][\w]*)\s*(M|m\d+)?\s*:\s*)"
        R"((\d+)\|(\d+)@([01])([+-])\s*)"
        R"(\(\s*([^,]+)\s*,\s*([^)]+)\s*\)\s*)"
        R"(\[\s*([^|]*)\|([^\]]*)\]\s*)"
        R"RX("([^"]*)"\s*(.*)$)RX"));
    return re;
}

const QRegularExpression &messageCommentRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"RX(^CM_\s+BO_\s+(\d+)\s+"([\s\S]*)"\s*;?\s*$)RX"));
    return re;
}

const QRegularExpression &signalCommentRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"RX(^CM_\s+SG_\s+(\d+)\s+([A-Za-z_][\w]*)\s+"([\s\S]*)"\s*;?\s*$)RX"));
    return re;
}

// BA_ "GenMsgCycleTime" BO_ 256 10;
const QRegularExpression &cycleTimeRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"RX(^BA_\s+"GenMsgCycleTime"\s+BO_\s+(\d+)\s+([\d.]+)\s*;?\s*$)RX"));
    return re;
}

/*!
    VAL_ 2503 GE_Fahrstufe 0 "Zwischenstellung" 1 "P" 2 "R" ;

    The head is matched here; the pairs are pulled out of the tail with
    valuePairRe() below, because a fixed regex cannot capture an unbounded
    number of repeating groups - it only ever keeps the LAST repetition.
*/
const QRegularExpression &valueTableRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"(^VAL_\s+(\d+)\s+([A-Za-z_][\w]*)\s+([\s\S]*)$)"));
    return re;
}

//  0 "Zwischenstellung"    (one entry of a VAL_ tail; negative keys are legal)
const QRegularExpression &valuePairRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"RX((-?\d+)\s+"([^"]*)")RX"));
    return re;
}

const QRegularExpression &nodesRe()
{
    static const QRegularExpression re(QStringLiteral(R"(^BU_\s*:\s*(.*)$)"));
    return re;
}

/*!
    A line inside the NS_ block: a bare keyword and nothing else (CM_, BA_,
    VAL_, SIG_VALTYPE_, FILTER ...). These are declarations, not sections.
*/
const QRegularExpression &bareKeywordRe()
{
    static const QRegularExpression re(QStringLiteral("^[A-Z][A-Z0-9_]*$"));
    return re;
}

/*! A real comment: CM_ followed by whitespace and something. */
const QRegularExpression &commentStartRe()
{
    static const QRegularExpression re(QStringLiteral(R"(^CM_\s+\S)"));
    return re;
}

/*! Start of any known section - used to stop a runaway comment. */
const QRegularExpression &sectionStartRe()
{
    static const QRegularExpression re(QStringLiteral(
        "^(VERSION|NS_|BS_|BU_|BO_|SG_|CM_|BA_|VAL_|EV_|SIG_|SGTYPE_|CAT_|FILTER)"));
    return re;
}

const QRegularExpression &whitespaceRe()
{
    static const QRegularExpression re(QStringLiteral("\\s+"));
    return re;
}

constexpr quint32 ExtendedFlag = 0x80000000u;
constexpr quint32 ExtendedMask = 0x1FFFFFFFu;

DbcMessageData *findMessage(DbcDatabase &db, quint32 rawId)
{
    const bool extended = (rawId & ExtendedFlag) != 0;
    const quint32 id = extended ? (rawId & ExtendedMask) : rawId;
    for (DbcMessageData &message : db.messages)
        if (message.id == id && message.extended == extended)
            return &message;
    return nullptr;
}

/*! .dbc numbers are always C-locale. QString::toDouble() is too - QLocale is NOT. */
double toNumber(const QString &text, double fallback = 0.0)
{
    bool ok = false;
    const double value = text.trimmed().toDouble(&ok);
    return ok ? value : fallback;
}

} // namespace

// ------------------------------------------------------------------ reading

DbcDatabase DbcParser::parseFile(const QString &filePath, QString *errorMessage)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Cannot open %1: %2").arg(filePath, file.errorString());
        return {};
    }

    const QByteArray raw = file.readAll();
    file.close();

    // Most .dbc files in the wild are Windows-1252, some are UTF-8. Try UTF-8
    // and fall back, rather than mangling unit strings like "degC".
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8(raw);
    if (utf8.hasError())
        text = QString::fromLatin1(raw);

    return parseText(text, QFileInfo(filePath).fileName());
}

DbcDatabase DbcParser::parseText(const QString &text, const QString &databaseName)
{
    DbcDatabase database;
    database.name = databaseName;

    // Index of the message currently being filled with SG_ lines. An index and
    // not a pointer: appending to the vector may reallocate.
    int currentMessage = -1;

    static const QRegularExpression lineBreak(QStringLiteral("[\r\n]"));
    const QStringList lines = text.split(lineBreak, Qt::SkipEmptyParts);

    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;

        // ---- the NS_ declaration block
        //
        // NS_ lists bare keywords, one per line, including "CM_". Trimmed, that
        // line looks exactly like the start of a comment, and the multi-line
        // comment reader would then swallow everything up to the first line
        // ending in ";" - i.e. every BO_ and SG_ in the file. Skip the block.
        if (line.startsWith(QLatin1String("NS_"))) {
            while (i + 1 < lines.size()
                   && bareKeywordRe().match(lines.at(i + 1).trimmed()).hasMatch()) {
                ++i;
            }
            currentMessage = -1;
            continue;
        }

        // ---- nodes
        if (line.startsWith(QLatin1String("BU_"))) {
            const auto match = nodesRe().match(line);
            if (match.hasMatch())
                database.nodes = match.captured(1).split(whitespaceRe(), Qt::SkipEmptyParts);
            currentMessage = -1;
            continue;
        }

        // ---- message
        if (line.startsWith(QLatin1String("BO_"))) {
            const auto match = messageRe().match(line);
            if (!match.hasMatch()) {
                database.warnings << QStringLiteral("Unparsed BO_: %1").arg(line);
                currentMessage = -1;
                continue;
            }

            // .dbc ids are unsigned 32-bit with bit 31 as the extended flag,
            // so they exceed INT_MAX routinely (2596055714 in this file).
            // Parse as 64-bit and range-check rather than trusting the width.
            bool idOk = false;
            const qulonglong rawId64 = match.captured(1).toULongLong(&idOk);
            if (!idOk || rawId64 > 0xFFFFFFFFull) {
                database.warnings << QStringLiteral("Out-of-range message id: %1").arg(line);
                currentMessage = -1;
                continue;
            }
            const quint32 rawId = quint32(rawId64);

            DbcMessageData message;
            message.extended = (rawId & ExtendedFlag) != 0;
            message.id = message.extended ? (rawId & ExtendedMask) : rawId;
            message.name = match.captured(2);
            message.dlc = match.captured(3).toInt();
            message.transmitter = match.captured(4);
            if (message.transmitter == QLatin1String("Vector__XXX"))
                message.transmitter.clear();

            database.messages.append(message);
            currentMessage = database.messages.size() - 1;
            continue;
        }

        // ---- signal (only valid inside a message block)
        if (line.startsWith(QLatin1String("SG_"))) {
            if (currentMessage < 0) {
                database.warnings << QStringLiteral("SG_ outside a message: %1").arg(line);
                continue;
            }
            const auto match = signalRe().match(line);
            if (!match.hasMatch()) {
                database.warnings << QStringLiteral("Unparsed SG_: %1").arg(line);
                continue;
            }

            DbcSignalData signalData;
            signalData.name = match.captured(1);
            signalData.multiplexer = match.captured(2);
            signalData.startBit = match.captured(3).toInt();
            signalData.bitLength = match.captured(4).toInt();
            signalData.byteOrder = match.captured(5).toInt();          // 0 = BE, 1 = LE
            signalData.valueType = match.captured(6) == QLatin1String("-") ? 1 : 0;
            signalData.factor = toNumber(match.captured(7), 1.0);
            signalData.offset = toNumber(match.captured(8), 0.0);
            signalData.minimum = toNumber(match.captured(9), 0.0);
            signalData.maximum = toNumber(match.captured(10), 0.0);
            signalData.unit = match.captured(11);

            static const QRegularExpression receiverSplit(QStringLiteral("[\\s,]+"));
            signalData.receivers = match.captured(12).split(receiverSplit, Qt::SkipEmptyParts);
            signalData.receivers.removeAll(QStringLiteral("Vector__XXX"));

            database.messages[currentMessage].signalList.append(signalData);
            continue;
        }

        // Anything below here ends the message block.
        currentMessage = -1;

        // ---- comments (may span several lines until the closing ";")
        if (line.startsWith(QLatin1String("CM_"))) {
            if (!commentStartRe().match(line).hasMatch())
                continue;                       // a bare "CM_" keyword, not a comment

            while (!line.endsWith(QLatin1Char(';')) && i + 1 < lines.size()) {
                // Belt and braces: a comment that is missing its ";" must not
                // eat the next section.
                const QString peek = lines.at(i + 1).trimmed();
                if (sectionStartRe().match(peek).hasMatch()) {
                    database.warnings << QStringLiteral("Unterminated CM_: %1").arg(line);
                    break;
                }
                ++i;
                line += QLatin1Char('\n') + peek;
            }

            auto match = signalCommentRe().match(line);
            if (match.hasMatch()) {
                if (DbcMessageData *message = findMessage(database, quint32(match.captured(1).toULongLong()))) {
                    const QString signalName = match.captured(2);
                    for (DbcSignalData &signalData : message->signalList) {
                        if (signalData.name == signalName) {
                            signalData.comment = match.captured(3);
                            break;
                        }
                    }
                }
                continue;
            }

            match = messageCommentRe().match(line);
            if (match.hasMatch()) {
                if (DbcMessageData *message = findMessage(database, quint32(match.captured(1).toULongLong())))
                    message->comment = match.captured(2);
            }
            continue;
        }

        // ---- cycle time attribute
        if (line.startsWith(QLatin1String("BA_ \"GenMsgCycleTime\""))) {
            const auto match = cycleTimeRe().match(line);
            if (match.hasMatch()) {
                if (DbcMessageData *message = findMessage(database, quint32(match.captured(1).toULongLong())))
                    message->cycleMs = int(toNumber(match.captured(2)));
            }
            continue;
        }

        // ---- value description tables
        //
        // A VAL_ line may legally wrap, so it is joined up to its ";" exactly
        // the way CM_ is - with the same guard against an unterminated line
        // swallowing the next section.
        if (line.startsWith(QLatin1String("VAL_ "))) {
            while (!line.endsWith(QLatin1Char(';')) && i + 1 < lines.size()) {
                const QString peek = lines.at(i + 1).trimmed();
                if (sectionStartRe().match(peek).hasMatch()) {
                    database.warnings << QStringLiteral("Unterminated VAL_: %1").arg(line);
                    break;
                }
                ++i;
                line += QLatin1Char(' ') + peek;
            }

            const auto match = valueTableRe().match(line);
            if (!match.hasMatch()) {
                database.warnings << QStringLiteral("Unparsed VAL_: %1").arg(line);
                continue;
            }

            DbcMessageData *message =
                    findMessage(database, quint32(match.captured(1).toULongLong()));
            if (!message)
                continue;                       // a table for a message we skipped

            const QString signalName = match.captured(2);
            auto target = std::find_if(message->signalList.begin(),
                                       message->signalList.end(),
                                       [&signalName](const DbcSignalData &s) {
                                           return s.name == signalName;
                                       });
            if (target == message->signalList.end())
                continue;

            auto pairs = valuePairRe().globalMatch(match.captured(3));
            while (pairs.hasNext()) {
                const auto pair = pairs.next();
                target->valueDescriptions.insert(pair.captured(1).toInt(), pair.captured(2));
            }
            continue;
        }

        // Everything else (VERSION, NS_, BS_, BA_DEF_, SIG_VALTYPE_, ...) is
        // intentionally ignored.
    }

    return database;
}

// ------------------------------------------------------------------ writing

QString DbcParser::serialize(const DbcDatabase &database)
{
    QString out;
    out.reserve(4096);

    out += QStringLiteral("VERSION \"\"\n\n\n");
    out += QStringLiteral("NS_ :\n\n");
    out += QStringLiteral("BS_:\n\n");

    QStringList nodes = database.nodes;
    if (nodes.isEmpty()) {
        for (const DbcMessageData &message : database.messages) {
            if (!message.transmitter.isEmpty() && !nodes.contains(message.transmitter))
                nodes << message.transmitter;
            for (const DbcSignalData &signalData : message.signalList)
                for (const QString &receiver : signalData.receivers)
                    if (!nodes.contains(receiver))
                        nodes << receiver;
        }
    }
    out += QStringLiteral("BU_: %1\n\n").arg(nodes.join(QLatin1Char(' ')));

    for (const DbcMessageData &message : database.messages) {
        const quint32 rawId = message.extended ? (message.id | ExtendedFlag) : message.id;
        out += QStringLiteral("BO_ %1 %2: %3 %4\n")
               .arg(rawId)
               .arg(message.name)
               .arg(message.dlc)
               .arg(message.transmitter.isEmpty() ? QStringLiteral("Vector__XXX")
                                                  : message.transmitter);

        for (const DbcSignalData &s : message.signalList) {
            const QString receivers = s.receivers.isEmpty()
                                      ? QStringLiteral("Vector__XXX")
                                      : s.receivers.join(QLatin1Char(','));
            out += QStringLiteral(" SG_ %1%2 : %3|%4@%5%6 (%7,%8) [%9|%10] \"%11\" %12\n")
                   .arg(s.name)
                   .arg(s.multiplexer.isEmpty() ? QString() : QLatin1Char(' ') + s.multiplexer)
                   .arg(s.startBit)
                   .arg(s.bitLength)
                   .arg(s.byteOrder)
                   .arg(s.valueType == 1 ? QLatin1Char('-') : QLatin1Char('+'))
                   .arg(s.factor, 0, 'g', 10)
                   .arg(s.offset, 0, 'g', 10)
                   .arg(s.minimum, 0, 'g', 10)
                   .arg(s.maximum, 0, 'g', 10)
                   .arg(s.unit)
                   .arg(receivers);
        }
        out += QLatin1Char('\n');
    }

    out += QLatin1Char('\n');

    for (const DbcMessageData &message : database.messages) {
        const quint32 rawId = message.extended ? (message.id | ExtendedFlag) : message.id;
        if (!message.comment.isEmpty())
            out += QStringLiteral("CM_ BO_ %1 \"%2\";\n").arg(rawId).arg(message.comment);
        for (const DbcSignalData &s : message.signalList) {
            if (!s.comment.isEmpty())
                out += QStringLiteral("CM_ SG_ %1 %2 \"%3\";\n")
                       .arg(rawId).arg(s.name).arg(s.comment);
        }
    }

    // VAL_ goes after CM_ and before the attribute block, which is where every
    // tool this project has been tested against puts it.
    for (const DbcMessageData &message : database.messages) {
        const quint32 rawId = message.extended ? (message.id | ExtendedFlag) : message.id;
        for (const DbcSignalData &s : message.signalList) {
            if (s.valueDescriptions.isEmpty())
                continue;
            QString line = QStringLiteral("VAL_ %1 %2").arg(rawId).arg(s.name);
            // QMap iterates in key order, so the file comes out sorted and a
            // save/load/save cycle is byte-stable - which is what makes a .dbc
            // diffable in version control.
            for (auto it = s.valueDescriptions.constBegin();
                 it != s.valueDescriptions.constEnd(); ++it) {
                // .dbc has no escape for a quote inside a quoted string, so a
                // description containing one would truncate the entry and make
                // every following pair on the line unparseable. Drop it: a
                // slightly wrong label beats a file that will not reload.
                QString text = it.value();
                text.remove(QLatin1Char('"'));
                line += QStringLiteral(" %1 \"%2\"").arg(it.key()).arg(text);
            }
            out += line + QStringLiteral(" ;\n");
        }
    }

    out += QStringLiteral("BA_DEF_ BO_ \"GenMsgCycleTime\" INT 0 65535;\n");
    out += QStringLiteral("BA_DEF_DEF_ \"GenMsgCycleTime\" 0;\n");
    for (const DbcMessageData &message : database.messages) {
        if (message.cycleMs <= 0)
            continue;
        const quint32 rawId = message.extended ? (message.id | ExtendedFlag) : message.id;
        out += QStringLiteral("BA_ \"GenMsgCycleTime\" BO_ %1 %2;\n").arg(rawId).arg(message.cycleMs);
    }

    return out;
}
