#include "CanLogModel.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>

#include "core/LogExporter.h"
#include "core/TraceStreamWriter.h"

CanLogModel::~CanLogModel()
{
    // The writer owns an open QFile. Quitting the thread lets its event loop
    // run out and deleteLater fire THERE, so the destructor that flushes and
    // closes runs on the thread that opened the file - never on this one.
    if (m_writerThread) {
        m_writerThread->quit();
        m_writerThread->wait();
    }
}

CanLogModel::CanLogModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_ring.resize(size_t(m_capacity));

    // Slower than the 5 Hz readouts elsewhere: this only has to notice that a
    // frame STOPPED, and a rate cell that blinks is worse than one that takes
    // half a second to admit the truth.
    m_rateWindow.start();
    m_rateTimer = new QTimer(this);
    // 200 ms, matching DbcFlatModel's live column: fast enough that a stopped
    // message is admitted within ~600 ms, slow enough that the count in each
    // window is a meaningful sample.
    m_rateTimer->setInterval(200);
    connect(m_rateTimer, &QTimer::timeout, this, &CanLogModel::decayRates);
    m_rateTimer->start();
}

// ---------------------------------------------------------------- read side

int CanLogModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_count;
}

const CanFrame &CanLogModel::frameAt(int row) const
{
    return m_ring[size_t((m_start + row) % m_capacity)];
}

std::vector<CanFrame> CanLogModel::snapshot() const
{
    // const_cast: flushPending() mutates, but "what has this model received"
    // is conceptually a read. Draining first is what makes the exported file
    // match the bus rather than the last repaint.
    const_cast<CanLogModel *>(this)->flushPending();

    std::vector<CanFrame> out;
    out.reserve(size_t(m_count));
    for (int row = 0; row < m_count; ++row)
        out.push_back(frameAt(row));
    return out;
}

QVariant CanLogModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_count)
        return {};

    const CanFrame &frame = frameAt(index.row());

    switch (role) {
    case TimeRole:      return frame.seconds();
    case DirectionRole: return frame.direction == CanFrame::Tx ? QStringLiteral("Tx")
                                                               : QStringLiteral("Rx");
    case ChannelRole:   return m_channelNames.value(frame.channel, QStringLiteral("CAN?"));
    case CanIdRole:     return frame.id;
    case IdTextRole:    return frame.idText();       // formatted only for visible rows
    case ExtendedRole:  return frame.extended;
    case DlcRole:       return int(frame.dlc);
    case PayloadRole:   return frame.payloadText();
    case NameRole:      return frame.name;
    // Looked up rather than stored on the row: CanFrame is the wire struct,
    // deliberately POD and allocation-free, and a per-row counter would make
    // every copy on the device thread carry display state.
    case CountRole:     return qint64(m_countByKey.value(frameKey(frame), 0));
    case FrequencyRole: return frequencyFor(frameKey(frame));
    default:            return {};
    }
}

QHash<int, QByteArray> CanLogModel::roleNames() const
{
    // Must stay identical to what LogRow.qml / CanLoggerView.qml read.
    return {
        { TimeRole,      QByteArrayLiteral("time")      },
        { DirectionRole, QByteArrayLiteral("direction") },
        { ChannelRole,   QByteArrayLiteral("channel")   },
        { CanIdRole,     QByteArrayLiteral("canId")     },
        { IdTextRole,    QByteArrayLiteral("idText")    },
        { ExtendedRole,  QByteArrayLiteral("extended")  },
        { DlcRole,       QByteArrayLiteral("dlc")       },
        { PayloadRole,   QByteArrayLiteral("payload")   },
        { NameRole,      QByteArrayLiteral("name")      },
        { CountRole,     QByteArrayLiteral("count")     },
        { FrequencyRole, QByteArrayLiteral("frequency") }
    };
}

// ---------------------------------------------------------------- properties

int CanLogModel::count() const
{
    return m_count;
}

int CanLogModel::capacity() const
{
    return m_capacity;
}

