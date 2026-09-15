#include "TraceStreamWriter.h"

#include <QFileInfo>
#include <QTimerEvent>

#include "core/LogExporter.h"
#include "core/TraceFormat.h"

TraceStreamWriter::TraceStreamWriter(QObject *parent)
    : QObject(parent)
{
}

TraceStreamWriter::~TraceStreamWriter()
{
    // Not stop(): that emits, and a signal from a destructor reaches slots on
    // an object that is half gone. Just get the bytes onto the disk.
    if (m_file.isOpen()) {
        if (!m_buffer.isEmpty())
            m_file.write(m_buffer);
        m_file.flush();
        m_file.close();
    }
}

void TraceStreamWriter::start(const QString &path, int format, const QStringList &channelNames)
{
    stop();                       // a second Start replaces the first file

    m_format = format;
    m_channelNames = channelNames;
    m_messageNumber = 0;
    m_frames = 0;
    m_bytes = 0;
    m_failed = false;
    m_buffer.clear();

    m_file.setFileName(path);
    // NOT QSaveFile. QSaveFile only materialises the destination on commit(),
    // so a session killed mid-capture would leave nothing at all - and the
    // whole point of streaming is that what has already been written survives.
    // A plain QFile trades atomicity for exactly that: whatever reached the
    // disk is on the disk, and a truncated trace is still a trace.
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit opened(false, path, m_file.errorString());
        return;
    }

    QByteArray header;
    switch (m_format) {
    case LogExporter::Asc: TraceFormat::appendAscHeader(header); break;
    case LogExporter::Trc: TraceFormat::appendTrcHeader(header); break;
    default:               TraceFormat::appendCsvHeader(header); break;
    }
    if (m_file.write(header) != header.size()) {
        const QString error = m_file.errorString();
        m_file.close();
        emit opened(false, path, error);
        return;
    }
    m_bytes += header.size();
    m_file.flush();               // once, so the file is non-empty immediately

    m_flushTimer = startTimer(FlushIntervalMs);
    m_emptyCapture = false;
    // start() is the untriggered path: recording from the first frame, which
    // is what it has always meant. arm() borrows this function and sets Armed
    // itself the moment it returns.
    if (!m_arming)
        setState(Recording);
    emit opened(true, path, QString());
}

void TraceStreamWriter::arm(const QString &path, int format,
                            const QStringList &channelNames, qint64 preTriggerNs,
                            const TriggerCondition &startCondition,
                            const TriggerCondition &stopCondition,
                            const CanDecoder *decoder)
{
    // Everything start() does - open, header, flush timer - and then one
    // difference: the state it leaves behind.
    m_preTrigger.clear();
    m_preTriggerNs = qMax(qint64(0), preTriggerNs);
    m_start = startCondition;
    m_stop = stopCondition;
    m_decoder = decoder;

    m_arming = true;
    start(path, format, channelNames);
    m_arming = false;
    if (!m_file.isOpen())
        return;                    // start() already reported the failure

    // A start condition of None would mean "wait forever", which is not a
    // thing a Start button may do. Treat it as "record now" and stay in
    // Recording, so the button the user pressed does what it says.
    if (!m_start.isActive()) {
        setState(Recording);
        return;
    }

    m_emptyCapture = true;
    setState(Armed);
}

void TraceStreamWriter::setFilter(const LogFilter &filter)
{
    m_filter = filter;
}

void TraceStreamWriter::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(int(state));
}

void TraceStreamWriter::remember(const CanFrame &frame)
{
    // A zero window means "the file starts at the trigger", which is what the
    // dialog promises. Without this the eviction test - strictly older than
    // (newest - 0) - keeps the newest frame, so a 0 s window would still write
    // one frame of history.
    if (m_preTriggerNs <= 0)
        return;

    m_preTrigger.push_back(frame);

    // Evicted against the NEWEST frame rather than a wall clock: the window is
    // "the last N seconds of traffic", and on a replayed log the bus clock and
    // the wall clock have nothing to do with each other.
    const qint64 cutoff = frame.timestampNs - m_preTriggerNs;
    while (!m_preTrigger.empty() && m_preTrigger.front().timestampNs < cutoff)
        m_preTrigger.pop_front();

    // The safety net for a device with a broken clock - see the header.
    while (int(m_preTrigger.size()) > MaxPreTriggerFrames)
        m_preTrigger.pop_front();
}

void TraceStreamWriter::writeFrame(const CanFrame &frame)
{
    switch (m_format) {
    case LogExporter::Asc:
        TraceFormat::appendAscFrame(m_buffer, frame);
        break;
    case LogExporter::Trc:
        // The counter is a member, not a loop index: .trc numbers rows
        // continuously for the whole file, and a per-batch restart would
        // produce repeated numbers that no reader trusts.
        TraceFormat::appendTrcFrame(m_buffer, frame, ++m_messageNumber);
        break;
    default:
        TraceFormat::appendCsvFrame(m_buffer, frame, m_channelNames);
        break;
    }
    ++m_frames;
}

