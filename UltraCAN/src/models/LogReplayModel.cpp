#include "LogReplayModel.h"

#include <QFileInfo>
#include <QThread>
#include <QtGlobal>

#include <algorithm>

#include "core/BusReplayPacer.h"
#include "core/CanBusManager.h"
#include "core/TraceReader.h"

namespace {
constexpr qint64 NsPerSecond = 1'000'000'000LL;
}

LogReplayModel::LogReplayModel(QObject *parent)
    : QObject(parent)
{
}

LogReplayModel::~LogReplayModel()
{
    // stop() BEFORE quit(): the pacer is inside runFrom(), not in an event
    // loop, so a queued quit would sit behind a slot that never returns and
    // wait() would hang the exit.
    if (m_pacer)
        m_pacer->stop();
    if (m_pacerThread) {
        m_pacerThread->quit();
        m_pacerThread->wait();
    }
}

void LogReplayModel::setBusManager(CanBusManager *manager)
{
    if (m_busManager == manager)
        return;
    if (m_busManager)
        disconnect(m_busManager, nullptr, this, nullptr);

    m_busManager = manager;
    if (m_pacer)
        m_pacer->setBusManager(manager);

    if (m_busManager) {
        // A cable pulled mid-replay must not leave the transport lit over a
        // bus that is gone, and must not leave the interlock armed either.
        connect(m_busManager, &CanBusManager::connectionChanged, this, [this] {
            emit busReadyChanged();
            if (!isBusReady() && (m_playing || m_armed)) {
                stop();
                setLastError(tr("The CAN interface went away - replay stopped and disarmed."));
            }
        });
    }
    emit busReadyChanged();
}

void LogReplayModel::setChannelNames(const QStringList &names)
{
    m_channelNames = names;
}

bool LogReplayModel::isBusReady() const
{
    return m_busManager && m_busManager->isConnected();
}

qreal LogReplayModel::duration() const
{
    return double(m_durationNs) / double(NsPerSecond);
}

qreal LogReplayModel::position() const
{
    return double(m_positionNs) / double(NsPerSecond);
}

// ------------------------------------------------------------------- loading

bool LogReplayModel::loadLog(const QUrl &fileUrl)
{
    std::vector<CanFrame> frames;
    QStringList warnings;
    QString error;

    if (!TraceReader::readFile(fileUrl, m_channelNames, frames, warnings, error)) {
        setLastError(error);
        return false;
    }

    // A new file is a new intent. Whatever was armed was armed for the old one.
    stop();
    setArmed(false);

    std::sort(frames.begin(), frames.end(),
              [](const CanFrame &a, const CanFrame &b) { return a.timestampNs < b.timestampNs; });

    m_frames = std::move(frames);
    m_warnings = warnings;
    m_sourceName = QFileInfo(fileUrl.isLocalFile() ? fileUrl.toLocalFile()
                                                   : fileUrl.toString()).fileName();
    m_originNs = m_frames.empty() ? 0 : m_frames.front().timestampNs;
    m_durationNs = m_frames.empty() ? 0 : m_frames.back().timestampNs - m_originNs;
    m_positionNs = 0;
    m_framesSent = 0;

    setLastError(QString());
    emit loadedChanged();
    emit positionChanged();
    emit progressChanged();

    qInfo("Log replay: loaded %d frames over %.3f s from %s",
          count(), duration(), qPrintable(m_sourceName));
    return true;
}

void LogReplayModel::clear()
{
    stop();
    setArmed(false);
    m_frames.clear();
    m_frames.shrink_to_fit();
    m_sourceName.clear();
    m_warnings.clear();
    m_originNs = 0;
    m_durationNs = 0;
    m_positionNs = 0;
    m_framesSent = 0;
    emit loadedChanged();
    emit positionChanged();
    emit progressChanged();
}

// -------------------------------------------------------------------- arming

void LogReplayModel::setArmed(bool armed)
{
    if (m_armed == armed)
        return;
    if (armed && !isBusReady()) {
        setLastError(tr("No CAN interface is connected - there is nothing to transmit on."));
        return;
    }
    if (!armed && m_playing)
        pause();                    // disarming mid-run stops the wire first
    m_armed = armed;
    emit armedChanged();
}

// ----------------------------------------------------------------- transport

