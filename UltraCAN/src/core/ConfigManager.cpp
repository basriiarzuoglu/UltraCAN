#include "ConfigManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringDecoder>

#include <utility>

#include "models/CanAnalyzerModel.h"
#include "models/DbcModel.h"
#include "models/GraphDataModel.h"
#include "models/CanLogModel.h"
#include "models/MathChannelManager.h"

namespace {

constexpr auto FormatTag = "ultracan-config";

// Section and field names, in one place. Typos in a string literal used twice
// are the classic way for a writer and a reader to disagree silently.
constexpr auto KeyFormat      = "format";
constexpr auto KeyVersion     = "version";
constexpr auto KeyApp         = "application";
constexpr auto KeyDatabases   = "databases";
constexpr auto KeyEditor      = "editor";
constexpr auto KeyGraph       = "graph";
constexpr auto KeyAnalyzer    = "analyzer";
constexpr auto KeyFiles       = "files";
constexpr auto KeyWorkspace   = "workspace";
constexpr auto KeyMathChannels = "math_channels";
constexpr auto KeyLogger      = "logger";
constexpr auto KeyEmbedded    = "embedded_files";
constexpr auto KeySources     = "sources";
constexpr auto KeyEncoding    = "encoding";
constexpr auto KeyData        = "data";
constexpr auto EncodingBase64 = "base64";
constexpr auto KeyAnalysisType = "analysisType";
constexpr auto KeyByteOrder   = "byteOrder";
constexpr auto KeyAutoSort    = "autoSort";
constexpr auto KeySortColumn  = "sortColumn";
constexpr auto KeySortDescending = "sortDescending";

} // namespace

ConfigManager::ConfigManager(QObject *parent)
    : QObject(parent)
{
}

void ConfigManager::setDbcModel(DbcModel *model)      { m_dbcModel = model; }
void ConfigManager::setGraphModel(GraphDataModel *m)  { m_graphModel = m; }
void ConfigManager::setAnalyzerModel(CanAnalyzerModel *m) { m_analyzerModel = m; }
void ConfigManager::setMathChannels(MathChannelManager *m) { m_mathChannels = m; }
void ConfigManager::setLogModel(CanLogModel *m) { m_logModel = m; }

// ------------------------------------------------------------------ plumbing

void ConfigManager::setLastError(const QString &message)
{
    if (m_lastError == message)
        return;
    m_lastError = message;
    emit lastErrorChanged();
}

void ConfigManager::setLastMessage(const QString &message)
{
    if (m_lastMessage == message)
        return;
    m_lastMessage = message;
    emit lastMessageChanged();
}

void ConfigManager::noteMissing(const QString &path)
{
    if (path.isEmpty() || m_missingFiles.contains(path))
        return;
    m_missingFiles.append(path);
    emit missingFilesChanged();
}

QString ConfigManager::localPathOf(const QUrl &url)
{
    if (url.isLocalFile())
        return QFileInfo(url.toLocalFile()).absoluteFilePath();
    // A relative or bare path arrives as a QUrl with no scheme; treat it as a
    // path rather than dropping it.
    if (url.scheme().isEmpty() && !url.path().isEmpty())
        return QFileInfo(url.path()).absoluteFilePath();
    return {};
}

QString ConfigManager::safeFileName(const QString &key)
{
    // QFileInfo::fileName() alone is not enough: it keeps a Windows drive
    // prefix ("C:evil.dbc") and does nothing about a bare "..". Take the last
    // component of either separator, then scrub what is left.
    QString name = key;
    const int slash = qMax(name.lastIndexOf(QLatin1Char('/')),
                           name.lastIndexOf(QLatin1Char('\\')));
    if (slash >= 0)
        name = name.mid(slash + 1);
    name.remove(QLatin1Char(':'));
    while (name.startsWith(QLatin1Char('.')))
        name.remove(0, 1);
    name = name.trimmed();
    return name;
}

