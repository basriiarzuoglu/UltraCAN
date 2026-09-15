#include "core/TraceReader.h"
#include "CanPlaybackModel.h"

#include "core/CanBusManager.h"

#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTimer>

#include <algorithm>

#include "dbc/CanDecoder.h"

namespace {
/*! 60 Hz: the fastest a screen shows anything, and fine for a playhead. */
constexpr int ClockIntervalMs = 16;
constexpr qint64 NsPerSecond = 1'000'000'000LL;
}

CanPlaybackModel *CanPlaybackModel::s_instance = nullptr;

CanPlaybackModel::CanPlaybackModel(QObject *parent)
    : QObject(parent)
{
    if (!s_instance) {
        s_instance = this;
    } else {
        qCritical("CanPlaybackModel: a SECOND instance was created (%p; first was %p).",
                  static_cast<void *>(this), static_cast<void *>(s_instance));
    }

    m_clock = new QTimer(this);
    m_clock->setInterval(ClockIntervalMs);
    m_clock->setTimerType(Qt::PreciseTimer);
    connect(m_clock, &QTimer::timeout, this, &CanPlaybackModel::tick);
}

CanPlaybackModel::~CanPlaybackModel()
{
    if (s_instance == this)
        s_instance = nullptr;
}

CanPlaybackModel *CanPlaybackModel::instance()
{
    return s_instance;
}

void CanPlaybackModel::setDecoder(const CanDecoder *decoder)
{
    m_decoder = decoder;
}

void CanPlaybackModel::setChannelNames(const QStringList &names)
{
    if (names.isEmpty() || names == m_channelNames)
        return;
    m_channelNames = names;
}

// -------------------------------------------------------------------- model




// ------------------------------------------------------------------ lookups

int CanPlaybackModel::indexAtTime(qreal seconds) const
{
    if (m_frames.empty())
        return -1;

    const qint64 target = qint64(seconds * double(NsPerSecond));

    // O(log n): this is what keeps scrubbing responsive at half a million rows.
    const auto it = std::upper_bound(m_frames.begin(), m_frames.end(), target,
                                     [](qint64 value, const CanFrame &frame) {
                                         return value < frame.timestampNs;
                                     });
    if (it == m_frames.begin())
        return -1;                       // before the first frame
    return int(std::distance(m_frames.begin(), it)) - 1;
}

qreal CanPlaybackModel::timeAt(int index) const
{
    if (index < 0 || index >= int(m_frames.size()))
        return 0.0;
    return m_frames[size_t(index)].seconds();
}

QVariantMap CanPlaybackModel::frameAt(int row) const
{
    QVariantMap map;
    if (row < 0 || row >= int(m_frames.size()))
        return map;

    // Built by hand now that the role table is gone. Kept because it is a
    // handy way to inspect one frame from QML; the replayed trace itself shows
    // up in the CAN Logger, which sees the virtual bus like any other source.
    const CanFrame &frame = m_frames[size_t(row)];
    map.insert(QStringLiteral("time"), frame.seconds());
    map.insert(QStringLiteral("direction"),
               frame.direction == CanFrame::Tx ? QStringLiteral("Tx") : QStringLiteral("Rx"));
    map.insert(QStringLiteral("channel"),
               m_channelNames.value(frame.channel, QStringLiteral("CAN?")));
    map.insert(QStringLiteral("canId"), frame.id);
    map.insert(QStringLiteral("idText"), frame.idText());
    map.insert(QStringLiteral("extended"), frame.extended);
    map.insert(QStringLiteral("dlc"), int(frame.dlc));
    map.insert(QStringLiteral("payload"), frame.payloadText());
    map.insert(QStringLiteral("name"),
               m_decoder ? m_decoder->messageNameForFrame(frame.id, frame.extended) : QString());
    return map;
}

qreal CanPlaybackModel::averageRate() const
{
    const qreal seconds = duration();
    return seconds > 0 ? m_frames.size() / seconds : 0.0;
}

QString CanPlaybackModel::formatTime(qreal seconds) const
{
    const qreal clamped = qMax(0.0, seconds);
    const int minutes = int(clamped / 60);
    const qreal rest = clamped - minutes * 60;
    return QStringLiteral("%1:%2")
           .arg(minutes, 2, 10, QLatin1Char('0'))
           .arg(rest, 6, 'f', 3, QLatin1Char('0'));
}

// ---------------------------------------------------------------- transport

