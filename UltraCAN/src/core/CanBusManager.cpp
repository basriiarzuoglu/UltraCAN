#include "CanBusManager.h"

#include "uds/IsoTpLayer.h"

#include <QRandomGenerator>
#include <QThread>
#include <QTimer>
#include <QVariantList>

#include "can/CanDevice.h"
#include "models/CanAnalyzerModel.h"
#include "models/DbcFlatModel.h"
#include "models/DbcModel.h"
#include "models/GraphDataModel.h"

CanBusManager *CanBusManager::s_instance = nullptr;

namespace {

/*! "AA BB 0C" -> bytes. Tolerates missing separators and odd whitespace. */
int parseHexPayload(const QString &text, std::array<quint8, 64> &out)
{
    const QString compact = QString(text).remove(QLatin1Char(' '))
                                         .remove(QLatin1Char(':'))
                                         .remove(QLatin1Char('-'));
    int written = 0;
    for (int i = 0; i + 1 < compact.size() && written < int(out.size()); i += 2) {
        bool ok = false;
        const quint8 byte = quint8(compact.mid(i, 2).toUInt(&ok, 16));
        if (!ok)
            break;
        out[size_t(written++)] = byte;
    }
    return written;
}

} // namespace

CanBusManager::CanBusManager(QObject *parent)
    : QObject(parent)
    , m_logModel(new CanLogModel(this))
{
    if (!s_instance) {
        s_instance = this;
    } else {
        qCritical("CanBusManager: a SECOND instance was created (%p; first was %p).",
                  static_cast<void *>(this), static_cast<void *>(s_instance));
    }

    // Both may travel through queued connections once other consumers appear.
    qRegisterMetaType<CanFrame>("CanFrame");
    qRegisterMetaType<QVector<CanFrame>>("QVector<CanFrame>");

    m_clock.start();
    m_logModel->setChannelNames(m_channelNames);

    // busTime is for display only. Notifying it per frame would repaint every
    // binding in the UI thousands of times a second.
    m_uiClockTimer = new QTimer(this);
    m_uiClockTimer->setInterval(50);
    connect(m_uiClockTimer, &QTimer::timeout, this, &CanBusManager::busTimeChanged);
    m_uiClockTimer->start();

    // Rebuilding the decode table walks the whole database, so a burst of
    // edits (or a load, which also emits per-row changes) must collapse into
    // one rebuild on the next event-loop turn.
    m_decoderRebuildTimer = new QTimer(this);
    m_decoderRebuildTimer->setInterval(0);
    m_decoderRebuildTimer->setSingleShot(true);
    connect(m_decoderRebuildTimer, &QTimer::timeout, this, &CanBusManager::refreshDecoder);

    // The device lives on its own thread for the whole life of the manager;
    // opening and closing the bus is just a queued call onto that thread.
    m_deviceThread = new QThread(this);
    m_deviceThread->setObjectName(QStringLiteral("can-device"));

    m_device = new CanDevice([this] { return timestampNs(); });   // no parent
    m_device->moveToThread(m_deviceThread);

    // DIRECT connection on purpose: this runs on the device thread and hands
    // the batch straight to the model's thread-safe staging queue. Routing it
    // through the GUI event loop would copy the frames an extra time.
    connect(m_device, &CanDevice::framesReady,
            this, &CanBusManager::ingestBatch, Qt::DirectConnection);

    // These are queued (different threads), so they land on the GUI thread.
    connect(m_device, &CanDevice::stateChanged,
            this, &CanBusManager::setConnectionState);
    connect(m_device, &CanDevice::errorText,
            this, &CanBusManager::setLastError);
    connect(m_device, &CanDevice::errorFramesSeen, this, [this](int count) {
        m_errorFrameCount += count;
        emit statisticsChanged();
    });

    m_deviceThread->start();
}

CanBusManager::~CanBusManager()
{
    stopSimulation();

    if (m_deviceThread) {
        // Close on the device thread and wait, so the QCanBusDevice is torn
        // down by the thread that created it.
        if (m_deviceThread->isRunning()) {
            QMetaObject::invokeMethod(m_device, [device = m_device] { device->close(); },
                                      Qt::BlockingQueuedConnection);
        }
        m_deviceThread->quit();
        m_deviceThread->wait();
        delete m_device;
        m_device = nullptr;
    }

    if (s_instance == this)
        s_instance = nullptr;
}

