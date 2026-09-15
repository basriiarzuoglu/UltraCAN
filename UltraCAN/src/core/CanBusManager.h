#pragma once

#include <QCanBusFrame>
#include <QElapsedTimer>
#include <QJSEngine>
#include <QObject>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>   // QML_ELEMENT / QML_SINGLETON

#include <atomic>

#include "core/CanFrame.h"
#include "dbc/CanDecoder.h"
#include "models/CanLogModel.h"

class CanDevice;
class CanAnalyzerModel;
class DbcFlatModel;
class IsoTpLayer;
class DbcModel;
class GraphDataModel;
class QThread;
class QTimer;

/*!
    \brief Owner of the single bus clock, of the hardware device and of the
           backend's shared models.

    \b {One clock}. Every timestamp in the application comes from here:
    logger, sender and player all read \l timestampNs(), a monotonic
    QElapsedTimer started once. QElapsedTimer::nsecsElapsed() only reads a
    monotonic counter, so the driver thread may call it freely.

    \b {One ingress}. \l ingest() / \l ingestBatch() are the only ways a frame
    enters the application, whether it came from the hardware, the built-in
    simulator or the QML sender.

    \b {Threading}. The QCanBusDevice lives on its own thread inside CanDevice;
    see that class for the rules. \l ingestBatch() is invoked on THAT thread by
    a direct connection and must stay thread-safe: it only touches an atomic
    flag and CanLogModel::enqueue(), which does its own locking and coalescing.
*/
class CanBusManager : public QObject
{
    Q_OBJECT
    // NOT QML_ELEMENT / QML_SINGLETON: this instance is created and owned by
    // main() and published with qmlRegisterSingletonInstance(). Letting the
    // engine register the type as well would give QML a SECOND, unwired
    // instance - which is exactly what happened before.
    // QML_ANONYMOUS keeps the metatype registered (needed wherever this type is
    // returned from a Q_PROPERTY) without claiming the type NAME.
    QML_ANONYMOUS

    Q_PROPERTY(qreal busTime READ busTime NOTIFY busTimeChanged FINAL)
    Q_PROPERTY(bool recording READ isRecording WRITE setRecording NOTIFY recordingChanged FINAL)
    Q_PROPERTY(bool simulating READ isSimulating WRITE setSimulating NOTIFY simulatingChanged FINAL)
    Q_PROPERTY(CanLogModel *logModel READ logModel CONSTANT FINAL)
    Q_PROPERTY(QStringList channelNames READ channelNames CONSTANT FINAL)

    // -- hardware connection --------------------------------------------------
    /*!
        Which source the application's frames come from right now.

        The gate lives HERE and not in GraphDataModel because ingestBatch() is
        the single fan-out point for the trace, the raw analyzer AND the graph.
        Gating only the graph would leave live traffic still filling the logger
        and the analyzer while a log plays - three consumers, one switch.
    */
    Q_PROPERTY(int frameSource READ frameSource WRITE setFrameSource
               NOTIFY frameSourceChanged FINAL)
    Q_PROPERTY(bool playbackSource READ isPlaybackSource NOTIFY frameSourceChanged FINAL)

    Q_PROPERTY(bool connected READ isConnected NOTIFY connectionChanged FINAL)
    Q_PROPERTY(int connectionState READ connectionState NOTIFY connectionChanged FINAL)
    Q_PROPERTY(QString busStatusText READ busStatusText NOTIFY connectionChanged FINAL)
    Q_PROPERTY(QString pluginName READ pluginName NOTIFY connectionChanged FINAL)
    Q_PROPERTY(QString interfaceName READ interfaceName NOTIFY connectionChanged FINAL)
    Q_PROPERTY(int bitrate READ bitrate NOTIFY connectionChanged FINAL)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged FINAL)
    Q_PROPERTY(int errorFrameCount READ errorFrameCount NOTIFY statisticsChanged FINAL)

