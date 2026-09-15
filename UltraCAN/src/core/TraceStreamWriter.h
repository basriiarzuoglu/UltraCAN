#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QStringList>
#include <QVector>

#include "core/CanFrame.h"
#include "core/LogFilter.h"
#include "core/TriggerCondition.h"

#include <deque>

/*!
    \class TraceStreamWriter
    \brief Appends frames to a trace file while the bus is running.

    \section1 Not the same job as LogExporter

    \c LogExporter writes what the ring buffer HAS: a snapshot, formatted and
    written once. This writes what the bus DOES: an open file that grows for as
    long as the user leaves it running, past the ring's 20 000-frame cap and
    past the point where the ring has started dropping. The two coexist because
    neither can do the other's job - you cannot snapshot frames the ring
    already evicted, and you cannot stream a capture that finished before you
    pressed the button.

    Both use \c TraceFormat, so a streamed file and an exported file of the
    same traffic are byte-identical apart from their headers.

    \section1 Which thread

    Its own. Frames reach \c CanLogModel::enqueue() on the DEVICE thread, and
    that thread is the one draining the driver's receive queue - a write() into
    a filesystem that stalls (a network share, a sleeping disk, a full buffer
    cache) would stall the CAN read loop behind it and drop frames on the wire,
    which is the one failure this feature exists to prevent.

    So the model hands batches over with a QUEUED connection and returns
    immediately. This object lives on a \c QThread of its own and does every
    open, write, flush and close there.

    \section1 Buffering

    Two levels, and both are needed:

    \list
    \li Frames are formatted into an in-memory \c QByteArray and written to the
        file only when it passes \c TraceFormat::ChunkFlushBytes. Formatting is
        cheap; a syscall per frame is not.
    \li \c QFile::flush() - the syscall that actually pushes the buffer at the
        OS - runs on a timer, not per frame. Per-frame flushing at 5 000
        frames/s is 5 000 syscalls a second and is what the performance
        constraint is really about.
    \endlist

    The interval is a compromise with the other failure mode: a technician who
    pulls the USB cable, or an application that is killed, keeps everything
    already flushed. Half a second of traffic is an acceptable loss; a whole
    session is not.
*/
class TraceStreamWriter : public QObject
{
    Q_OBJECT

public:
    explicit TraceStreamWriter(QObject *parent = nullptr);
    ~TraceStreamWriter() override;

    /*! How often the OS buffer is pushed to disk. See the class note. */
    static constexpr int FlushIntervalMs = 500;

    /*!
        Hard ceiling on the pre-trigger ring, independent of the time window.

        The window is enforced on frame TIMESTAMPS, and a device whose clock
        reports zero (or repeats) would make "older than newest - window" true
        for nothing at all, so the deque would grow until the process died.
        At 200 000 frames this caps the ring at a few megabytes whatever the
        timestamps say.
    */
    static constexpr int MaxPreTriggerFrames = 200000;

    /*! Idle: nothing is being written and nothing is being kept. Armed: frames
        go into the ring only. Recording: frames go to disk. */
    enum State { Idle = 0, Armed, Recording };
    Q_ENUM(State)

public slots:
    /*!
        Opens \a path and writes the format header. \a format is a
        \c LogExporter::Format value.

        Runs on the writer thread. Reports through \l opened rather than a
        return value, because the caller is on another thread and cannot wait
        for one.
    */
    void start(const QString &path, int format, const QStringList &channelNames);
    /*!
        Sets the pass filter. Applied to every frame BEFORE the ring and before
        the triggers - see LogFilter. Takes effect on the next batch; changing
        it mid-capture is allowed and simply narrows what the rest of the file
        contains.
    */
    void setFilter(const LogFilter &filter);

    /*!
        Opens \a path and waits: incoming frames go into the pre-trigger ring
        instead of the file until \a startCondition fires.

        \a preTriggerNs is the length of the ring in bus-clock nanoseconds.
        \a decoder may be null, in which case a SignalValue condition never
        fires - it is borrowed, not owned, and must outlive this object.

        The file is opened NOW rather than at the trigger, deliberately: a path
        that cannot be written is a mistake the user must hear about while they
        are still looking at the dialog, not forty minutes into a wait for an
        event that then has nowhere to go. The cost is a header-only file if
        the trigger never comes, and \l stop() deletes that file rather than
        leaving a stub that looks like a failed capture.
    */
    void arm(const QString &path, int format, const QStringList &channelNames,
             qint64 preTriggerNs,
             const TriggerCondition &startCondition,
             const TriggerCondition &stopCondition,
             const CanDecoder *decoder);
    /*! Formats \a frames and buffers them. Never blocks on the disk. */
    void append(const QVector<CanFrame> &frames);
    /*! Writes the footer, flushes and closes. Safe to call when not open. */
    void stop();

signals:
    void opened(bool ok, const QString &path, const QString &error);
    /*! \a state is a \l State value. Queued to the GUI thread. */
    void stateChanged(int state);
    /*! The start condition fired at \a timestampNs, and \a preTriggerFrames
        frames of history were written ahead of it. */
    void triggerFired(qint64 timestampNs, int preTriggerFrames);
    /*! The stop condition fired; the file is closed and the state is Idle. */
    void triggerStopped(qint64 timestampNs);
    /*! Periodic progress, for the status strip. */
    void progress(qint64 frames, qint64 bytes);
    void closed(const QString &path, qint64 frames, qint64 bytes);
    /*! Periodic count of frames the filter rejected. */
    void filtered(qint64 frames);

private:
    void timerEvent(QTimerEvent *event) override;
    /*! Pushes the accumulator into the file. Returns false on a write error,
        having already closed and reported it. */
    bool drain(bool force);
    void fail(const QString &message);
    /*! Formats one frame into the accumulator. Does NOT flush. */
    void writeFrame(const CanFrame &frame);
    /*! Pushes \a frame into the ring and evicts everything older than the
        window. */
    void remember(const CanFrame &frame);
    void setState(State state);

    QFile m_file;
    QByteArray m_buffer;
    int m_format = 0;
    QStringList m_channelNames;
    /*! 1-based and continuous for the whole file - .trc numbers its rows, and
        restarting the count per batch would produce a file no reader trusts. */
    int m_messageNumber = 0;
    qint64 m_frames = 0;
    qint64 m_bytes = 0;
    int m_flushTimer = 0;
    bool m_failed = false;

    // -- triggering. Touched on the writer thread only.
    State m_state = Idle;
    std::deque<CanFrame> m_preTrigger;
    qint64 m_preTriggerNs = 0;
    TriggerCondition m_start;
    TriggerCondition m_stop;
    const CanDecoder *m_decoder = nullptr;
    LogFilter m_filter;
    /*! Frames the filter rejected, for the status line - a filter that blocks
        everything looks exactly like a dead bus otherwise. */
    qint64 m_filtered = 0;
    /*! Reused by TriggerCondition::matches() so evaluation allocates nothing. */
    QVector<DecodedSample> m_scratch;
    /*! True while the file holds a header and nothing else, so stop() knows it
        can delete it instead of leaving a stub. */
    bool m_emptyCapture = false;
    /*! Set while arm() is borrowing start(), so start() does not announce
        Recording for the instant before arm() sets Armed - the UI would show
        one frame of red before turning yellow. */
    bool m_arming = false;
};