CanBusManager *CanBusManager::instance()
{
    return s_instance;
}


// ---------------------------------------------------------------- decoding

void CanBusManager::setDbcModel(DbcModel *model)
{
    if (m_dbcModel == model)
        return;

    if (m_dbcModel)
        disconnect(m_dbcModel, nullptr, this, nullptr);

    m_dbcModel = model;

    if (m_dbcModel) {
        // BOTH signals matter: loaded() covers opening a file, databaseChanged()
        // covers everything else - adding a signal, editing a start bit, a
        // factor, an endianness. Listening only to loaded() leaves the decoder
        // describing a database that no longer exists.
        connect(m_dbcModel, &DbcModel::loaded, this, &CanBusManager::scheduleDecoderRebuild);
        connect(m_dbcModel, &DbcModel::databaseChanged, this, &CanBusManager::scheduleDecoderRebuild);
        refreshDecoder();
    } else {
        m_decoder.clear();
    }
}

void CanBusManager::scheduleDecoderRebuild()
{
    if (m_decoderRebuildTimer && !m_decoderRebuildTimer->isActive())
        m_decoderRebuildTimer->start();
}

void CanBusManager::setAnalyzerSink(CanAnalyzerModel *sink)
{
    m_analyzerSink = sink;
}

void CanBusManager::setDiagnosticsSink(IsoTpLayer *sink)
{
    m_diagnosticsSink = sink;
}

void CanBusManager::setLiveValueSink(DbcFlatModel *sink)
{
    m_liveValueSink = sink;
}

void CanBusManager::setSampleSink(GraphDataModel *sink)
{
    if (m_sampleSink == sink)
        return;

    if (m_sampleSink)
        disconnect(m_sampleSink, nullptr, this, nullptr);

    m_sampleSink = sink;

    // Nothing further to wire. The graph decodes with its OWN databases now, so
    // it wants raw frames rather than this manager's decode table, and it gates
    // itself: ingestFrames() returns on an atomic read while nothing is
    // plotted, which is the saving the old m_decodingEnabled flag used to buy.
}

void CanBusManager::refreshDecoder()
{
    if (!m_dbcModel) {
        m_decoder.clear();
        // Announced on this path too: a consumer holding keys into a table
        // that no longer exists is worse than one holding none.
        emit decoderRebuilt();
        return;
    }

    m_decoder.setDatabase(m_dbcModel->database());
    qInfo("CAN decoder: %d signals in %d messages from %s",
          m_decoder.signalCount(), m_dbcModel->count(),
          qPrintable(m_dbcModel->databaseName()));
    emit decoderRebuilt();
}

// ------------------------------------------------------------------- clock

qint64 CanBusManager::timestampNs() const
{
    return m_clock.nsecsElapsed();
}

qreal CanBusManager::timestamp() const
{
    return double(timestampNs()) / 1'000'000'000.0;
}

qreal CanBusManager::busTime() const
{
    return timestamp();
}

void CanBusManager::resetClock()
{
    m_clock.restart();
    m_logModel->clear();
    m_traceHasSimulatedFrames.store(false, std::memory_order_relaxed);
    m_errorFrameCount = 0;
    emit busTimeChanged();
    emit statisticsChanged();
}

// ------------------------------------------------------------------ state

void CanBusManager::setRecording(bool recording)
{
    if (m_recording.load() == recording)
        return;
    m_recording.store(recording);
    emit recordingChanged();
}

void CanBusManager::setConnectionState(int state)
{
    if (m_connectionState.exchange(state) == state)
        return;

    // A trace must have exactly ONE source. connectBus() already stops the
    // simulator, but stopping it does not un-record what it wrote: whatever
    // the generator produced between launch and Connect is still sitting in
    // the ring, and real frames then append underneath it. That is what makes
    // 0x100/0x200/0x300 appear in a capture from live hardware.
    //
    // The flag is what keeps this safe. A trace that never held a fabricated
    // frame is never touched, so reconnecting after a real capture does not
    // silently destroy it.
    if (state == Connected && m_traceHasSimulatedFrames.exchange(false)) {
        const int discarded = m_logModel->count();
        if (discarded > 0) {
            m_logModel->clear();
            qInfo("Bus connected: discarded %d simulated frame(s) so the trace "
                  "holds live traffic only.", discarded);
        }
    }

    emit connectionChanged();
}