QJsonValue ConfigManager::encodeFile(const QString &absPath)
{
    QFile file(absPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};                                  // null: caller falls back
    const QByteArray raw = file.readAll();

    // Bytes, not text. DbcParser falls back to Latin-1 precisely because most
    // .dbc files in the wild are Windows-1252, and pushing those through a
    // JSON string as UTF-8 replaces every high byte with U+FFFD - the unit
    // "°C" comes back as garbage and the corruption is invisible until
    // somebody reads the axis label. So: plain string when it really is UTF-8,
    // base64 otherwise, and the file that comes out is the file that went in.
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString text = utf8(raw);
    if (!utf8.hasError())
        return text;

    QJsonObject wrapped;
    wrapped[QLatin1String(KeyEncoding)] = QLatin1String(EncodingBase64);
    wrapped[QLatin1String(KeyData)] = QString::fromLatin1(raw.toBase64());
    return wrapped;
}

bool ConfigManager::decodeFile(const QJsonValue &value, QByteArray *out)
{
    if (!out)
        return false;
    if (value.isString()) {
        *out = value.toString().toUtf8();
        return true;
    }
    if (value.isObject()) {
        const QJsonObject wrapped = value.toObject();
        if (wrapped.value(QLatin1String(KeyEncoding)).toString() != QLatin1String(EncodingBase64))
            return false;
        const QByteArray decoded = QByteArray::fromBase64Encoding(
            wrapped.value(QLatin1String(KeyData)).toString().toLatin1())
            .decoded;
        *out = decoded;
        return true;
    }
    return false;
}

QString ConfigManager::sessionDirFor(const QString &configPath)
{
    // AppDataLocation, not QDir::tempPath(). The extracted files become
    // DbcModel::sourceUrl, which SettingsManager persists and saveDbc() writes
    // back to - a temp path would be remembered into a launch where the file
    // no longer exists, and any Save in between would have gone somewhere the
    // OS may delete without warning.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        return {};

    // One folder per config, so importing two sessions in a row does not leave
    // the first one's databases lying next to the second one's.
    QString stem = QFileInfo(configPath).completeBaseName();
    stem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    if (stem.isEmpty())
        stem = QStringLiteral("session");

    QDir dir(base + QStringLiteral("/sessions/") + stem);
    if (!dir.exists() && !QDir().mkpath(dir.absolutePath()))
        return {};

    // Stale extractions from a previous import of the SAME config would
    // otherwise survive as files nothing references any more.
    const QStringList stale = dir.entryList({ QStringLiteral("*.dbc") }, QDir::Files);
    for (const QString &name : stale)
        dir.remove(name);

    return dir.absolutePath();
}

void ConfigManager::setGraphState(const QVariantMap &state)
{
    m_graphState = state;
}

QVariantMap ConfigManager::graphState() const
{
    return m_graphState;
}

// -------------------------------------------------------------------- export