void CanLogModel::setCapacity(int capacity)
{
    capacity = qMax(1, capacity);
    if (capacity == m_capacity)
        return;

    // Keep the newest frames that still fit; simplest correct approach is to
    // copy them out in order and rebuild the ring.
    const int keep = qMin(m_count, capacity);
    QVector<CanFrame> survivors;
    survivors.reserve(keep);
    for (int row = m_count - keep; row < m_count; ++row)
        survivors.append(frameAt(row));

    beginResetModel();
    m_droppedCount += (m_count - keep);
    m_capacity = capacity;
    m_ring.assign(size_t(m_capacity), CanFrame {});
    m_start = 0;
    m_count = keep;
    for (int i = 0; i < keep; ++i)
        m_ring[size_t(i)] = survivors[i];
    reindexRows();          // m_start moved and rows were dropped
    endResetModel();

    emit capacityChanged();
    emit countChanged();
    emit statisticsChanged();
}

void CanLogModel::setChannelNames(const QStringList &names)
{
    if (names == m_channelNames)
        return;
    m_channelNames = names;
    if (m_count > 0)
        emit dataChanged(index(0), index(m_count - 1), { ChannelRole });
}

// ------------------------------------------------------------- producer side

void CanLogModel::enqueue(const CanFrame &frame)
{
    {
        QMutexLocker locker(&m_pendingMutex);
        m_pending.append(frame);
    }
    scheduleFlush();
}

void CanLogModel::ingestForDisk(const QVector<CanFrame> &frames)
{
    // Called from CanBusManager::fanOut() on the DEVICE thread, in front of
    // the recording gate - so the background log keeps writing while the trace
    // is not listening, while the ring is evicting, and while overwrite mode
    // is collapsing rows away. See the header note.
    //
    // The connection is queued, so this is a refcount bump on a shared
    // container and a post to another thread's event loop: no disk syscall on
    // the device thread, whatever the filesystem is doing.
    // m_feedingWriter, not m_streaming: while ARMED the writer needs every
    // frame to fill its pre-trigger ring, even though none of them reach the
    // file yet. An atomic because this is the device thread reading a flag the
    // GUI thread writes.
    if (m_feedingWriter.load(std::memory_order_relaxed) && !frames.isEmpty())
        emit framesForDisk(frames);
}

void CanLogModel::enqueue(const QVector<CanFrame> &frames)
{
    if (frames.isEmpty())
        return;

    {
        QMutexLocker locker(&m_pendingMutex);
        m_pending += frames;
    }
    scheduleFlush();
}

void CanLogModel::scheduleFlush()
{
    // Only one flush may be in flight. Everything produced meanwhile is picked
    // up by that same flush, so a burst becomes one batch instead of N events.
    bool expected = false;
    if (!m_flushScheduled.compare_exchange_strong(expected, true))
        return;

    QMetaObject::invokeMethod(this, "flushPending", Qt::QueuedConnection);
}

void CanLogModel::flushPending()
{
    QVector<CanFrame> batch;
    {
        QMutexLocker locker(&m_pendingMutex);
        batch.swap(m_pending);
    }
    m_flushScheduled.store(false);

    if (batch.isEmpty())
        return;

    appendBatch(batch);

    // A producer may have enqueued while we were flushing and found the flag
    // still set; make sure that work is not stranded.
    {
        QMutexLocker locker(&m_pendingMutex);
        if (m_pending.isEmpty())
            return;
    }
    scheduleFlush();
}

void CanLogModel::appendBatch(const QVector<CanFrame> &batch)
{
    m_totalCount += batch.size();

    // Counted before either strategy runs, and for every frame in both modes:
    // the number describes the bus, not the display. Cheap - one hash bump per
    // frame, on the GUI thread, already inside a coalesced batch.
    for (const CanFrame &frame : batch) {
        const qint64 seen = ++m_countByKey[frameKey(frame)];
        if (seen > m_maxCount)
            m_maxCount = seen;
        noteArrival(frame);
    }

    if (m_overwriteMode) {
        appendOverwriting(batch);
        emit countChanged();
        emit statisticsChanged();
        return;
    }
    appendChronological(batch);

    emit countChanged();
    emit statisticsChanged();
}