void CanBusManager::setLastError(const QString &rawMessage)
{
    // Second line of defence: CanDevice already sanitizes, but any future
    // source of driver text lands here too.
    const QString message = CanDevice::sanitize(rawMessage);
    if (m_lastError == message)
        return;
    m_lastError = message;
    qWarning("CAN: %s", qPrintable(message));
    emit lastErrorChanged();
}

QString CanBusManager::busStatusText() const
{
    switch (m_connectionState.load(std::memory_order_relaxed)) {
    case Connecting:
        return tr("Connecting to %1...").arg(m_interfaceName);
    case Connected:
        // SocketCAN gets no rate in the label. The combo box value was never
        // sent to the driver there - the kernel owns the bitrate, set with
        // `ip link` - so printing it would state a number that is simply not
        // what the bus is running at whenever the two disagree. See the
        // socketcan branch in CanDevice::open().
        if (CanDevice::isSocketCan(m_pluginName))
            return tr("Connected (%1 / %2, kernel bitrate)")
                   .arg(m_pluginName, m_interfaceName);
        return tr("Connected (%1 / %2 @ %3 kbit/s)")
               .arg(m_pluginName, m_interfaceName)
               .arg(m_bitrate / 1000);
    case Closing:
        return tr("Closing...");
    default:
        return m_simulating ? tr("Disconnected (simulator running)")
                            : tr("Disconnected");
    }
}

// --------------------------------------------------------------- hardware

QStringList CanBusManager::availablePlugins() const
{
    return CanDevice::availablePlugins();
}

QStringList CanBusManager::availableInterfaces(const QString &plugin) const
{
    return CanDevice::availableInterfaces(plugin);
}

QString CanBusManager::preferredPlugin() const
{
    return CanDevice::preferredPlugin();
}

QVariantList CanBusManager::standardBitrates() const
{
    return { 125000, 250000, 500000, 800000, 1000000 };
}

void CanBusManager::refreshInterfaces()
{
    // Nothing to recompute here: availableInterfaces()/availablePlugins()
    // hit QCanBus fresh on every call already (see CanDevice::
    // availableInterfaces()). This method exists so "the user asked for a
    // rescan" is a named, explicit action in both C++ and QML instead of an
    // implicit side effect of opening the connection panel, and so the panel
    // has a signal to hang a Refresh button off rather than re-calling the
    // enumeration functions blind.
    emit interfacesRefreshed();
}

void CanBusManager::connectBus(const QString &plugin, const QString &interfaceName, int bitrate)
{
    if (plugin.isEmpty() || interfaceName.isEmpty()) {
        setLastError(tr("Pick a plugin and an interface first."));
        return;
    }

    // Real traffic and fabricated traffic must never share the trace.
    setSimulating(false);

    m_pluginName = plugin;
    m_interfaceName = interfaceName;
    m_bitrate = bitrate > 0 ? bitrate : m_bitrate;
    setLastError(QString());
    setConnectionState(Connecting);
    emit connectionChanged();

    const int rate = m_bitrate;
    QMetaObject::invokeMethod(m_device, [device = m_device, plugin, interfaceName, rate] {
        device->open(plugin, interfaceName, rate);
    });
}

void CanBusManager::disconnectBus()
{
    setConnectionState(Closing);
    QMetaObject::invokeMethod(m_device, [device = m_device] { device->close(); });
}

// ----------------------------------------------------------------- ingress

void CanBusManager::ingest(CanFrame frame)
{
    if (frame.timestampNs == 0)
        frame.timestampNs = timestampNs();

    emit frameIngested(frame);

    // NO enqueue() here. It used to sit on this line, and it survived the
    // change that turned the rest of this function into a delegation - so
    // every frame that came through the single-frame path was entered in the
    // trace TWICE, once here and once in fanOut(). Invisible on a live bus,
    // where almost everything arrives in batches through ingestBatch(); very
    // visible the moment a restbus replay put a thousand Tx frames through
    // here and the trace showed two thousand.
    //
    // Delegate rather than repeat the fan-out. Two copies of "who gets frames"
    // is how the Tx echo reached the trace but never the raw analyzer - this
    // path was not updated when that sink was added. One list, one place to
    // add the next consumer.
    ingestBatch(QVector<CanFrame> { frame });
}