QJsonObject ConfigManager::buildDocument()
{
    QJsonObject root;
    root[QLatin1String(KeyFormat)] = QLatin1String(FormatTag);
    root[QLatin1String(KeyVersion)] = FormatVersion;
    root[QLatin1String(KeyApp)] = QCoreApplication::applicationVersion();

    // -- databases, carried WHOLE -----------------------------------------
    //
    // Paths were the original design and they were wrong for the one job this
    // format exists to do. A session mailed to a colleague named
    // C:/Users/basri/... and arrived on a machine where that folder does not
    // exist, so every reference resolved to nothing and the workspace came
    // back empty. The files travel inside the document now.
    //
    // `sources` keeps the origin path anyway - not as a fallback for a broken
    // reference, but so that an import on the machine that WROTE the config
    // can keep using the real file. See applyDocument().
    QJsonObject databases;
    QJsonObject embedded;
    QJsonObject sources;
    QHash<QString, QString> keyByPath;      // absolute path -> embedded_files key

    // Returns the reference to write into editor/graph/analyzer: normally the
    // embedded_files key, or the absolute path when the file could not be read
    // (the reader treats an unknown reference as a path, which is also how it
    // reads a version 1 document).
    const auto reference = [&](const QString &rawPath) -> QString {
        if (rawPath.isEmpty())
            return {};
        const QString absolute = QFileInfo(rawPath).absoluteFilePath();

        const auto known = keyByPath.constFind(absolute);
        if (known != keyByPath.constEnd())
            return known.value();               // same file twice = one payload

        const QJsonValue payload = encodeFile(absolute);
        if (payload.isNull() || payload.isUndefined()) {
            // Loaded earlier in the session, gone from disk now. Reference the
            // path and let the far end report it missing, rather than dropping
            // the database from the config without a word.
            noteMissing(absolute);
            return absolute;
        }

        // Two different files can share a basename - the same DBC under two
        // project folders is the everyday case. The key is the filename where
        // it can be, and the filename with a counter where it cannot; either
        // way it stays a filename, because that is what gets written to disk
        // on the far end.
        QString key = QFileInfo(absolute).fileName();
        if (embedded.contains(key)) {
            const QString stem = QFileInfo(key).completeBaseName();
            const QString suffix = QFileInfo(key).suffix();
            for (int n = 2; embedded.contains(key); ++n) {
                key = suffix.isEmpty() ? QStringLiteral("%1-%2").arg(stem).arg(n)
                                       : QStringLiteral("%1-%2.%3").arg(stem).arg(n).arg(suffix);
            }
        }

        embedded[key] = payload;
        sources[key] = absolute;
        keyByPath.insert(absolute, key);
        return key;
    };

    if (m_dbcModel) {
        const QString ref = reference(localPathOf(m_dbcModel->sourceUrl()));
        if (!ref.isEmpty())
            databases[QLatin1String(KeyEditor)] = ref;
    }

    const auto referenceArray = [&](const QVariantList &loaded) {
        QJsonArray out;
        for (const QVariant &entry : loaded) {
            const QString ref = reference(entry.toMap().value(QStringLiteral("path")).toString());
            if (!ref.isEmpty())
                out.append(ref);
        }
        return out;
    };

    if (m_graphModel)
        databases[QLatin1String(KeyGraph)] = referenceArray(m_graphModel->loadedDbcs());
    if (m_analyzerModel)
        databases[QLatin1String(KeyAnalyzer)] = referenceArray(m_analyzerModel->loadedDbcs());

    databases[QLatin1String(KeyEmbedded)] = embedded;
    databases[QLatin1String(KeySources)] = sources;
    root[QLatin1String(KeyDatabases)] = databases;

    // -- the analyzer's top bar --------------------------------------------
    if (m_analyzerModel) {
        QJsonObject analyzer;
        analyzer[QLatin1String(KeyAnalysisType)] = m_analyzerModel->analysisType();
        // The control is labelled "Data direction" in the UI and byteOrder in
        // the model; same thing, and the model's name is the one that has to
        // match the property this reads back into.
        analyzer[QLatin1String(KeyByteOrder)] = m_analyzerModel->byteOrder();
        analyzer[QLatin1String(KeyAutoSort)] = m_analyzerModel->autoSort();
        analyzer[QLatin1String(KeySortColumn)] = m_analyzerModel->sortColumn();
        analyzer[QLatin1String(KeySortDescending)] = m_analyzerModel->sortDescending();
        root[QLatin1String(KeyAnalyzer)] = analyzer;
    }

    // -- math channels ------------------------------------------------------
    //
    // Written BEFORE the workspace in the document for the same reason they
    // are read before it: the workspace refers to them by name, and a file
    // reads better when the thing being referred to comes first.
    if (m_mathChannels)
        root[QLatin1String(KeyMathChannels)] = m_mathChannels->toJson();

    // -- the logger's filter and triggers -----------------------------------
    //
    // One block, not two. They are configured in one dialog and they only make
    // sense read together - a filter that excludes the trigger's id is a
    // contradiction, and splitting them across the file would let a partial
    // import produce exactly that.
    if (m_logModel) {
        const QVariantMap triggers = m_logModel->triggerConfig();
        const auto condition = [](const QVariant &raw) {
            const QVariantMap map = raw.toMap();
            return QJsonObject {
                { QStringLiteral("kind"),        map.value(QStringLiteral("kind")).toInt() },
                // Written as a NUMBER, not "0x123": JSON has no hex literal, and
                // a string here would make every reader parse a format this file
                // does not otherwise use.
                { QStringLiteral("frameId"),     double(map.value(QStringLiteral("frameId")).toUInt()) },
                { QStringLiteral("extended"),    map.value(QStringLiteral("extended")).toBool() },
                { QStringLiteral("messageName"), map.value(QStringLiteral("messageName")).toString() },
                { QStringLiteral("signalName"),  map.value(QStringLiteral("signalName")).toString() },
                { QStringLiteral("comparison"),  map.value(QStringLiteral("comparison")).toInt() },
                { QStringLiteral("threshold"),   map.value(QStringLiteral("threshold")).toDouble() }
            };
        };

        const QVariantMap filter = m_logModel->logFilter();
        QJsonObject filterJson;
        for (auto it = filter.constBegin(); it != filter.constEnd(); ++it) {
            filterJson[it.key()] = it.key() == QStringLiteral("startId")
                                || it.key() == QStringLiteral("endId")
                                 ? QJsonValue(double(it.value().toUInt()))
                                 : QJsonValue(it.value().toInt());
        }

        root[QLatin1String(KeyLogger)] = QJsonObject {
            { QStringLiteral("preTriggerSeconds"),
              triggers.value(QStringLiteral("preTriggerSeconds")).toDouble() },
            { QStringLiteral("startTrigger"), condition(triggers.value(QStringLiteral("start"))) },
            { QStringLiteral("stopTrigger"),  condition(triggers.value(QStringLiteral("stop"))) },
            { QStringLiteral("filter"),       filterJson }
        };
    }

    // -- the graph workspace, exactly as QML handed it over -----------------
    root[QLatin1String(KeyWorkspace)] = QJsonObject::fromVariantMap(m_graphState);

    return root;
}