public:
    /*! Mirrors QCanBusDevice::CanBusDeviceState without leaking the include. */
    enum State {
        Unconnected = 0,
        Connecting  = 1,
        Connected   = 2,
        Closing     = 3
    };
    Q_ENUM(State)

    explicit CanBusManager(QObject *parent = nullptr);
    ~CanBusManager() override;

    static CanBusManager *instance();
    // No create() hook: main() publishes this instance with
    // qmlRegisterSingletonInstance(); see the note above QML_ANONYMOUS.

    // -- the clock ------------------------------------------------------------
    qint64 timestampNs() const;                 //!< thread-safe
    Q_INVOKABLE qreal timestamp() const;        //!< thread-safe, seconds
    qreal busTime() const;                      //!< notified at UI rate; bind this
    Q_INVOKABLE void resetClock();

    // -- objects --------------------------------------------------------------
    CanLogModel *logModel() const { return m_logModel; }
    QStringList channelNames() const { return m_channelNames; }

    bool isRecording() const { return m_recording.load(std::memory_order_relaxed); }
    void setRecording(bool recording);

    bool isSimulating() const { return m_simulating; }
    void setSimulating(bool simulating);

    // -- decoding -------------------------------------------------------------
    /*! Wired in main(): the decode table is rebuilt whenever this model loads. */
    void setDbcModel(DbcModel *model);
    /*! Wired in main(): where RAW FRAMES go for plotting. The Graph Plot keeps
        its own databases now, so it decodes on its own side - this manager's
        decode table would produce keys that mean nothing to it. */
    void setSampleSink(GraphDataModel *sink);
    /*! Wired in main(): raw frames for the bit/byte heatmap. Fed from the
        device thread, independently of recording and of decoding - the whole
        point of the analyzer is that it works on a bus with no database. */
    void setAnalyzerSink(CanAnalyzerModel *sink);
    /*! Wired in main(): raw frames for the DBC editor's live value column.
        Unlike the other two sinks this one is gated at the far end - it drops
        every batch unless the editor page is actually on screen - so a column
        nobody is looking at costs one null check per batch. */
    void setLiveValueSink(DbcFlatModel *sink);
    /*! Wired in main(): raw frames for the UDS tester's ISO-TP transport.
        Same device-thread contract as the sinks above - IsoTpLayer::ingest()
        drops everything that is not addressed to the ECU under test before
        it costs anything, and hops to its own thread for the rest. */
    void setDiagnosticsSink(IsoTpLayer *sink);
    /*! Read-only handle for consumers that need to resolve signal keys. */
    const CanDecoder *decoder() const { return &m_decoder; }
    /*! Rebuild the decode table from the current database, now. */
    Q_INVOKABLE void refreshDecoder();
    /*! Rebuild on the next event-loop turn; a burst of edits rebuilds once. */
    void scheduleDecoderRebuild();
    /*! How many signals the decoder currently knows - 0 means nothing decodes. */
    Q_INVOKABLE int decoderSignalCount() const { return m_decoder.signalCount(); }


    // -- hardware -------------------------------------------------------------
    bool isConnected() const { return m_connectionState.load(std::memory_order_relaxed) == Connected; }
    int connectionState() const { return m_connectionState.load(std::memory_order_relaxed); }
    QString busStatusText() const;
    QString pluginName() const { return m_pluginName; }
    QString interfaceName() const { return m_interfaceName; }
    int bitrate() const { return m_bitrate; }
    QString lastError() const { return m_lastError; }
    int errorFrameCount() const { return m_errorFrameCount; }

    /*! Plugins this Qt build actually ships ("socketcan", "peakcan", "virtualcan", ...). */
    Q_INVOKABLE QStringList availablePlugins() const;
    /*! Interfaces the given plugin currently offers. */
    Q_INVOKABLE QStringList availableInterfaces(const QString &plugin) const;
    /*! Platform default: socketcan (Linux), peakcan (Windows), else virtualcan. */
    Q_INVOKABLE QString preferredPlugin() const;
    /*! Bitrates offered by the connection UI. */
    Q_INVOKABLE QVariantList standardBitrates() const;
    /*!
        Explicit "rescan the hardware" entry point for the connection panel's
        Refresh button. \c availableInterfaces() already re-queries
        \c QCanBus on every call - Qt's plugin loader caches nothing - so
        this method's real job is naming that intent and giving QML a signal
        to react to, rather than re-deriving the same list twice. A USB
        analyzer plugged in after the app started shows up the next time this
        (or availableInterfaces()) is called, with no restart required.
    */
    Q_INVOKABLE void refreshInterfaces();

    enum FrameSource {
        Live = 0,      //!< hardware (or the simulator)
        Playback       //!< a recorded log, replayed as a virtual bus
    };
    Q_ENUM(FrameSource)

    int frameSource() const { return m_frameSource.load(std::memory_order_relaxed); }
    bool isPlaybackSource() const { return frameSource() == Playback; }
    void setFrameSource(int source);

    /*!
        \b {Virtual bus.} Frames replayed from a log, injected at exactly the
        point the driver injects real ones - so the trace, the analyzer and the
        graph all see them without knowing the difference. Dropped unless the
        source is Playback.
    */
    void ingestPlaybackBatch(const QVector<CanFrame> &frames);

    /*! Opens the bus. Stops the simulator first - one source of truth at a time. */
    Q_INVOKABLE void connectBus(const QString &plugin, const QString &interfaceName, int bitrate);
    Q_INVOKABLE void disconnectBus();

public slots:
    /*! Single frame ingress. Callable from ANY thread. Stamps when timestampNs == 0. */
    void ingest(CanFrame frame);
    /*!
        Batch ingress, invoked ON THE DEVICE THREAD by a direct connection.
        Keep it free of anything that is not thread-safe.
    */
    void ingestBatch(const QVector<CanFrame> &frames);

    /*!
        Writes a ready-made frame to the active device. Thread-safe: the call is
        marshalled onto the device thread, so a transmit timer never blocks on
        the driver. Returns false when no device is connected.
    */
    bool writeFrame(const QCanBusFrame &frame);

    /*!
        The transmit path used by CanSenderModel: stamps \a frame with the bus
        clock, writes it to the hardware when connected, and ALWAYS echoes it
        into the trace as Tx so the Logger shows what was asked for even on a
        bus that does not loop transmissions back. Returns whether the hardware
        write was issued.
    */
    bool sendFrame(CanFrame frame);

    /*!
        Transmit path for the QML sender: same keys the QML models use
        (canId, extended, channel, dlc, payload "AA BB ...", name). Goes to the
        hardware when connected, and is echoed into the trace as Tx either way.
    */
    Q_INVOKABLE void transmit(const QVariantMap &frame);