void CanPlaybackModel::play()
{
    if (m_frames.empty())
        return;
    if (m_positionNs >= m_durationNs) {
        setPositionNs(0, false);        // replay from the top rather than sitting at the end
        // A replay is a jump backwards in time, and everything holding a
        // rolling window has to be told. Without this the graph kept the
        // finished pass on screen and drew the new one over it: two runs
        // superimposed on one axis, which reads as noise rather than as a
        // second lap. seeked() is the existing "the playhead moved
        // discontinuously" signal - main() has it wired to
        // GraphDataModel::rebaseTo(), which drops the points AND unpins
        // m_latestTime so the window follows the log back to zero.
        emit seeked(0.0);
    }

    m_wallClock.restart();
    m_clock->start();
    // Claim the frame stream BEFORE the first tick, or the first batch is
    // dropped by the bus's own gate.
    setPlaybackActive(true);
    setPlaying(true);
}


void CanPlaybackModel::pause()
{
    m_clock->stop();
    setPlaying(false);
}

void CanPlaybackModel::stop()
{
    m_clock->stop();
    setPlaying(false);
    setPositionNs(0, false);
    // Stop hands the bus back to the hardware. Pause deliberately does not:
    // a paused log with live traffic resuming underneath it would put two
    // timelines on one graph, which is the thing this whole mode prevents.
    setPlaybackActive(false);
    emit seeked(0.0);
}

void CanPlaybackModel::seek(qreal seconds)
{
    const qint64 target = qBound<qint64>(0, qint64(seconds * double(NsPerSecond)), m_durationNs);
    setPositionNs(target, false);       // a scrub jumps; it does not "play" the frames it skips
    m_wallClock.restart();
    // Whatever is on the graph belongs to where the playhead WAS.
    emit seeked(double(target) / double(NsPerSecond));
}

void CanPlaybackModel::seekToIndex(int index)
{
    if (index < 0 || index >= int(m_frames.size()))
        return;
    setPositionNs(m_frames[size_t(index)].timestampNs, false);
    m_wallClock.restart();
    emit seeked(double(m_positionNs) / double(NsPerSecond));
}

void CanPlaybackModel::tick()
{
    if (m_frames.empty()) {
        pause();
        return;
    }

    // Wall-clock elapsed, NOT the nominal interval: a tick delayed by a repaint
    // must not slow the recording down.
    const qint64 elapsedNs = m_wallClock.nsecsElapsed();
    m_wallClock.restart();

    const qint64 advance = qint64(double(elapsedNs) * m_speed);
    qint64 next = m_positionNs + advance;

    if (next >= m_durationNs) {
        if (m_loop) {
            setPositionNs(m_durationNs, true);   // dispatch the tail first
            setPositionNs(0, false);
            // Same reason as the replay branch in play(): the lap that just
            // ended must come off the graph before the next one starts, or
            // every loop leaves its predecessor behind.
            //
            // Emitted AFTER the tail dispatch, and that ORDER costs the graph
            // the tail on purpose. rebaseTo() clears the staged-sample queue,
            // so the frames dispatched one line up never reach a series - but
            // they DO reach the log, the analyzer and anything else on the bus
            // path, none of which holds a rolling window. Emitting first would
            // be worse: the tail would then be attributed to the new lap and
            // land at t=duration on an axis just rebased to zero, dragging the
            // window to the far right before the first real point arrives.
            emit seeked(0.0);
            return;
        }
        setPositionNs(m_durationNs, true);
        pause();
        return;
    }

    setPositionNs(next, true);
}

void CanPlaybackModel::setPositionNs(qint64 positionNs, bool dispatch)
{
    positionNs = qBound<qint64>(0, positionNs, m_durationNs);
    const int previousIndex = m_currentIndex;
    const bool moved = positionNs != m_positionNs;
    m_positionNs = positionNs;

    const int index = indexAtTime(double(m_positionNs) / double(NsPerSecond));
    if (index != previousIndex) {
        m_currentIndex = index;
        emit currentIndexChanged();

        // Everything between the old and the new playhead just went past.
        if (dispatch && index > previousIndex) {
            dispatchRange(previousIndex + 1, index);
            emit framesPlayed(previousIndex + 1, index);
        }
    }

    if (moved)
        emit positionChanged();
}

void CanPlaybackModel::setPlaying(bool playing)
{
    if (m_playing == playing)
        return;
    m_playing = playing;
    emit playingChanged();
}

void CanPlaybackModel::setSpeed(qreal speed)
{
    speed = qBound(0.01, speed, 100.0);
    if (qFuzzyCompare(m_speed, speed))
        return;
    m_speed = speed;
    m_wallClock.restart();              // do not apply the new rate to time already elapsed
    emit speedChanged();
}

void CanPlaybackModel::setLoop(bool loop)
{
    if (m_loop == loop)
        return;
    m_loop = loop;
    emit loopChanged();
}

void CanPlaybackModel::setBusManager(CanBusManager *manager)
{
    m_busManager = manager;
}

void CanPlaybackModel::setPlaybackActive(bool active)
{
    if (m_playbackActive == active)
        return;
    m_playbackActive = active;
    emit playbackActiveChanged();
}