bool ConfigManager::exportConfig(const QUrl &fileUrl)
{
    const QString path = localPathOf(fileUrl);
    if (path.isEmpty()) {
        setLastError(tr("“%1” is not a local file.").arg(fileUrl.toString()));
        setLastMessage(m_lastError);
        return false;
    }

    if (!m_missingFiles.isEmpty()) {
        m_missingFiles.clear();
        emit missingFilesChanged();
    }

    // Synchronous by construction: a direct-connection signal runs its slots
    // inside emit, so the QML handler's setGraphState() has already returned
    // before the next line executes. If nothing is connected, m_graphState
    // keeps whatever it last held - which is empty on a fresh launch, and the
    // file simply carries no workspace.
    emit graphStateRequested();

    const QJsonDocument document(buildDocument());

    // QSaveFile, not QFile: a config overwritten by a process that dies
    // mid-write is worse than no config at all, because the user does not find
    // out until the day they try to import it.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setLastError(tr("Could not write %1: %2").arg(QFileInfo(path).fileName(), file.errorString()));
        setLastMessage(m_lastError);
        return false;
    }
    file.write(document.toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        setLastError(tr("Could not finish writing %1: %2")
                     .arg(QFileInfo(path).fileName(), file.errorString()));
        setLastMessage(m_lastError);
        return false;
    }

    setLastError(QString());
    if (m_missingFiles.isEmpty()) {
        setLastMessage(tr("Session saved to %1 - databases included.")
                       .arg(QFileInfo(path).fileName()));
    } else {
        // Loaded at some point, unreadable now. The config is still written -
        // the layout is worth keeping - but the user has to know that this one
        // will not open anywhere else.
        setLastMessage(tr("Session saved to %1 - %n database(s) could not be read and were "
                          "referenced by path instead.", nullptr, int(m_missingFiles.size()))
                       .arg(QFileInfo(path).fileName()));
        for (const QString &missing : std::as_const(m_missingFiles))
            qWarning("Config export: could not embed %s", qPrintable(missing));
    }
    qInfo("Config export: %s", qPrintable(path));
    return true;
}