signals:
    void busTimeChanged();
    void recordingChanged();
    void simulatingChanged();
    void frameSourceChanged();
    void connectionChanged();
    void lastErrorChanged();
    void statisticsChanged();
    /*! Emitted for every single-frame ingest, on the ingesting thread. */
    void frameIngested(const CanFrame &frame);

    /*!
        The decode table has been replaced - every key handed out by
        \c CanDecoder::signalKey() before this point is now meaningless.

        Anything that CACHES those keys has to re-resolve here. The rebuild is
        coalesced onto a zero-timer, so it lands one event-loop turn after the
        DbcModel signal that triggered it, and any consumer that also rebuilds
        on a zero-timer will have run FIRST - against the old table. This
        signal is what closes that window; it is not decorative. */
    void decoderRebuilt();

    /*! Emitted by refreshInterfaces() so the connection panel's Interface
        (and Plugin) combo boxes know to repopulate themselves. */
    void interfacesRefreshed();

private:
    void startSimulation();
    void stopSimulation();
    /*! The one place frames reach the consumers. Both ingest paths funnel here
        after their own gate, so a new consumer is added once. */
    void fanOut(const QVector<CanFrame> &frames);
    void setConnectionState(int state);
    void setLastError(const QString &rawMessage);

    static CanBusManager *s_instance;

    QElapsedTimer m_clock;
    CanLogModel *m_logModel = nullptr;
    IsoTpLayer *m_diagnosticsSink = nullptr;
    QTimer *m_uiClockTimer = nullptr;
    QTimer *m_decoderRebuildTimer = nullptr;

    QStringList m_channelNames { QStringLiteral("CAN1"), QStringLiteral("CAN2") };

    // Read from producer threads on every frame.
    /*!
        LISTENING at launch - reversed, and only because the trace changed
        shape underneath it.

        This used to be false, with a good argument: a recorder already
        recording when you open it has decided for the user, and by the time
        they look at the Logger the ring holds whatever happened during
        startup. That argument was about an ACCUMULATING record. It stopped
        applying when CanLogModel::overwriteMode became the default: the trace
        is now one row per (id, extended, channel), updated in place, so it
        cannot fill with startup noise - it can only show what is arriving now,
        which is the thing a live view is for. Making the user press Start to
        see whether a bus is alive was the wrong ceremony for that.

        What the reversal does NOT change:
        \list
        \li The background log is independent of this flag entirely - see
            fanOut(). A file that only recorded while the on-screen list did
            would not be a background log.
        \li m_traceHasSimulatedFrames still guards the phase-13 pollution bug:
            fabricated frames are discarded from the ring on the Connected
            transition, and the SIMULATED badge is still the visible tell. The
            simulator is off by default, so the common path never produces one.
        \endlist

        All three enqueue paths still check this flag - live batches, the
        single-frame ingest the Tx echo uses, and the simulator burst - so
        turning it off still means the ring genuinely receives nothing.
    */
    std::atomic_bool m_recording { true };
    bool m_simulating = false;

    // -- hardware -------------------------------------------------------------
    QThread *m_deviceThread = nullptr;
    CanDevice *m_device = nullptr;

    // -- decoding -------------------------------------------------------------
    CanDecoder m_decoder;
    DbcModel *m_dbcModel = nullptr;
    GraphDataModel *m_sampleSink = nullptr;
    CanAnalyzerModel *m_analyzerSink = nullptr;
    DbcFlatModel *m_liveValueSink = nullptr;
    //! Read on the device thread on every batch.
    std::atomic_int m_frameSource { Live };
    // Read from the transmit path on other threads.
    std::atomic_int m_connectionState { Unconnected };
    QString m_pluginName;
    QString m_interfaceName;
    int m_bitrate = 500000;
    QString m_lastError;
    int m_errorFrameCount = 0;

    // Built-in bus simulator: a QTimer living on its own thread, so the whole
    // producer path (worker thread -> staging queue -> GUI flush) is exercised
    // exactly as the driver exercises it.
    QThread *m_simulationThread = nullptr;
    QTimer *m_simulationTimer = nullptr;
    quint64 m_simulationTick = 0;
    /*! Set by the simulator the moment it puts a frame into the trace, cleared
        whenever the trace is emptied. It is what lets the Connected transition
        throw away fabricated traffic WITHOUT ever throwing away real traffic -
        see the note in setConnectionState(). Atomic: written on the simulation
        thread, read on the GUI thread. */
    std::atomic_bool m_traceHasSimulatedFrames { false };
};