void CanBusManager::ingestBatch(const QVector<CanFrame> &frames)
{
    // Runs on the device thread. Everything here must be thread-safe:
    // atomic reads and CanLogModel::enqueue(), which locks internally.
    if (frames.isEmpty())
        return;

    // Mutual exclusion. While a log is being replayed the hardware keeps
    // running and keeps producing frames; letting them through would interleave
    // two unrelated timelines in the same trace, the same heatmap and the same
    // plot - and the plot would be the confusing one, because the two clocks
    // are seconds or hours apart and one set of points would silently fall
    // outside the window.
    if (m_frameSource.load(std::memory_order_relaxed) != Live)
        return;

    fanOut(frames);
}

void CanBusManager::ingestPlaybackBatch(const QVector<CanFrame> &frames)
{
    if (frames.isEmpty())
        return;
    // The mirror of the gate above: a stale batch dispatched as playback stops
    // must not leak into a live capture.
    if (m_frameSource.load(std::memory_order_relaxed) != Playback)
        return;

    fanOut(frames);
}

void CanBusManager::fanOut(const QVector<CanFrame> &frames)
{
    // The background log is a recorder in its own right and sits IN FRONT of
    // the recording gate, with the same contract as the three sinks below: it
    // must keep writing while the trace is not listening, while the ring is
    // evicting past its cap, and while overwrite mode has collapsed the view
    // to one row per id. A file that stops when the on-screen list does is not
    // a background log.
    m_logModel->ingestForDisk(frames);

    if (m_recording.load(std::memory_order_relaxed))
        m_logModel->enqueue(frames);

    // Independent of BOTH recording and decoding. The raw analyzer exists for
    // buses nobody has a .dbc for, and pausing the trace must not blind it.
    // ingest() locks briefly and allocates nothing - see its threading note.
    if (m_analyzerSink)
        m_analyzerSink->ingest(frames);

    // Live values for the DBC editor grid. Same contract as the analyzer:
    // still on the device thread, still allocating nothing when it is idle.
    if (m_liveValueSink)
        m_liveValueSink->ingest(frames);

    // Same contract for the graph: raw frames, decoded on the far side against
    // ITS databases. Independent of recording too - live plots with the trace
    // paused is a normal thing to want.
    if (m_sampleSink)
        m_sampleSink->ingestFrames(frames);

    // The UDS tester. Last because it is the only sink that can answer back:
    // its Flow Control goes out through sendFrame(), and a reassembly that
    // completes here re-enters the event loop on the UI thread.
    if (m_diagnosticsSink)
        m_diagnosticsSink->ingest(frames);
}

void CanBusManager::setFrameSource(int source)
{
    const int wanted = (source == Playback) ? Playback : Live;
    if (m_frameSource.exchange(wanted) == wanted)
        return;

    // Entering playback silences the simulator too. It is a frame producer like
    // any other, and "why is my log full of 0x100" is a bad afternoon.
    if (wanted == Playback && m_simulating)
        setSimulating(false);

    qInfo("Frame source: %s", wanted == Playback ? "PLAYBACK (virtual bus)" : "LIVE");
    emit frameSourceChanged();
}

bool CanBusManager::writeFrame(const QCanBusFrame &frame)
{
    if (!m_device || !isConnected())
        return false;

    // Queued: the driver call happens on the device thread, so a 10 ms cyclic
    // timer never waits on the hardware.
    QMetaObject::invokeMethod(m_device, [device = m_device, frame] {
        device->writeRaw(frame);
    });
    return true;
}

bool CanBusManager::sendFrame(CanFrame frame)
{
    frame.timestampNs = timestampNs();
    frame.direction = CanFrame::Tx;

    bool written = false;
    if (isConnected() && m_device) {
        QMetaObject::invokeMethod(m_device, [device = m_device, frame] {
            device->send(frame);
        });
        written = true;
    }

    ingest(frame);        // echo into the trace either way
    return written;
}