void CanLogModel::appendChronological(const QVector<CanFrame> &batch)
{

    // A batch larger than the whole buffer: only its tail can ever be visible.
    int first = 0;
    if (batch.size() > m_capacity) {
        first = batch.size() - m_capacity;
        m_droppedCount += first;
    }
    const int incoming = batch.size() - first;

    const int overflow = m_count + incoming - m_capacity;
    if (overflow > 0) {
        beginRemoveRows(QModelIndex(), 0, overflow - 1);
        m_start = (m_start + overflow) % m_capacity;
        m_count -= overflow;
        m_droppedCount += overflow;
        endRemoveRows();
    }

    beginInsertRows(QModelIndex(), m_count, m_count + incoming - 1);
    for (int i = 0; i < incoming; ++i) {
        m_ring[size_t((m_start + m_count) % m_capacity)] = batch[first + i];
        ++m_count;
    }
    endInsertRows();

    // The rows that were ALREADY on screen did not change, but their counts
    // did - a repeat of an id further up the list makes that row's Count
    // stale. One spanning emission for the pre-existing rows, not one per
    // frame; the newly inserted ones are covered by endInsertRows().
    const int existing = m_count - incoming;
    if (existing > 0)
        emit dataChanged(index(0), index(existing - 1), { CountRole });
}

/*!
    One row per identity, updated in place.

    Two things this must not do, both learned the hard way elsewhere in this
    codebase:

    \list
    \li \b {No dataChanged per frame.} At 5 000 frames/s across 20 identities
        that is 5 000 signal emissions a second into a delegate that can repaint
        60 times a second. The batch is already coalesced by the flush timer, so
        one spanning emission per batch is both correct and two orders of
        magnitude cheaper.
    \li \b {No assuming rows stay put.} The row indexes cached in m_rowByKey
        are offsets from m_start, and an eviction moves m_start. Overwrite mode
        only evicts when the bus carries more distinct identities than the ring
        has slots - rare, but "rare" is not "never" on an unknown bus, and the
        consequence would be rows silently updating each other's data.
    \endlist
*/
void CanLogModel::appendOverwriting(const QVector<CanFrame> &batch)
{
    int touchedLow = -1;
    int touchedHigh = -1;

    for (const CanFrame &frame : batch) {
        const quint64 key = frameKey(frame);
        const auto it = m_rowByKey.constFind(key);

        if (it != m_rowByKey.constEnd()) {
            const int row = it.value();
            m_ring[size_t((m_start + row) % m_capacity)] = frame;
            touchedLow = touchedLow < 0 ? row : qMin(touchedLow, row);
            touchedHigh = qMax(touchedHigh, row);
            continue;
        }

        // A new identity. Appending is the same ring arithmetic as the
        // chronological path, eviction included.
        if (m_count == m_capacity) {
            beginRemoveRows(QModelIndex(), 0, 0);
            m_start = (m_start + 1) % m_capacity;
            --m_count;
            ++m_droppedCount;
            endRemoveRows();
            // Every cached row index just shifted down by one. Rebuilding is
            // O(rows) and happens only when a bus exceeds the capacity in
            // DISTINCT identities; paying it lazily would mean serving wrong
            // rows in between.
            reindexRows();
            touchedLow = touchedHigh = -1;
        }

        beginInsertRows(QModelIndex(), m_count, m_count);
        m_ring[size_t((m_start + m_count) % m_capacity)] = frame;
        m_rowByKey.insert(key, m_count);
        ++m_count;
        endInsertRows();
    }

    if (touchedLow >= 0 && touchedHigh >= touchedLow)
        emit dataChanged(index(touchedLow), index(touchedHigh));
}

namespace {

/*! Weight of the newest window. Same constant DbcFlatModel's rate column
    landed on: settles in about a second and still ignores one odd window. */
constexpr double RateSmoothing = 0.35;

/*! Windows with nothing arriving before an identity is snapped to exactly 0.
    Three rather than one so a bus that skips a single window does not blink to
    0 Hz and back; the EMA alone would only approach zero asymptotically and a
    stopped message would sit at 0.4 Hz forever. */
constexpr int EmptyWindowsUntilDead = 3;

}   // namespace