void LogReplayModel::ensurePacer()
{
    if (m_pacerThread)
        return;

    m_pacerThread = new QThread(this);
    m_pacerThread->setObjectName(QStringLiteral("can-replay-pacer"));
    m_pacer = new BusReplayPacer;            // no parent: moved to the thread
    m_pacer->setBusManager(m_busManager);
    m_pacer->moveToThread(m_pacerThread);
    connect(m_pacerThread, &QThread::finished, m_pacer, &QObject::deleteLater);

    connect(this, &LogReplayModel::startPacer, m_pacer, &BusReplayPacer::runFrom);

    connect(m_pacer, &BusReplayPacer::progress, this, [this](qint64 positionNs) {
        setPositionNs(positionNs);
    });
    connect(m_pacer, &BusReplayPacer::looped, this, [this] {
        setPositionNs(0);
        emit looped();
    });
    connect(m_pacer, &BusReplayPacer::finished, this, [this](bool completedRun) {
        setTransmitting(false);
        setPlaying(false);
        if (completedRun) {
            setPositionNs(m_durationNs);
            // Ran to the end on its own. Disarm: the next run should be as
            // deliberate as the first.
            setArmed(false);
            emit completed();
        }
    });

    // The pacer's whole job is to wake on time, and it loses that race to any
    // repaint scheduled beside it.
    m_pacerThread->start(QThread::HighPriority);
}

void LogReplayModel::play()
{
    if (m_frames.empty()) {
        setLastError(tr("Load a recording first."));
        return;
    }
    if (!isBusReady()) {
        setLastError(tr("No CAN interface is connected."));
        return;
    }
    if (!m_armed) {
        // The interlock. Not a warning the user can click past: play() simply
        // does not transmit until arming has happened deliberately.
        setLastError(tr("Replay is not armed. Arm it before transmitting."));
        return;
    }
    if (m_playing)
        return;

    ensurePacer();
    setLastError(QString());

    if (m_positionNs >= m_durationNs)
        setPositionNs(0);

    // Copied on THIS thread before the run starts, so loading another file
    // mid-replay cannot pull the vector out from under the pacer.
    QVector<CanFrame> copy;
    copy.reserve(int(m_frames.size()));
    for (const CanFrame &frame : m_frames)
        copy.append(frame);

    setPlaying(true);
    setTransmitting(true);
    emit startPacer(copy, indexAtPosition(), m_speed, m_loop);
}

void LogReplayModel::pause()
{
    if (m_pacer)
        m_pacer->stop();          // atomic + notify; returns immediately
    setTransmitting(false);
    setPlaying(false);
}

void LogReplayModel::stop()
{
    pause();
    setPositionNs(0);
    // Disarmed on stop, deliberately. A transmitter that stays armed after the
    // user has finished with it is a transmitter that runs again by accident.
    if (m_armed) {
        m_armed = false;
        emit armedChanged();
    }
}

void LogReplayModel::seek(qreal seconds)
{
    const qint64 target = qBound<qint64>(0, qint64(seconds * double(NsPerSecond)), m_durationNs);
    const bool wasPlaying = m_playing;
    if (wasPlaying)
        pause();
    setPositionNs(target);
    if (wasPlaying)
        play();                   // restarts the pacer from the new index
}

int LogReplayModel::indexAtPosition() const
{
    if (m_frames.empty())
        return 0;
    const qint64 absolute = m_originNs + m_positionNs;
    const auto it = std::lower_bound(m_frames.begin(), m_frames.end(), absolute,
                                     [](const CanFrame &frame, qint64 value) {
                                         return frame.timestampNs < value;
                                     });
    return int(std::distance(m_frames.begin(), it));
}

// ------------------------------------------------------------- plain setters

void LogReplayModel::setSpeed(qreal speed)
{
    speed = qBound(0.01, speed, 100.0);
    if (qFuzzyCompare(m_speed, speed))
        return;
    m_speed = speed;
    if (m_pacer)
        m_pacer->setSpeed(speed);      // re-anchors mid-run; see BusReplayPacer
    emit speedChanged();
}

void LogReplayModel::setLoop(bool loop)
{
    if (m_loop == loop)
        return;
    m_loop = loop;
    if (m_pacer)
        m_pacer->setLoop(loop);
    emit loopChanged();
}

void LogReplayModel::setPlaying(bool playing)
{
    if (m_playing == playing)
        return;
    m_playing = playing;
    emit playingChanged();
}

void LogReplayModel::setTransmitting(bool transmitting)
{
    if (m_transmitting == transmitting)
        return;
    m_transmitting = transmitting;
    emit transmittingChanged();
}

void LogReplayModel::setPositionNs(qint64 positionNs)
{
    positionNs = qBound<qint64>(0, positionNs, m_durationNs);
    if (m_positionNs == positionNs)
        return;
    m_positionNs = positionNs;
    m_framesSent = indexAtPosition();
    emit positionChanged();
    emit progressChanged();
}

void LogReplayModel::setLastError(const QString &message)
{
    if (m_lastError == message)
        return;
    m_lastError = message;
    emit lastErrorChanged();
}

QString LogReplayModel::formatTime(qreal seconds) const
{
    if (seconds < 0 || !qIsFinite(seconds))
        seconds = 0;
    const int minutes = int(seconds) / 60;
    const double rest = seconds - minutes * 60;
    return QStringLiteral("%1:%2").arg(minutes, 2, 10, QLatin1Char('0'))
                                  .arg(rest, 6, 'f', 3, QLatin1Char('0'));
}