// -------------------------------------------------------------------- import

bool ConfigManager::applyDocument(const QJsonObject &root, const QString &configPath)
{
    const QString format = root.value(QLatin1String(KeyFormat)).toString();
    if (format != QLatin1String(FormatTag)) {
        setLastError(tr("That file is not an UltraCAN session."));
        return false;
    }
    const int version = root.value(QLatin1String(KeyVersion)).toInt();
    if (version > FormatVersion) {
        // Forward compatibility is a promise this format has not made, so
        // refuse rather than half-apply a newer file and lose the rest.
        setLastError(tr("That session was written by a newer version (format %1, this build reads %2).")
                     .arg(version).arg(FormatVersion));
        return false;
    }

    // ORDER IS THE WHOLE POINT of this function.
    //
    //   1. databases, because every later step names things that live in them;
    //   2. the analyzer's controls;
    //   3. the workspace, handed to QML LAST - it resolves signals by name
    //      against the databases loaded in step 1, so running it earlier finds
    //      nothing and silently restores empty panes.

    const QJsonObject databases = root.value(QLatin1String(KeyDatabases)).toObject();
    const QJsonObject embedded = databases.value(QLatin1String(KeyEmbedded)).toObject();
    const QJsonObject sources = databases.value(QLatin1String(KeySources)).toObject();

    // Extracted lazily: a document that embeds nothing (version 1, or a
    // hand-written one) must not litter app data with an empty folder.
    QString sessionDir;
    QHash<QString, QString> extracted;      // embedded_files key -> path on disk

    /*!
        Turns one reference into a path on this machine.

        Three cases, in this order:
          1. the reference is not an embedded_files key  -> it IS a path. That
             is a version 1 document, or a file whose contents could not be
             read at export time.
          2. sources says where it came from and that file is still there ->
             use the original. On the machine that wrote the config this is
             every file, which is what keeps the editor pointing at the real
             document instead of a copy: a Save then edits the user's file
             rather than something under app data. (If a DIFFERENT file now
             sits at that path, this picks it - deliberately: the path is the
             user's own, and preferring a months-old embedded copy over the
             file they are working in would be the greater surprise.)
          3. otherwise extract the payload and use that.
    */
    const auto resolve = [&](const QString &reference) -> QString {
        if (reference.isEmpty())
            return {};
        if (!embedded.contains(reference))
            return QFileInfo(reference).absoluteFilePath();

        const auto already = extracted.constFind(reference);
        if (already != extracted.constEnd())
            return already.value();

        const QString origin = sources.value(reference).toString();
        if (!origin.isEmpty() && QFileInfo::exists(origin)) {
            const QString absolute = QFileInfo(origin).absoluteFilePath();
            extracted.insert(reference, absolute);
            return absolute;
        }

        QByteArray payload;
        if (!decodeFile(embedded.value(reference), &payload)) {
            noteMissing(reference);
            extracted.insert(reference, QString());
            return {};
        }

        if (sessionDir.isEmpty()) {
            sessionDir = sessionDirFor(configPath);
            if (sessionDir.isEmpty()) {
                noteMissing(reference);
                return {};
            }
        }

        const QString name = safeFileName(reference);
        if (name.isEmpty()) {
            noteMissing(reference);
            extracted.insert(reference, QString());
            return {};
        }

        const QString target = sessionDir + QLatin1Char('/') + name;
        QSaveFile out(target);
        if (!out.open(QIODevice::WriteOnly) || out.write(payload) != payload.size()
            || !out.commit()) {
            noteMissing(reference);
            extracted.insert(reference, QString());
            return {};
        }

        extracted.insert(reference, target);
        return target;
    };

    const auto load = [this](const QString &path, auto &&loader) {
        if (path.isEmpty())
            return;                              // resolve() already reported it
        if (!QFileInfo::exists(path)) {
            noteMissing(path);
            return;
        }
        if (!loader(QUrl::fromLocalFile(path)))
            noteMissing(path);
    };

    if (m_dbcModel) {
        const QString editorPath = resolve(databases.value(QLatin1String(KeyEditor)).toString());
        load(editorPath, [this](const QUrl &url) { return m_dbcModel->loadDbc(url); });
    }

    if (m_graphModel) {
        // Replace, do not merge. Importing a session means "show me what that
        // file describes", and leaving whatever was already open would give a
        // database list that matches neither the file nor the previous state.
        m_graphModel->clearGraphDbcs();
        const QJsonArray files = databases.value(QLatin1String(KeyGraph)).toArray();
        for (const QJsonValue &value : files)
            load(resolve(value.toString()),
                 [this](const QUrl &url) { return m_graphModel->loadGraphDbc(url); });
    }

    if (m_analyzerModel) {
        m_analyzerModel->clearAnalyzerDbcs();
        const QJsonArray files = databases.value(QLatin1String(KeyAnalyzer)).toArray();
        for (const QJsonValue &value : files)
            load(resolve(value.toString()),
                 [this](const QUrl &url) { return m_analyzerModel->loadAnalyzerDbc(url); });

        const QJsonObject analyzer = root.value(QLatin1String(KeyAnalyzer)).toObject();
        if (!analyzer.isEmpty()) {
            m_analyzerModel->setAnalysisType(analyzer.value(QLatin1String(KeyAnalysisType))
                                             .toInt(m_analyzerModel->analysisType()));
            m_analyzerModel->setByteOrder(analyzer.value(QLatin1String(KeyByteOrder))
                                          .toInt(m_analyzerModel->byteOrder()));
            // sortBy() BEFORE setAutoSort(): setAutoSort(true) rewrites the
            // sort column to "by change" when it finds the id sort still in
            // place, so setting the flag first would throw away the column we
            // are about to restore.
            m_analyzerModel->sortBy(analyzer.value(QLatin1String(KeySortColumn))
                                    .toInt(m_analyzerModel->sortColumn()),
                                    analyzer.value(QLatin1String(KeySortDescending))
                                    .toBool(m_analyzerModel->sortDescending()));
            m_analyzerModel->setAutoSort(analyzer.value(QLatin1String(KeyAutoSort))
                                         .toBool(m_analyzerModel->autoSort()));
        }
    }

    // -- math channels, strictly here --------------------------------------
    //
    // AFTER the databases: a channel binds to a signal by dbc/message/signal
    // NAME, and GraphDataModel resolves those against the databases loaded
    // above. Restoring channels first would build every one of them against an
    // empty database list - they would compile (the formula does not need the
    // signals to exist) and then never fire, which is the worst of the three
    // outcomes because nothing on screen would look wrong.
    //
    // BEFORE m_graphState is handed to QML: the workspace names its virtual
    // traces by channel name, and channelIdFor() has to find them. QML rebuilds
    // the panes on graphStateRestored(), which importConfig() emits after this
    // function returns - so "before the panes" means "before we return".
    if (m_mathChannels) {
        const int lost = m_mathChannels->fromJson(root.value(QLatin1String(KeyMathChannels))
                                                  .toArray());
        if (lost > 0) {
            // Counted, not fatal: the rest of the session is still worth
            // restoring, and the panes that wanted those channels come back
            // without them rather than not at all.
            qWarning("Config import: %d math channel(s) could not be restored", lost);
        }
    }

    // -- the logger, after the databases for the same reason as the channels:
    // a signal-value trigger is stored by name and resolved against the decode
    // table, which only exists once the editor's DBC above has been loaded.
    if (m_logModel) {
        const QJsonObject logger = root.value(QLatin1String(KeyLogger)).toObject();
        if (!logger.isEmpty()) {
            m_logModel->setPreTriggerSeconds(
                        logger.value(QStringLiteral("preTriggerSeconds")).toDouble(2.0));

            const auto restore = [this, &logger](const QString &key, bool isStart) {
                const QJsonObject c = logger.value(key).toObject();
                m_logModel->setTrigger(isStart,
                                       c.value(QStringLiteral("kind")).toInt(),
                                       quint32(c.value(QStringLiteral("frameId")).toDouble()),
                                       c.value(QStringLiteral("extended")).toBool(),
                                       c.value(QStringLiteral("messageName")).toString(),
                                       c.value(QStringLiteral("signalName")).toString(),
                                       c.value(QStringLiteral("comparison")).toInt(),
                                       c.value(QStringLiteral("threshold")).toDouble());
            };
            restore(QStringLiteral("startTrigger"), true);
            restore(QStringLiteral("stopTrigger"), false);

            const QJsonObject filter = logger.value(QStringLiteral("filter")).toObject();
            m_logModel->setLogFilter(filter.value(QStringLiteral("mode")).toInt(),
                                     quint32(filter.value(QStringLiteral("startId")).toDouble()),
                                     quint32(filter.value(QStringLiteral("endId")).toDouble()),
                                     filter.value(QStringLiteral("byteIndex")).toInt(),
                                     filter.value(QStringLiteral("payloadMask")).toInt(0xFF),
                                     filter.value(QStringLiteral("payloadMatch")).toInt());
        }
    }

    m_graphState = root.value(QLatin1String(KeyWorkspace)).toObject().toVariantMap();
    return true;
}