/*!
    COUNTING, not timing.

    The first version derived the rate from the frame TIMESTAMPS - an average
    of the gaps between arrivals of the same id. The arithmetic was right and
    tested, and on the bench it still produced an empty column, because every
    timestamp-derived estimate has the same weakness: it is only as good as the
    timestamps, and there are several ordinary ways for those to be unusable.

      - A plugin that reports no hardware timestamp falls back to the bus clock
        read once per frame; frames handled inside the same clock tick then
        share a timestamp and every delta is zero.
      - A driver whose timestamps are not monotonic per id produces negative
        deltas, which have to be discarded, and nothing is ever learned.
      - Worst of all, the staleness test compared each id against the NEWEST
        timestamp seen on ANY id. One frame with an out-of-range timestamp -
        an epoch re-anchor, a wrapped counter, an echo stamped from a different
        base - moved that high-water mark permanently into the future and every
        row on the screen read 0 Hz from then on. A single bad frame killed the
        whole column, silently and for the rest of the session.

    Counting frames per fixed window has none of those failure modes because it
    does not read the timestamps at all. The window is wall time, the count is
    an integer, and an id that stops simply counts zero. It is also what
    DbcFlatModel's live-rate column already does, which is the strongest
    argument available: that one works on this hardware today.

    The cost is resolution - a rate is only known to within one window - and
    that is the right trade for a column whose job is "is this arriving, and
    roughly how fast".
*/
void CanLogModel::noteArrival(const CanFrame &frame)
{
    ++m_windowCountByKey[frameKey(frame)];
}

double CanLogModel::frequencyFor(quint64 key) const
{
    return m_rateByKey.value(key, 0.0);
}

void CanLogModel::decayRates()
{
    // Wall time, measured, not the nominal interval: a tick delayed by a
    // repaint would otherwise report a rate too high by exactly the delay.
    const qint64 elapsedNs = m_rateWindow.isValid() ? m_rateWindow.nsecsElapsed() : 0;
    m_rateWindow.restart();
    const double seconds = double(elapsedNs) / 1e9;
    if (seconds <= 0.0)
        return;

    QHash<quint64, int> counts;
    counts.swap(m_windowCountByKey);

    // Every id we have ever seen is stepped, not just the ones that arrived -
    // otherwise a message that stops transmitting keeps displaying its last
    // rate forever, which is the reading a technician most needs to be true.
    const QList<quint64> known = m_rateByKey.keys();
    for (quint64 key : known) {
        if (!counts.contains(key))
            counts.insert(key, 0);
    }

    bool changed = false;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        const quint64 key = it.key();
        const int seen = it.value();

        if (seen == 0) {
            const int empties = ++m_emptyWindowsByKey[key];
            if (empties >= EmptyWindowsUntilDead) {
                // Snap, do not decay: an EMA only approaches zero, and a
                // stopped message reading 0.4 Hz is worse than one reading 0.
                if (!qFuzzyIsNull(m_rateByKey.value(key, 0.0))) {
                    m_rateByKey.insert(key, 0.0);
                    changed = true;
                }
                continue;
            }
        } else {
            m_emptyWindowsByKey.remove(key);
        }

        const double instant = double(seen) / seconds;
        const bool hadRate = m_rateByKey.contains(key);
        const double previous = m_rateByKey.value(key, instant);
        const double smoothed = hadRate ? previous + RateSmoothing * (instant - previous)
                                        : instant;
        if (!hadRate || !qFuzzyCompare(smoothed + 1.0, previous + 1.0)) {
            m_rateByKey.insert(key, smoothed);
            changed = true;
        }
    }

    if (!changed || m_count <= 0)
        return;

    // One spanning emit rather than one per row: QQmlDelegateModel intersects
    // the range with the delegates it actually created, so the cost tracks
    // what is on screen, not how many identities exist.
    emit dataChanged(index(0), index(m_count - 1), { FrequencyRole });
}

