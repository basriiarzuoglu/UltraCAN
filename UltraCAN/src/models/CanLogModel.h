#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QMutex>
#include <QStringList>
#include <QElapsedTimer>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>   // QML_ELEMENT / QML_UNCREATABLE

#include <atomic>
#include <vector>

#include "core/CanFrame.h"
#include "core/LogFilter.h"
#include "core/TriggerCondition.h"

class QThread;
class TraceStreamWriter;

/*!
    \brief Bounded, thread-safe trace model behind the CAN Logger view.

    Replaces CanLogDummyModel. Two properties of the design matter:

    \b {Ring buffer}. The model never grows past \l capacity; once full the
    oldest frames are removed. An unbounded trace model is the classic way a
    logger eats all available memory in minutes.

    \b {Producer/consumer}. Frames arrive from the driver thread through
    \l enqueue(), which only takes a mutex and appends to a staging vector.
    A single queued invocation then flushes the staging vector into the model
    on the GUI thread inside ONE beginInsertRows()/endInsertRows() pair.
    Because a flush is only scheduled when none is pending, bursts coalesce
    automatically: the faster the bus, the bigger the batches, and the model
    signal count stays proportional to the event loop, not to the frame rate.
    Emitting one insert per frame at 10 kframes/s would melt the view.

    Role names match LogRow.qml exactly. Note \c payload, NOT \c data - a role
    called \c data shadows Item's default property inside a delegate.
*/
class CanLogModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Use CanBusManager.logModel")

    Q_PROPERTY(int count READ count NOTIFY countChanged FINAL)
    Q_PROPERTY(int capacity READ capacity WRITE setCapacity NOTIFY capacityChanged FINAL)
    Q_PROPERTY(qint64 droppedCount READ droppedCount NOTIFY statisticsChanged FINAL)
    Q_PROPERTY(qint64 totalCount READ totalCount NOTIFY statisticsChanged FINAL)

    /*!
        One row per frame IDENTITY instead of one row per frame.

        For watching a handful of periodic frames this is the only readable
        mode: twenty rows updating in place, rather than twenty rows a second
        scrolling past. It is not a filter and not a view - the ring really
        holds one entry per identity while it is on, which is also why the
        trace stops being a chronological record and \l overwriteMode is the
        reason to stream to disk rather than rely on Save Log.

        Identity is (id, extended, channel), not the id alone - see
        \l frameKey(). */
    Q_PROPERTY(bool overwriteMode READ overwriteMode WRITE setOverwriteMode
               NOTIFY overwriteModeChanged FINAL)
    /*! Highest CountRole on screen, so the view can scale a bar if it wants
        one, and so "is anything actually arriving" is answerable at a glance. */
    Q_PROPERTY(qint64 maxCount READ maxCount NOTIFY statisticsChanged FINAL)

    /*! True while a disk stream is open. */
    Q_PROPERTY(bool streaming READ isStreaming NOTIFY streamingChanged FINAL)
    /*! Absolute path of the open stream, empty when not streaming. */
    Q_PROPERTY(QString streamPath READ streamPath NOTIFY streamingChanged FINAL)
    Q_PROPERTY(qint64 streamFrames READ streamFrames NOTIFY streamProgress FINAL)
    Q_PROPERTY(qint64 streamBytes READ streamBytes NOTIFY streamProgress FINAL)
    /*! A TraceStreamWriter::State value: 0 idle, 1 armed, 2 recording. The one
        thing the toolbar button binds to - "waiting" and "recording" are not
        the same button in a different colour, they are different states. */
    Q_PROPERTY(int triggerState READ triggerState NOTIFY triggerStateChanged FINAL)
    Q_PROPERTY(double preTriggerSeconds READ preTriggerSeconds
               WRITE setPreTriggerSeconds NOTIFY triggerConfigChanged FINAL)