bool ConfigManager::importConfig(const QUrl &fileUrl)
{
    const QString path = localPathOf(fileUrl);
    if (path.isEmpty()) {
        setLastError(tr("“%1” is not a local file.").arg(fileUrl.toString()));
        setLastMessage(m_lastError);
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setLastError(tr("Could not open %1: %2").arg(QFileInfo(path).fileName(), file.errorString()));
        setLastMessage(m_lastError);
        return false;
    }

    QJsonParseError parseError {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setLastError(tr("%1 is not valid JSON: %2")
                     .arg(QFileInfo(path).fileName(), parseError.errorString()));
        setLastMessage(m_lastError);
        return false;
    }

    if (!m_missingFiles.isEmpty()) {
        m_missingFiles.clear();
        emit missingFilesChanged();
    }

    if (!applyDocument(document.object(), path)) {
        setLastMessage(m_lastError);
        return false;
    }

    setLastError(QString());
    // A database that has moved since the session was saved is a notice, not a
    // failure: the layout is still worth restoring, and the panes whose signals
    // are gone come back empty rather than not at all.
    if (m_missingFiles.isEmpty()) {
        setLastMessage(tr("Session loaded from %1.").arg(QFileInfo(path).fileName()));
    } else {
        setLastMessage(tr("Session loaded from %1 - %n database(s) could not be found.",
                          nullptr, int(m_missingFiles.size()))
                       .arg(QFileInfo(path).fileName()));
        for (const QString &missing : std::as_const(m_missingFiles))
            qWarning("Config import: missing database %s", qPrintable(missing));
    }

    // LAST, and only on success: QML now rebuilds the workspace against
    // databases that are already loaded.
    emit graphStateRestored();

    qInfo("Config import: %s", qPrintable(path));
    return true;
}