void CanLogModel::reindexRows()
{
    m_rowByKey.clear();
    if (!m_overwriteMode)
        return;
    m_rowByKey.reserve(m_count);
    for (int row = 0; row < m_count; ++row)
        m_rowByKey.insert(frameKey(frameAt(row)), row);
}

void CanLogModel::setOverwriteMode(bool enabled)
{
    if (m_overwriteMode == enabled)
        return;
    m_overwriteMode = enabled;

    if (!enabled) {
        // Off: drop the index and resume appending. The rows already collected
        // stay - they are real frames, and throwing them away because the user
        // changed how the NEXT ones are displayed would be destroying data to
        // answer a question about presentation.
        m_rowByKey.clear();
        emit overwriteModeChanged();
        return;
    }

    // On: collapse what is already there to the last frame per identity.
    //
    // The alternative - leave the history and only overwrite identities seen
    // from now on - produces a list that is half chronological and half live,
    // where the same id appears both as an old row and as a live one. Nobody
    // can read that. Collapsing makes the mode mean one thing.
    std::vector<CanFrame> collapsed;
    QHash<quint64, int> seen;
    collapsed.reserve(size_t(qMin(m_count, m_capacity)));
    for (int row = 0; row < m_count; ++row) {
        const CanFrame &frame = frameAt(row);
        const quint64 key = frameKey(frame);
        const auto it = seen.constFind(key);
        if (it != seen.constEnd())
            collapsed[size_t(it.value())] = frame;      // newest wins
        else {
            seen.insert(key, int(collapsed.size()));
            collapsed.push_back(frame);
        }
    }

    beginResetModel();
    m_start = 0;
    m_count = int(collapsed.size());
    for (size_t i = 0; i < collapsed.size(); ++i)
        m_ring[i] = collapsed[i];
    reindexRows();
    endResetModel();

    emit overwriteModeChanged();
    emit countChanged();
}

// ============================================================ disk streaming

void CanLogModel::ensureWriterThread()
{
    if (!m_writerThread) {
        m_writerThread = new QThread(this);
        m_writerThread->setObjectName(QStringLiteral("can-trace-writer"));
        m_writer = new TraceStreamWriter;        // no parent: moved to the thread
        m_writer->moveToThread(m_writerThread);
        // deleteLater on the writer's own thread, so its QFile is closed there.
        connect(m_writerThread, &QThread::finished, m_writer, &QObject::deleteLater);

        // Every one of these crosses a thread boundary, so they are all queued
        // by Qt automatically - which is exactly what keeps disk I/O off the
        // device thread and signal handling on the GUI thread.
        connect(this, &CanLogModel::framesForDisk, m_writer, &TraceStreamWriter::append);
        connect(m_writer, &TraceStreamWriter::opened, this,
                [this](bool ok, const QString &openedPath, const QString &error) {
            if (ok) {
                m_streamPath = openedPath;
                m_streaming = true;
            } else {
                m_streaming = false;
                m_feedingWriter.store(false, std::memory_order_relaxed);
                m_streamPath.clear();
                emit streamFailed(error);
            }
            emit streamingChanged();
        });
        connect(m_writer, &TraceStreamWriter::progress, this,
                [this](qint64 frames, qint64 bytes) {
            m_streamFrames = frames;
            m_streamBytes = bytes;
            emit streamProgress();
        });
        connect(m_writer, &TraceStreamWriter::stateChanged, this, [this](int state) {
            if (m_triggerState == state)
                return;
            m_triggerState = state;
            emit triggerStateChanged();
        });
        connect(m_writer, &TraceStreamWriter::triggerFired, this,
                &CanLogModel::triggerFired);
        connect(m_writer, &TraceStreamWriter::triggerStopped, this,
                &CanLogModel::triggerStopped);
        connect(m_writer, &TraceStreamWriter::filtered, this, [this](qint64 frames) {
            if (m_filteredFrames == frames)
                return;
            m_filteredFrames = frames;
            emit framesFiltered(frames);
        });

        connect(m_writer, &TraceStreamWriter::closed, this,
                [this](const QString &closedPath, qint64 frames, qint64 bytes) {
            // Both flags, and this one first: a stop condition closes the file
            // on the writer thread without anyone asking, and until the tap is
            // shut the device thread keeps posting batches at an object that
            // has nothing to do with them.
            m_feedingWriter.store(false, std::memory_order_relaxed);
            m_streaming = false;
            m_streamPath.clear();
            m_streamFrames = frames;
            m_streamBytes = bytes;
            emit streamingChanged();
            emit streamProgress();
            emit streamClosed(closedPath, frames, bytes);
        });

        m_writerThread->start();
    }
}