public:
    enum Role {
        TimeRole = Qt::UserRole + 1,
        DirectionRole,
        ChannelRole,
        CanIdRole,
        IdTextRole,
        ExtendedRole,
        DlcRole,
        PayloadRole,
        NameRole,
        /*! How many frames of this row's identity have been seen this session.

            Counted for EVERY frame in both modes, not only in overwrite mode.
            The number belongs to the bus, not to the display: switching modes
            must not reset it, and in chronological mode "this is the 412th
            0x100" is still the useful reading. */
        CountRole,
        /*! Arrival rate in Hz for this row's identity, smoothed and decaying.

            NOT the literal 1/dt of the last two frames. That is a single-sample
            estimate: one late frame and a steady 100 Hz message reads 62 Hz,
            and - the part that actually misleads - a node that stops
            transmitting keeps displaying its last rate forever, because no
            frame ever arrives to update it. The same two problems were already
            solved in DbcFlatModel's live-rate column; this uses the same shape.
            See m_rateByKey and the note above noteArrival(). */
        FrequencyRole
    };
    Q_ENUM(Role)

    explicit CanLogModel(QObject *parent = nullptr);
    ~CanLogModel() override;

    // -- QAbstractListModel ---------------------------------------------------
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // -- properties -----------------------------------------------------------
    int count() const;
    int capacity() const;
    void setCapacity(int capacity);
    qint64 droppedCount() const { return m_droppedCount; }
    qint64 totalCount() const { return m_totalCount; }

    /*! Names shown in the Ch column; index = CanFrame::channel. */
    void setChannelNames(const QStringList &names);
    QStringList channelNames() const { return m_channelNames; }

    /*!
        \brief GUI-THREAD ONLY. Flat, chronological copy of the whole trace.

        The export path's foundation. The ring cannot be walked from a worker
        thread: a flush on the GUI thread rotates \c m_start and can overwrite
        the slot the worker is reading, so a background iteration would produce
        a file with duplicated and missing frames and no error anywhere.

        Copying first removes the problem instead of locking around it. CanFrame
        is a POD, so this is one allocation and a memcpy - about a millisecond
        for a full 20k ring - and the live acquisition is never blocked for
        longer than that. Any frames already staged by the producer are flushed
        in first, so the export contains everything received up to this instant,
        not merely everything the view has drawn.
    */
    std::vector<CanFrame> snapshot() const;

    // -- producer side (ANY thread) -------------------------------------------
    void enqueue(const CanFrame &frame);
    void enqueue(const QVector<CanFrame> &frames);

    bool overwriteMode() const { return m_overwriteMode; }
    qint64 maxCount() const { return m_maxCount; }
    void setOverwriteMode(bool enabled);

    // -- disk streaming ---------------------------------------------------------
    bool isStreaming() const { return m_streaming; }
    QString streamPath() const { return m_streamPath; }
    qint64 streamFrames() const { return m_streamFrames; }
    qint64 streamBytes() const { return m_streamBytes; }

    /*!
        Opens \a fileUrl and starts appending every frame that arrives from
        here on. \a format is a \c LogExporter::Format value, or -1 to infer
        it from the extension.

        Independent of the ring: the stream sees frames before they are
        buffered, so it keeps recording past the capacity cap and keeps a
        chronological record even while \l overwriteMode is collapsing the
        view. That is the whole point - a snapshot cannot contain frames the
        ring already dropped.

        Returns false only for a bad url; an I/O failure arrives through
        \l streamFailed, because by then the work is on the writer thread.
    */
    Q_INVOKABLE bool startBackgroundLog(const QUrl &fileUrl, int format = -1);
    /*! Writes the footer, flushes and closes. Safe when not streaming. */
    Q_INVOKABLE void stopBackgroundLog();

    // -- triggering ------------------------------------------------------------
    int triggerState() const { return m_triggerState; }
    double preTriggerSeconds() const { return m_preTriggerSeconds; }
    void setPreTriggerSeconds(double seconds);

    /*!
        Sets the start (\a isStart true) or stop condition.

        \a kind is a \c TriggerCondition::Kind, \a comparison a
        \c TriggerCondition::Comparison. For an id match only \a frameId and
        \a extended matter; for a signal value only \a messageName,
        \a signalName, \a comparison and \a threshold do.

        Returns false when a signal was named that the current decode table
        does not contain - the condition is stored anyway, so that loading the
        right DBC later makes it work without the user retyping it.
    */
    Q_INVOKABLE bool setTrigger(bool isStart, int kind, quint32 frameId, bool extended,
                                const QString &messageName, const QString &signalName,
                                int comparison, double threshold);
    Q_INVOKABLE void clearTriggers();

    // -- pass filter -----------------------------------------------------------
    /*!
        Sets what the background log is allowed to see. \a mode is a
        \c LogFilter::Mode; the rest are ignored by the modes that do not use
        them. Applies to the capture in flight as well as the next one.
    */
    Q_INVOKABLE void setLogFilter(int mode, quint32 startId, quint32 endId,
                                  int byteIndex, int payloadMask, int payloadMatch);
    Q_INVOKABLE QVariantMap logFilter() const;
    /*!
        Empty when the filter and the start trigger can both be satisfied,
        otherwise a sentence saying they cannot. A filter that excludes the very
        id the capture is waiting for means the trigger can never fire, and the
        user needs that said out loud while the dialog is open.
    */
    Q_INVOKABLE QString filterTriggerConflict() const;
    /*! Current setup, for the dialog to open showing what is already there. */
    Q_INVOKABLE QVariantMap triggerConfig() const;
    /*! "Message.Signal" for everything the live decode table knows, so the
        dialog can offer a list instead of a text field to mistype. */
    Q_INVOKABLE QStringList decoderSignalNames() const;

    /*!
        Opens \a fileUrl and waits for the start condition, keeping the last
        \l preTriggerSeconds of traffic in memory meanwhile. With no start
        condition set this is exactly \l startBackgroundLog().
    */
    Q_INVOKABLE bool armBackgroundLog(const QUrl &fileUrl, int format = -1);

    /*! Borrowed, not owned - CanBusManager's. Re-resolve the trigger keys
        whenever that manager rebuilds its table. */
    void setDecoder(const CanDecoder *decoder);
    /*! Re-resolves signal-value triggers against the current decode table.
        A key that no longer resolves goes back to -1 rather than matching
        whichever signal inherited its index. */
    void refreshTriggerKeys();

    /*!
        The background log's tap, called from \c CanBusManager::fanOut() for
        every frame on the bus.

        Deliberately NOT \l enqueue(). enqueue() sits behind the recording gate
        and behind the ring; this sits in front of both, alongside the analyzer
        and the graph sinks, which fanOut() already documents as independent of
        recording. That is what "decoupled" has to mean here: the file records
        the bus, not the view's opinion of it - unaffected by the 20 000-frame
        cap, by \l overwriteMode collapsing rows, and by whether the trace is
        listening at all.
    */
    void ingestForDisk(const QVector<CanFrame> &frames);

    // -- QML helpers ----------------------------------------------------------
    /*! Mirrors ListModel.get() so QML written against the dummy model still works. */
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE void clear();
    /*! CSV text of the whole trace (or the first \a maxRows), for the export path. */
    Q_INVOKABLE QString toCsv(int maxRows = -1) const;