void TraceStreamWriter::append(const QVector<CanFrame> &frames)
{
    if (!m_file.isOpen() || m_failed || frames.isEmpty() || m_state == Idle)
        return;

    // Frame by frame rather than batch by batch, because a trigger can fire in
    // the MIDDLE of a batch: the frames before it belong to the pre-trigger
    // ring and the frames after it belong to the file, and a batch-level
    // decision would put all of them on the same side of the event.
    for (const CanFrame &frame : frames) {
        // THE FILTER GATE, and it is first on purpose. A rejected frame does
        // not enter the ring, does not reach a trigger, and does not reach the
        // file - see LogFilter for why a filtered frame must not be allowed to
        // fire the start condition.
        if (!m_filter.allows(frame)) {
            ++m_filtered;
            continue;
        }

        if (m_state == Armed) {
            if (!m_start.matches(frame, m_decoder, m_scratch)) {
                remember(frame);
                continue;
            }

            // Fired. The history goes down first, in arrival order, then the
            // frame that fired it - so the file reads as one continuous
            // recording with the event in the middle rather than at the top.
            const int history = int(m_preTrigger.size());
            for (const CanFrame &past : m_preTrigger)
                writeFrame(past);
            m_preTrigger.clear();
            m_preTrigger.shrink_to_fit();
            m_emptyCapture = false;

            setState(Recording);
            emit triggerFired(frame.timestampNs, history);
            // falls through to the Recording branch for this same frame
        }

        writeFrame(frame);

        if (m_stop.isActive() && m_stop.matches(frame, m_decoder, m_scratch)) {
            // The frame that stopped it is IN the file: a stop condition marks
            // the end of what you wanted to see, not the last thing you are
            // allowed to see.
            const qint64 when = frame.timestampNs;
            stop();
            emit triggerStopped(when);
            return;
        }
    }

    // Size-triggered only. The clock-triggered flush is the timer's job, and
    // doing both here would put a syscall back on the per-batch path.
    if (m_buffer.size() >= TraceFormat::ChunkFlushBytes)
        drain(false);
}

void TraceStreamWriter::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_flushTimer && m_filter.isActive())
        emit filtered(m_filtered);

    if (event->timerId() != m_flushTimer) {
        QObject::timerEvent(event);
        return;
    }
    if (!m_file.isOpen() || m_failed)
        return;

    const qint64 before = m_bytes;
    if (drain(true) && m_bytes != before)
        emit progress(m_frames, m_bytes);
}

bool TraceStreamWriter::drain(bool force)
{
    if (!m_file.isOpen() || m_buffer.isEmpty()) {
        // Nothing buffered, but the OS may still be holding the last chunk.
        if (force && m_file.isOpen())
            m_file.flush();
        return true;
    }

    const qint64 written = m_file.write(m_buffer);
    if (written != m_buffer.size()) {
        fail(m_file.errorString());
        return false;
    }
    m_bytes += written;
    m_buffer.clear();

    // write() only reaches the OS buffer. flush() is the part that costs, so
    // it happens on the timer's schedule - see the class note.
    if (force)
        m_file.flush();
    return true;
}

void TraceStreamWriter::fail(const QString &message)
{
    m_failed = true;
    const QString path = m_file.fileName();
    if (m_flushTimer) {
        killTimer(m_flushTimer);
        m_flushTimer = 0;
    }
    m_file.close();
    // Reported through opened(false, ...) rather than a separate signal: from
    // the UI's point of view the session has ended and the reason is the same
    // string it would have shown had the open failed.
    emit opened(false, path, message);
}

void TraceStreamWriter::stop()
{
    if (m_flushTimer) {
        killTimer(m_flushTimer);
        m_flushTimer = 0;
    }
    if (!m_file.isOpen())
        return;

    if (!m_failed) {
        // .asc is the only one of the three with a terminator, and a reader
        // that wants it will not accept the file without it.
        if (m_format == LogExporter::Asc)
            TraceFormat::appendAscFooter(m_buffer);
        drain(true);
    }

    const QString path = m_file.fileName();
    m_file.close();

    // Armed, never fired: the file holds a header and nothing else. Deleting
    // it is kinder than leaving a stub the user will open next week and read
    // as a capture that lost its data.
    const bool discarded = m_emptyCapture && !m_failed;
    if (discarded)
        QFile::remove(path);

    emit closed(path, discarded ? 0 : m_frames, discarded ? 0 : m_bytes);

    m_buffer.clear();
    m_frames = 0;
    m_bytes = 0;
    m_messageNumber = 0;
    m_emptyCapture = false;
    m_filtered = 0;
    m_preTrigger.clear();
    m_preTrigger.shrink_to_fit();
    setState(Idle);
}