void CanBusManager::transmit(const QVariantMap &frame)
{
    CanFrame out;
    out.timestampNs = timestampNs();
    out.direction = CanFrame::Tx;
    out.id = frame.value(QStringLiteral("canId")).toUInt();
    out.extended = frame.value(QStringLiteral("extended")).toBool();
    out.name = frame.value(QStringLiteral("name")).toString();

    const QString channel = frame.value(QStringLiteral("channel")).toString();
    const int channelIndex = m_channelNames.indexOf(channel);
    out.channel = quint8(channelIndex >= 0 ? channelIndex : 0);

    const int parsed = parseHexPayload(frame.value(QStringLiteral("payload")).toString(),
                                       out.payload);
    const int dlc = frame.value(QStringLiteral("dlc"), parsed).toInt();
    out.dlc = quint8(qBound(0, dlc, 64));

    sendFrame(out);       // one transmit path for QML and for CanSenderModel
}

// -------------------------------------------------------------- simulation

void CanBusManager::setSimulating(bool simulating)
{
    if (m_simulating == simulating)
        return;

    m_simulating = simulating;
    if (simulating)
        startSimulation();
    else
        stopSimulation();

    emit simulatingChanged();
    emit connectionChanged();      // the status text mentions the simulator
}

void CanBusManager::startSimulation()
{
    if (m_simulationThread)
        return;

    struct Template {
        const char *name;
        quint32 id;
        bool extended;
        quint8 channel;
        CanFrame::Direction direction;
        quint8 dlc;
        int period;          // in 10 ms ticks
    };
    static const Template templates[] = {
        { "EngineData",     0x100,      false, 0, CanFrame::Rx, 8, 1  },
        { "BatteryStatus",  0x200,      false, 0, CanFrame::Rx, 8, 5  },
        { "VehicleCommand", 0x300,      false, 1, CanFrame::Tx, 4, 2  },
        { "ChargerStatus",  0x18FF50E5, true,  1, CanFrame::Rx, 8, 10 },
        { "DiagResponse",   0x7E8,      false, 0, CanFrame::Rx, 8, 25 }
    };

    m_simulationThread = new QThread;
    m_simulationThread->setObjectName(QStringLiteral("can-sim"));

    m_simulationTimer = new QTimer;     // no parent: moved to the worker thread
    m_simulationTimer->setInterval(10);
    m_simulationTimer->setTimerType(Qt::PreciseTimer);
    m_simulationTimer->moveToThread(m_simulationThread);

    // Receiver context is the timer, so the lambda runs on the worker thread -
    // the same cross-thread producer path the driver uses.
    connect(m_simulationTimer, &QTimer::timeout, m_simulationTimer, [this] {
        ++m_simulationTick;
        QVector<CanFrame> burst;
        for (const Template &t : templates) {
            if (m_simulationTick % quint64(t.period) != 0)
                continue;

            CanFrame frame;
            frame.timestampNs = timestampNs();
            frame.id = t.id;
            frame.name = QString::fromLatin1(t.name);
            frame.channel = t.channel;
            frame.dlc = t.dlc;
            frame.direction = t.direction;
            frame.extended = t.extended;
            for (quint8 b = 0; b < t.dlc; ++b)
                frame.payload[b] = quint8(QRandomGenerator::global()->bounded(256));
            burst.append(frame);
        }
        if (!burst.isEmpty() && m_recording.load(std::memory_order_relaxed)) {
            // Remember that this trace is no longer purely real. Nothing acts
            // on it until a bus actually connects.
            m_traceHasSimulatedFrames.store(true, std::memory_order_relaxed);
            m_logModel->enqueue(burst);     // one lock, one queued flush
        }
    });

    connect(m_simulationThread, &QThread::started,
            m_simulationTimer, qOverload<>(&QTimer::start));

    m_simulationThread->start();
}

void CanBusManager::stopSimulation()
{
    if (!m_simulationThread)
        return;

    m_simulationThread->quit();
    m_simulationThread->wait();         // from here on nothing touches the timer
    delete m_simulationTimer;
    delete m_simulationThread;
    m_simulationTimer = nullptr;
    m_simulationThread = nullptr;
    m_simulationTick = 0;
}