bool CanLogModel::openWriter(const QUrl &fileUrl, int format, bool armed)
{
    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();
    if (path.isEmpty()) {
        emit streamFailed(tr("“%1” is not a local file.").arg(fileUrl.toString()));
        return false;
    }

    if (m_streaming)
        stopBackgroundLog();

    ensureWriterThread();

    const int resolved = format >= 0
                       ? format
                       : LogExporter::instance()
                           ? LogExporter::instance()->formatForSuffix(QFileInfo(path).suffix())
                           : int(LogExporter::Csv);

    m_streamFrames = 0;
    m_streamBytes = 0;
    // Optimistic, so the button flips the moment the user clicks it; the
    // opened() handler corrects it if the file would not open. Set BEFORE the
    // invoke, or the first batches would slip past the gate in ingestForDisk()
    // while the open is still in flight.
    m_streaming = true;
    m_streamPath = path;
    m_feedingWriter.store(true, std::memory_order_relaxed);
    emit streamingChanged();

    // Before either open, so the very first batch is already filtered.
    QMetaObject::invokeMethod(m_writer, [this, filter = m_logFilter] {
        m_writer->setFilter(filter);
    }, Qt::QueuedConnection);

    if (!armed) {
        QMetaObject::invokeMethod(m_writer, "start", Qt::QueuedConnection,
                                  Q_ARG(QString, path), Q_ARG(int, resolved),
                                  Q_ARG(QStringList, m_channelNames));
        return true;
    }

    // Resolved here, on the GUI thread, so the writer never touches the name
    // tables - it gets an integer key and a borrowed decoder pointer.
    refreshTriggerKeys();

    const qint64 preTriggerNs = qint64(qBound(0.0, m_preTriggerSeconds, 60.0) * 1e9);
    QMetaObject::invokeMethod(m_writer, [this, path, resolved, preTriggerNs] {
        m_writer->arm(path, resolved, m_channelNames, preTriggerNs,
                      m_startTrigger, m_stopTrigger, m_decoder);
    }, Qt::QueuedConnection);
    return true;
}

bool CanLogModel::startBackgroundLog(const QUrl &fileUrl, int format)
{
    return openWriter(fileUrl, format, false);
}

bool CanLogModel::armBackgroundLog(const QUrl &fileUrl, int format)
{
    // With nothing to wait for this IS a plain start, and saying so here keeps
    // the writer from opening a file it would immediately promote anyway.
    return openWriter(fileUrl, format, m_startTrigger.isActive());
}

// -------------------------------------------------------------- trigger setup

void CanLogModel::setDecoder(const CanDecoder *decoder)
{
    m_decoder = decoder;
    refreshTriggerKeys();
}

void CanLogModel::setPreTriggerSeconds(double seconds)
{
    const double clamped = qBound(0.0, seconds, 60.0);
    if (qFuzzyCompare(m_preTriggerSeconds + 1.0, clamped + 1.0))
        return;
    m_preTriggerSeconds = clamped;
    emit triggerConfigChanged();
}