signals:
    void countChanged();
    void capacityChanged();
    void statisticsChanged();
    void overwriteModeChanged();
    void streamingChanged();
    void streamProgress();
    /*! Streaming ended because of an I/O error, not because the user stopped. */
    void streamFailed(const QString &error);
    void streamClosed(const QString &path, qint64 frames, qint64 bytes);
    void triggerStateChanged();
    void triggerConfigChanged();
    /*! The start condition fired; \a preTriggerFrames of history went to disk
        ahead of it. For the status strip. */
    void triggerFired(qint64 timestampNs, int preTriggerFrames);
    void triggerStopped(qint64 timestampNs);
    /*! Running count of frames the pass filter rejected. */
    void framesFiltered(qint64 frames);

    /*! Handed to the writer thread by a QUEUED connection - never call into
        the writer directly from the device thread. */
    void framesForDisk(const QVector<CanFrame> &frames);

private:
    Q_INVOKABLE void flushPending();          //!< GUI thread only

    const CanFrame &frameAt(int row) const;
    void appendBatch(const QVector<CanFrame> &batch);
    void appendChronological(const QVector<CanFrame> &batch);
    void appendOverwriting(const QVector<CanFrame> &batch);
    /*! Counts one frame into the window currently open. */
    void noteArrival(const CanFrame &frame);
    /*! Closes the counting window: turns each identity's frame count into a
        rate, steps the ones that saw nothing towards zero, and repaints.
        On a timer because "it stopped" is the one event that produces no
        frame, so nothing else would ever notice. */
    void decayRates();
    /*! Hz for \a key, 0 when it has no usable period yet. */
    double frequencyFor(quint64 key) const;
    void scheduleFlush();

    /*! The identity a row is keyed on in \l overwriteMode.

        (id, extended, channel), packed. The id alone is not an identity: a
        standard 0x100 and an extended 0x100 are different arbitration fields,
        and the same id on two channels is two different senders. Collapsing
        either pair would make one of them permanently invisible. */
    static quint64 frameKey(const CanFrame &frame)
    {
        return (quint64(frame.id) << 32)
             | (quint64(frame.channel) << 8)
             | quint64(frame.extended ? 1 : 0);
    }
    /*! Rebuilds \l m_rowByKey from the ring. Needed after anything that moves
        rows - a reset, or an eviction. */
    void reindexRows();

    // Ring storage. m_start is the slot of row 0; m_count rows are valid.
    std::vector<CanFrame> m_ring;
    int m_start = 0;
    int m_count = 0;
    int m_capacity = 20000;

    QStringList m_channelNames { QStringLiteral("CAN1"), QStringLiteral("CAN2") };

    // Producer staging area.
    mutable QMutex m_pendingMutex;
    QVector<CanFrame> m_pending;
    std::atomic_bool m_flushScheduled { false };

    qint64 m_droppedCount = 0;
    qint64 m_totalCount = 0;

    // -- overwrite mode
    /*!
        ON by default.

        The counterpart default - CanBusManager::m_recording - was false for a
        documented reason: a recorder already recording when you open it has
        decided for the user, and the ring would hold whatever happened during
        startup. Overwrite mode is what makes always-listening safe, because
        the trace stops being an accumulating record and becomes a live view of
        what is on the bus right now. One row per identity cannot fill up with
        startup noise; it can only show what is currently arriving.

        The second line of defence against the phase-13 pollution bug -
        m_traceHasSimulatedFrames, cleared on the Connected transition - is
        untouched and still covers the simulator case.
    */
    bool m_overwriteMode = true;
    /*! frameKey() -> frames seen. Survives a mode toggle; cleared by clear(). */
    QHash<quint64, qint64> m_countByKey;
    qint64 m_maxCount = 0;

    // -- arrival rate, per identity
    //
    // COUNT-BASED, deliberately: frames per fixed wall-clock window, smoothed.
    // The timestamp-derived version this replaced is described at length above
    // noteArrival() in the .cpp - short version, every estimate built on the
    // frame timestamps inherits whatever is wrong with them, and one frame with
    // an out-of-range timestamp used to zero the entire column permanently.
    /*! frameKey() -> smoothed Hz. This IS the displayed value. */
    QHash<quint64, double> m_rateByKey;
    /*! Frames counted in the window currently open. Swapped out each tick. */
    QHash<quint64, int> m_windowCountByKey;
    /*! Consecutive empty windows, so a stopped id snaps to 0 instead of
        decaying towards it forever. */
    QHash<quint64, int> m_emptyWindowsByKey;
    /*! Measures the window actually elapsed, never the nominal interval. */
    QElapsedTimer m_rateWindow;
    /*! Closes each window. */
    QTimer *m_rateTimer = nullptr;
    /*! frameKey() -> row. Only populated while \l overwriteMode is on. */
    QHash<quint64, int> m_rowByKey;

    // -- disk streaming. The writer lives on m_writerThread; everything here
    //    is touched on the GUI thread only.
    QThread *m_writerThread = nullptr;
    TraceStreamWriter *m_writer = nullptr;
    bool m_streaming = false;
    QString m_streamPath;
    qint64 m_streamFrames = 0;
    qint64 m_streamBytes = 0;

    // -- triggering. GUI thread; the writer owns the copies it evaluates.
    int m_triggerState = 0;                  //!< TraceStreamWriter::Idle
    double m_preTriggerSeconds = 2.0;
    TriggerCondition m_startTrigger;
    TriggerCondition m_stopTrigger;
    LogFilter m_logFilter;
    qint64 m_filteredFrames = 0;
    const CanDecoder *m_decoder = nullptr;
    /*! Read on the DEVICE thread by ingestForDisk(): while armed the writer
        still needs every frame, even though none of them reach the file yet. */
    std::atomic_bool m_feedingWriter { false };

    /*! Shared tail of startBackgroundLog() and armBackgroundLog(). */
    bool openWriter(const QUrl &fileUrl, int format, bool armed);
    void ensureWriterThread();
};
