#include "LogExporter.h"

#include <QDateTime>
#include <QFileInfo>
#include <QMetaObject>
#include <QRunnable>
#include <QSaveFile>
#include <QThreadPool>

#include "core/TraceFormat.h"
#include "models/CanLogModel.h"

namespace {

// The format writers moved to core/TraceFormat.h when streaming to disk
// became a second writer of the same three formats - see the note there.
using namespace TraceFormat;

/*!
    Formats and writes the whole snapshot. Runs on a worker thread for anything
    but a small trace, so it touches nothing but its own arguments.

    QSaveFile, not QFile: on failure the destination keeps whatever it had. A
    truncated trace under the right filename is worse than no file at all,
    because it looks like it worked.
*/
bool writeTrace(const QString &path, int format, const std::vector<CanFrame> &frames,
                const QStringList &channelNames, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        *error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
        return false;
    }

    QByteArray chunk;
    chunk.reserve(ChunkFlushBytes + 4096);

    switch (format) {
    case LogExporter::Asc: appendAscHeader(chunk); break;
    case LogExporter::Trc: appendTrcHeader(chunk); break;
    default:               appendCsvHeader(chunk); break;
    }

    int number = 1;
    for (const CanFrame &frame : frames) {
        switch (format) {
        case LogExporter::Asc: appendAscFrame(chunk, frame); break;
        case LogExporter::Trc: appendTrcFrame(chunk, frame, number++); break;
        default:               appendCsvFrame(chunk, frame, channelNames); break;
        }

        if (chunk.size() >= ChunkFlushBytes) {
            if (file.write(chunk) != chunk.size()) {
                *error = QStringLiteral("Write failed: %1").arg(file.errorString());
                file.cancelWriting();
                return false;
            }
            chunk.clear();
        }
    }

    if (format == LogExporter::Asc)
        appendAscFooter(chunk);

    if (!chunk.isEmpty() && file.write(chunk) != chunk.size()) {
        *error = QStringLiteral("Write failed: %1").arg(file.errorString());
        file.cancelWriting();
        return false;
    }

    // commit() is the atomic rename. Until it returns true nothing has replaced
    // the destination.
    if (!file.commit()) {
        *error = QStringLiteral("Could not finish %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

/*! The worker. Owns its copy of the frames outright, so nothing it touches can
    be mutated by the GUI thread while it runs. */
class ExportTask : public QRunnable
{
public:
    ExportTask(LogExporter *owner, std::vector<CanFrame> frames, QStringList channelNames,
               QString path, QString fileName, int format)
        : m_owner(owner)
        , m_frames(std::move(frames))
        , m_channelNames(std::move(channelNames))
        , m_path(std::move(path))
        , m_fileName(std::move(fileName))
        , m_format(format)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        QString error;
        const bool ok = writeTrace(m_path, m_format, m_frames, m_channelNames, &error);
        const int frameCount = int(m_frames.size());

        // Back to the GUI thread. By name rather than by lambda because
        // reportResult is private and this class is not a friend.
        QMetaObject::invokeMethod(m_owner, "reportResult", Qt::QueuedConnection,
                                  Q_ARG(bool, ok), Q_ARG(QString, m_fileName),
                                  Q_ARG(int, frameCount), Q_ARG(QString, error));
    }

private:
    LogExporter *m_owner;
    std::vector<CanFrame> m_frames;
    QStringList m_channelNames;
    QString m_path;
    QString m_fileName;
    int m_format;
};

}   // namespace

// ============================================================== LogExporter

LogExporter *LogExporter::s_instance = nullptr;

LogExporter::LogExporter(QObject *parent)
    : QObject(parent)
{
    if (!s_instance) {
        s_instance = this;
    } else {
        qCritical("LogExporter: a SECOND instance was created (%p; first was %p).",
                  static_cast<void *>(this), static_cast<void *>(s_instance));
    }
}

LogExporter::~LogExporter()
{
    // A running task holds a raw pointer to this object and will post to it
    // when it finishes. Outliving that is not optional.
    QThreadPool::globalInstance()->waitForDone();
    if (s_instance == this)
        s_instance = nullptr;
}

LogExporter *LogExporter::instance()
{
    return s_instance;
}

void LogExporter::setSource(CanLogModel *model)
{
    m_source = model;
}

QStringList LogExporter::formatNames() const
{
    return { tr("CSV trace (*.csv)"), tr("Vector ASCII (*.asc)"), tr("PCAN trace (*.trc)") };
}

int LogExporter::formatForSuffix(const QString &suffix) const
{
    const QString lower = suffix.toLower();
    if (lower == QLatin1String("asc"))
        return Asc;
    if (lower == QLatin1String("trc"))
        return Trc;
    return Csv;
}

QString LogExporter::suffixFor(int format) const
{
    switch (format) {
    case Asc: return QStringLiteral("asc");
    case Trc: return QStringLiteral("trc");
    default:  return QStringLiteral("csv");
    }
}

bool LogExporter::exportLog(const QUrl &fileUrl, int format)
{
    if (m_busy) {
        setLastError(tr("An export is already running."));
        return false;
    }
    if (!m_source) {
        setLastError(tr("No trace is attached - the backend is not wired up."));
        return false;
    }

    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();
    if (path.isEmpty()) {
        setLastError(tr("No destination file was given."));
        return false;
    }

    const QFileInfo info(path);
    const int resolved = (format < 0) ? formatForSuffix(info.suffix()) : format;

    // GUI thread, deliberately: see snapshot()'s documentation. Everything
    // after this line works on a copy nothing else can touch.
    std::vector<CanFrame> frames = m_source->snapshot();
    if (frames.empty()) {
        setLastError(tr("The trace is empty - there is nothing to export."));
        return false;
    }

    const int frameCount = int(frames.size());
    const QString fileName = info.fileName();

    setLastError(QString());
    setBusy(true);
    emit started(fileName, frameCount);

    if (frameCount <= SyncFrameLimit) {
        // Cheaper than a thread hop, and the status line updates in the same
        // frame as the click.
        QString error;
        const bool ok = writeTrace(path, resolved, frames, m_source->channelNames(), &error);
        reportResult(ok, fileName, frameCount, error);
        return true;
    }

    QThreadPool::globalInstance()->start(
        new ExportTask(this, std::move(frames), m_source->channelNames(),
                       path, fileName, resolved));
    return true;
}

void LogExporter::reportResult(bool ok, const QString &fileName, int frameCount,
                               const QString &error)
{
    setBusy(false);
    setLastError(ok ? QString() : error);

    if (ok)
        qInfo("Export: %d frames -> %s", frameCount, qPrintable(fileName));
    else
        qWarning("Export FAILED (%s): %s", qPrintable(fileName), qPrintable(error));

    emit finished(ok, fileName, frameCount, error);
}

void LogExporter::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void LogExporter::setLastError(const QString &message)
{
    if (m_lastError == message)
        return;
    m_lastError = message;
    emit lastErrorChanged();
}