void CanLogModel::refreshTriggerKeys()
{
    // A linear scan of the decode table, run when the setup changes or the DBC
    // is reloaded - never per frame. CanDecoder indexes by key, not by name,
    // so this is the only direction the lookup goes.
    const auto resolve = [this](TriggerCondition &condition) {
        if (condition.kind != TriggerCondition::SignalValue) {
            condition.signalKey = -1;
            return;
        }
        condition.signalKey = -1;
        if (!m_decoder)
            return;
        const int total = m_decoder->signalCount();
        for (int key = 0; key < total; ++key) {
            if (m_decoder->signalName(key) != condition.signalName)
                continue;
            // The message name disambiguates: the same signal name appears in
            // two messages often enough that picking the first is a coin toss.
            if (!condition.messageName.isEmpty()
                && m_decoder->messageName(key) != condition.messageName)
                continue;
            condition.signalKey = key;
            return;
        }
    };
    resolve(m_startTrigger);
    resolve(m_stopTrigger);
}

bool CanLogModel::setTrigger(bool isStart, int kind, quint32 frameId, bool extended,
                             const QString &messageName, const QString &signalName,
                             int comparison, double threshold)
{
    TriggerCondition condition;
    condition.kind = TriggerCondition::Kind(qBound(0, kind, int(TriggerCondition::SignalValue)));
    condition.frameId = frameId;
    condition.extended = extended;
    condition.messageName = messageName;
    condition.signalName = signalName;
    condition.comparison =
            TriggerCondition::Comparison(qBound(0, comparison, int(TriggerCondition::NotEqual)));
    condition.threshold = threshold;

    (isStart ? m_startTrigger : m_stopTrigger) = condition;
    refreshTriggerKeys();
    emit triggerConfigChanged();

    // Stored either way - see the header. A DBC loaded after the trigger was
    // set makes it live without the user retyping anything.
    return condition.kind != TriggerCondition::SignalValue
           || (isStart ? m_startTrigger : m_stopTrigger).signalKey >= 0;
}

void CanLogModel::setLogFilter(int mode, quint32 startId, quint32 endId,
                               int byteIndex, int payloadMask, int payloadMatch)
{
    LogFilter filter;
    filter.mode = LogFilter::Mode(qBound(0, mode, int(LogFilter::PayloadMask)));
    filter.startId = startId;
    filter.endId = endId;
    filter.byteIndex = qBound(0, byteIndex, 7);
    filter.payloadMask = quint8(payloadMask & 0xFF);
    filter.payloadMatch = quint8(payloadMatch & 0xFF);
    m_logFilter = filter;

    // Live: a capture already running narrows from here on rather than making
    // the user stop and start again to change what it collects.
    if (m_writer) {
        QMetaObject::invokeMethod(m_writer, [this, filter] {
            m_writer->setFilter(filter);
        }, Qt::QueuedConnection);
    }
    emit triggerConfigChanged();
}

QVariantMap CanLogModel::logFilter() const
{
    return QVariantMap {
        { QStringLiteral("mode"),         int(m_logFilter.mode) },
        { QStringLiteral("startId"),      m_logFilter.startId },
        { QStringLiteral("endId"),        m_logFilter.endId },
        { QStringLiteral("byteIndex"),    m_logFilter.byteIndex },
        { QStringLiteral("payloadMask"),  int(m_logFilter.payloadMask) },
        { QStringLiteral("payloadMatch"), int(m_logFilter.payloadMatch) }
    };
}

QString CanLogModel::filterTriggerConflict() const
{
    if (!m_logFilter.isActive() || m_startTrigger.kind != TriggerCondition::IdMatch)
        return QString();

    // Only decidable for an id trigger. A signal-value trigger names a signal,
    // not an id, and answering "can this ever fire" would mean resolving the
    // signal back to its message id - true, but it would also make this
    // function lie whenever the DBC is not loaded. Silence is better than a
    // warning that appears and disappears with a file.
    CanFrame probe;
    probe.id = m_startTrigger.frameId;
    probe.extended = m_startTrigger.extended;
    probe.dlc = 8;
    probe.payload.fill(0);
    if (m_logFilter.mode == LogFilter::PayloadMask && m_startTrigger.frameId == m_logFilter.startId)
        return QString();          // the payload decides, and we cannot know it here

    if (!m_logFilter.allows(probe)) {
        return tr("The pass filter excludes 0x%1, which is the id the start trigger "
                  "waits for - the capture would never begin.")
               .arg(m_startTrigger.frameId, 0, 16);
    }
    return QString();
}