void CanPlaybackModel::dispatchRange(int firstIndex, int lastIndex)
{
    if (!m_busManager || firstIndex < 0)
        return;

    const int last = qMin(lastIndex, int(m_frames.size()) - 1);
    if (last < firstIndex)
        return;

    // A slow tick after a 10x speed change can cover thousands of frames. One
    // batch, one lock on the far side - the same shape the driver delivers.
    m_dispatchBuffer.clear();
    m_dispatchBuffer.reserve(last - firstIndex + 1);
    for (int i = firstIndex; i <= last; ++i)
        m_dispatchBuffer.append(m_frames[size_t(i)]);

    m_busManager->ingestPlaybackBatch(m_dispatchBuffer);
}

void CanPlaybackModel::setLastError(const QString &message)
{
    if (m_lastError == message)
        return;
    m_lastError = message;
    if (!message.isEmpty())
        qWarning("CanPlaybackModel: %s", qPrintable(message));
    emit lastErrorChanged();
}

// ================================================================== file I/O

void CanPlaybackModel::applyFrames(std::vector<CanFrame> &&frames, const QString &name,
                                   const QStringList &warnings)
{
    // Readers may emit out of order (some tools interleave channels); the
    // binary search in indexAtTime() requires sorted timestamps.
    std::stable_sort(frames.begin(), frames.end(),
                     [](const CanFrame &a, const CanFrame &b) {
                         return a.timestampNs < b.timestampNs;
                     });

    // Normalise so the first frame is t = 0, which is what a player's timeline
    // means (absolute capture times belong in the file, not the scrubber).
    if (!frames.empty()) {
        const qint64 base = frames.front().timestampNs;
        if (base != 0) {
            for (CanFrame &frame : frames)
                frame.timestampNs -= base;
        }
    }

    m_clock->stop();
    setPlaying(false);

    m_frames = std::move(frames);
    m_sourceName = name;
    m_warnings = warnings;
    m_durationNs = m_frames.empty() ? 0 : m_frames.back().timestampNs;

    m_positionNs = 0;
    m_currentIndex = -1;
    emit loadedChanged();
    emit positionChanged();
    emit currentIndexChanged();
}

void CanPlaybackModel::clear()
{
    applyFrames({}, QString(), {});
    setLastError(QString());
}

bool CanPlaybackModel::loadLog(const QUrl &fileUrl)
{
    std::vector<CanFrame> frames;
    QStringList warnings;
    QString error;

    if (!TraceReader::readFile(fileUrl, m_channelNames, frames, warnings, error)) {
        setLastError(error);
        return false;
    }

    setLastError(QString());
    const QString name = QFileInfo(fileUrl.isLocalFile() ? fileUrl.toLocalFile()
                                                         : fileUrl.toString()).fileName();
    applyFrames(std::move(frames), name, warnings);
    qInfo("Playback: %d frames over %.3f s from %s (%lld warnings)",
          count(), duration(), qPrintable(m_sourceName), qint64(m_warnings.size()));
    return true;
}

void CanPlaybackModel::loadDemo(const QString &name, int frameCount)
{
    struct Templ { const char *name; quint32 id; bool ext; quint8 ch; CanFrame::Direction dir;
                   quint8 dlc; int weight; };
    static const Templ templates[] = {
        { "EngineData",     0x100,      false, 0, CanFrame::Rx, 8, 10 },
        { "BatteryStatus",  0x200,      false, 0, CanFrame::Rx, 8, 5  },
        { "VehicleCommand", 0x300,      false, 1, CanFrame::Tx, 4, 4  },
        { "ChargerStatus",  0x18FF50E5, true,  1, CanFrame::Rx, 8, 2  },
        { "DiagResponse",   0x7E8,      false, 0, CanFrame::Rx, 8, 1  }
    };

    std::vector<const Templ *> bag;
    for (const Templ &t : templates)
        for (int w = 0; w < t.weight; ++w)
            bag.push_back(&t);

    std::vector<CanFrame> frames;
    frames.reserve(size_t(qMax(0, frameCount)));

    qint64 timestampNs = 0;
    auto *random = QRandomGenerator::global();
    for (int i = 0; i < frameCount; ++i) {
        const Templ *t = bag[size_t(random->bounded(int(bag.size())))];
        timestampNs += 2'000'000LL + random->bounded(20'000'000);   // 2..22 ms

        CanFrame frame;
        frame.timestampNs = timestampNs;
        frame.id = t->id;
        frame.name = QString::fromLatin1(t->name);
        frame.extended = t->ext;
        frame.channel = t->ch;
        frame.direction = t->dir;
        frame.dlc = t->dlc;
        for (quint8 b = 0; b < t->dlc; ++b)
            frame.payload[b] = quint8(random->bounded(256));
        frames.push_back(frame);
    }

    setLastError(QString());
    applyFrames(std::move(frames), name.isEmpty() ? tr("demo recording") : name, {});
}