void CanLogModel::clearTriggers()
{
    m_startTrigger = TriggerCondition {};
    m_stopTrigger = TriggerCondition {};
    emit triggerConfigChanged();
}

QVariantMap CanLogModel::triggerConfig() const
{
    const auto pack = [](const TriggerCondition &c) {
        return QVariantMap {
            { QStringLiteral("kind"),        int(c.kind) },
            { QStringLiteral("frameId"),     c.frameId },
            { QStringLiteral("extended"),    c.extended },
            { QStringLiteral("messageName"), c.messageName },
            { QStringLiteral("signalName"),  c.signalName },
            { QStringLiteral("comparison"),  int(c.comparison) },
            { QStringLiteral("threshold"),   c.threshold },
            { QStringLiteral("resolved"),    c.kind != TriggerCondition::SignalValue
                                             || c.signalKey >= 0 }
        };
    };
    return QVariantMap {
        { QStringLiteral("preTriggerSeconds"), m_preTriggerSeconds },
        { QStringLiteral("start"), pack(m_startTrigger) },
        { QStringLiteral("stop"),  pack(m_stopTrigger) }
    };
}

QStringList CanLogModel::decoderSignalNames() const
{
    QStringList out;
    if (!m_decoder)
        return out;
    const int total = m_decoder->signalCount();
    out.reserve(total);
    for (int key = 0; key < total; ++key) {
        const QString message = m_decoder->messageName(key);
        const QString name = m_decoder->signalName(key);
        if (name.isEmpty())
            continue;
        out.append(message.isEmpty() ? name : message + QLatin1Char('.') + name);
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

void CanLogModel::stopBackgroundLog()
{
    if (!m_writer || !m_streaming)
        return;
    // Gate the tap immediately: append() on a closing file is harmless but
    // pointless, and the button should not stay lit while the footer is
    // written.
    m_streaming = false;
    m_feedingWriter.store(false, std::memory_order_relaxed);
    emit streamingChanged();
    QMetaObject::invokeMethod(m_writer, "stop", Qt::QueuedConnection);
}

// ------------------------------------------------------------- QML helpers

QVariantMap CanLogModel::get(int row) const
{
    QVariantMap map;
    if (row < 0 || row >= m_count)
        return map;

    const QModelIndex idx = index(row);
    const auto roles = roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        map.insert(QString::fromUtf8(it.value()), data(idx, it.key()));
    return map;
}

void CanLogModel::clear()
{
    {
        QMutexLocker locker(&m_pendingMutex);
        m_pending.clear();
    }

    beginResetModel();
    m_start = 0;
    m_count = 0;
    m_droppedCount = 0;
    m_totalCount = 0;
    m_rowByKey.clear();     // every cached row index just stopped existing
    m_countByKey.clear();   // Clear means "this session starts now"
    m_rateByKey.clear();
    m_windowCountByKey.clear();
    m_emptyWindowsByKey.clear();
    m_rateWindow.restart();
    m_maxCount = 0;
    endResetModel();

    emit countChanged();
    emit statisticsChanged();
}

QString CanLogModel::toCsv(int maxRows) const
{
    const int rows = (maxRows < 0) ? m_count : qMin(m_count, maxRows);

    QString out;
    out.reserve(rows * 64);
    out += QStringLiteral("Time,Dir,Ch,ID,Type,DLC,Data\n");
    for (int row = 0; row < rows; ++row) {
        const CanFrame &frame = frameAt(row);
        out += QStringLiteral("%1,%2,%3,%4,%5,%6,%7\n")
               .arg(frame.seconds(), 0, 'f', 6)
               .arg(frame.direction == CanFrame::Tx ? QStringLiteral("Tx") : QStringLiteral("Rx"))
               .arg(m_channelNames.value(frame.channel, QStringLiteral("CAN?")))
               .arg(frame.idText())
               .arg(frame.extended ? QStringLiteral("Ext") : QStringLiteral("Std"))
               .arg(frame.dlc)
               .arg(frame.payloadText());
    }
    return out;
}
