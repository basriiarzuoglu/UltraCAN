// Qt Charts draws through QGraphicsScene, and its text items reach into
// QApplication::style(). QGuiApplication provides no style, so the first
// ChartView instantiation dereferences a null QStyle and the process dies in
// QWidgetTextControl::setCursorWidth with SIGSEGV.
// => the whole app must run on QApplication, not QGuiApplication.
#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QQuickStyle>

#include "core/CanBusManager.h"
#include "core/ConfigManager.h"
#include "core/LogExporter.h"
#include "core/SettingsManager.h"
#include "models/CanAnalyzerModel.h"
#include "models/CanLogSortProxyModel.h"
#include "models/CanPlaybackModel.h"
#include "models/CanSenderModel.h"
#include "models/DbcFlatModel.h"
#include "models/DbcModel.h"
#include "models/CanLogModel.h"
#include "models/GraphDataModel.h"
#include "models/LogReplayModel.h"
#include "models/MathChannelManager.h"
#include "uds/UdsClient.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("Basri Arzuoglu"));
    QCoreApplication::setApplicationName(QStringLiteral("UltraCAN"));
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION));

#if defined(Q_OS_LINUX)
    /*!
        Ties the running process to ultracan.desktop.

        This is the Linux counterpart of the .rc icon resource below: the
        QIcon set by setWindowIcon() covers windows the process draws itself,
        but the icon a Wayland compositor puts in the dock, in the Alt+Tab
        switcher and in GNOME's window list comes from the desktop ENTRY it
        matches the surface to - and the match is made on the app_id, which is
        exactly this string. Without it the window arrives as a generic
        surface and shows the fallback icon no matter how good the installed
        artwork is.

        The argument is the entry's base name with no ".desktop" suffix, and
        it must equal the installed file name (see the install() rules in
        CMakeLists.txt). X11 sessions match on WM_CLASS instead, which Qt
        derives from argv[0] - hence StartupWMClass=UltraCAN in the entry.
    */
    QGuiApplication::setDesktopFileName(QStringLiteral("ultracan"));
#endif

    /*!
        The RUNTIME icon: Alt+Tab, the taskbar button of a running instance, and
        any native window this process opens.

        This is only half the job. The icon Explorer draws on the .exe itself,
        on a desktop shortcut and on a pinned taskbar button comes from the
        icon RESOURCE compiled into the binary (app_icon.rc) - a Qt application
        with no .rc shows the default Qt logo there no matter what this call
        says. Both are wired up; see app_icon.rc.

        Built from several PNGs rather than one, because QIcon picks the closest
        available size and scales: handing it only the 40 px original leaves
        Windows resampling 40 -> 16 for the Alt+Tab strip, which turns the
        star's points to mush. Each entry below is a real render at that size.
        128 and 256 are there for the large-icon views in Explorer and for
        high-DPI Alt+Tab, which otherwise upscale the 64.

        The resource prefix is the QML module's, not a hand-rolled ":/icons":
        files listed under RESOURCES in qt_add_qml_module land at
        qrc:/qt/qml/<URI>/<relative path>. A wrong prefix is not an error - it
        yields a silently null QIcon.
    */
    {
        QIcon appIcon;
        const QString iconRoot = QStringLiteral(":/qt/qml/UltraCAN/assets/icons/");
        for (int size : { 16, 24, 32, 48, 64, 128, 256 })
            appIcon.addFile(iconRoot + QStringLiteral("ultracan_logo_%1.png").arg(size),
                            QSize(size, size));
        if (appIcon.isNull()) {
            // Loud, because the failure is otherwise invisible: the app just
            // keeps the default icon and nobody knows the path was wrong.
            qWarning("Application icon did not load from %s - check the RESOURCES "
                     "block in the root CMakeLists", qPrintable(iconRoot));
        } else {
            QApplication::setWindowIcon(appIcon);
        }
    }

    // "Basic" is the only Qt Quick Controls style that is fully customizable;
    // the dark look comes from our Theme singleton, not from a platform style.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // ------------------------------------------------------------------ backend
    // Created here, BEFORE the engine, and destroyed after it. Declaration
    // order is destruction order in reverse: busManager is declared last so it
    // is torn down FIRST, stopping the device and simulator threads before the
    // objects they feed disappear.
    // Settings first: it owns no other object and everything below may want to
    // ask it what the last session looked like.
    SettingsManager settings;

    DbcModel dbcModel;
    // After dbcModel and before busManager on purpose: this borrows a pointer
    // to each, and reverse declaration order is what guarantees it is torn down
    // while both are still alive.
    DbcFlatModel dbcRows;
    // Before graphData so it is destroyed AFTER it: every math track holds the
    // manager's channel id and evaluateMathTracks() dereferences the manager on
    // the refresh timer, which only stops when graphData goes.
    MathChannelManager mathChannels;
    GraphDataModel graphData;
    CanAnalyzerModel analyzer;
    CanSenderModel senderModel;
    CanBusManager busManager;

    // Declared AFTER busManager on purpose. The playback model borrows
    // busManager's decoder (a member, so the pointer never moves), and reverse
    // declaration order means playbackModel is destroyed FIRST - it can never
    // outlive the decoder it points at.
    CanPlaybackModel playbackModel;
    // The PHYSICAL replay engine, and a different object from playbackModel on
    // purpose: that one feeds the application and has no path to the wire,
    // this one feeds the wire and has no application-side dispatch. See the
    // class note - the two were briefly one object with a flag, and a flag on
    // a control an analyst presses all day is how a bus gets energised by
    // accident.
    LogReplayModel logReplay;
    // The UDS tester. Owns its own ISO-TP transport; declared after busManager
    // because it borrows it, and therefore torn down before it.
    UdsClient udsClient;
    LogExporter logExporter;
    // Borrows busManager's log model, so it is declared after busManager and
    // therefore destroyed before it - same rule as everything else here.
    CanLogSortProxyModel traceSortProxy;
    // Borrows the three models above; declared after them so it is destroyed
    // first, like every other borrower in this block.
    ConfigManager configManager;

    // Wiring, in one place instead of scattered lazy lookups:
    //   DBC load / edit -> rebuild the decode table
    //   decoded samples -> the graph
    //   graph tracking  -> switches decoding on and off
    busManager.setDbcModel(&dbcModel);
    busManager.setSampleSink(&graphData);
    // The logger's triggers evaluate decoded signals, so they need the same
    // decode table the bus is using - and they need to hear when it is
    // replaced, because a signal key does not survive a DBC reload.
    if (busManager.logModel()) {
        busManager.logModel()->setDecoder(busManager.decoder());
        QObject::connect(&busManager, &CanBusManager::decoderRebuilt,
                         busManager.logModel(), &CanLogModel::refreshTriggerKeys);
    }
    graphData.setMathChannelManager(&mathChannels);
    senderModel.setBusManager(&busManager);
    logReplay.setBusManager(&busManager);
    logReplay.setChannelNames(busManager.channelNames());
    senderModel.setDbcModel(&dbcModel);

    // Playback is offline by design: it never touches the bus. It only needs
    // the decode table (to label recorded ids with their DBC message names) and
    // the channel list (to render "CAN1"/"CAN2" instead of raw indices). The
    // decoder is rebuilt in place whenever a .dbc is loaded, so a log opened
    // before the database still gets its names on the next repaint.
    // The analyzer is fed from the device thread inside ingestBatch(), i.e.
    // before decoding and regardless of whether the trace is recording. It is
    // the one consumer that has to work on a bus nobody has a database for.
    busManager.setAnalyzerSink(&analyzer);

    // Diagnostics: the client writes requests through busManager.sendFrame(),
    // and its transport needs the return path. Registering the TRANSPORT as
    // the sink rather than the client keeps the frame-level filtering where
    // the frame-level state machine is.
    udsClient.setBus(&busManager);
    busManager.setDiagnosticsSink(udsClient.transport());

    // The DBC editor's grid: a flattened view of dbcModel, plus a live value
    // column. The sink is always connected but the model itself ignores every
    // batch until QML sets liveValuesEnabled, so nothing is decoded for it
    // unless that page is actually open.
    dbcRows.setSource(&dbcModel);
    dbcRows.setDecoder(busManager.decoder());
    busManager.setLiveValueSink(&dbcRows);
    // The decoder pointer never moves, but the TABLE behind it is replaced on
    // every load, and dbcRows caches keys into that table. Both objects react
    // to a load on a zero-timer and dbcRows' timer always starts first - it
    // therefore always resolves against the outgoing table. This connection is
    // what re-resolves afterwards; without it the DBC Editor's live value and
    // colour columns are empty after loading into an empty document, whatever
    // triggered the load. See DbcFlatModel::decoderRebuilt().
    QObject::connect(&busManager, &CanBusManager::decoderRebuilt,
                     &dbcRows, &DbcFlatModel::decoderRebuilt);
    analyzer.setChannelNames(busManager.channelNames());
    // ONE direction only. The analyzer resolves names against its own private
    // list of .dbc files (loadAnalyzerDbc / removeAnalyzerDbc), so what the
    // Editor has open never changes what the analyzer shows - you can throw
    // three candidate databases at an unknown bus without disturbing the file
    // you are authoring. What remains is the outward path: a bit span carved
    // out in the analyzer is published INTO the editor's document, because
    // that is the only thing in the application that can save a .dbc.
    analyzer.setChannelTarget(&dbcModel);

    playbackModel.setDecoder(busManager.decoder());
    playbackModel.setChannelNames(busManager.channelNames());
    // The virtual bus: replayed frames are injected at the same point the
    // driver injects real ones, so the trace, the analyzer and the graph all
    // see them without knowing the difference.
    playbackModel.setBusManager(&busManager);

    // ------------------------------------------------- live / playback handover
    //
    // Orchestration lives here rather than inside the models, so CanPlaybackModel
    // never learns that a graph or an analyzer exists - the same reason the
    // settings wiring is in main().
    QObject::connect(&playbackModel, &CanPlaybackModel::playbackActiveChanged,
                     &busManager, [&] {
        const bool playing = playbackModel.isPlaybackActive();
        busManager.setFrameSource(playing ? CanBusManager::Playback : CanBusManager::Live);

        // The two sources are unrelated timelines - a log starting at t=0 and a
        // bus that has been up for an hour. Anything still holding a rolling
        // window has to be told to throw it away, or it prunes every incoming
        // sample as "older than the window" and shows nothing at all.
        graphData.rebaseTo(playing ? playbackModel.position() : busManager.timestamp());
        analyzer.reset();
    });

    // A scrub is a jump, not playback: the points on screen belong to where the
    // playhead was, so they go.
    QObject::connect(&playbackModel, &CanPlaybackModel::seeked,
                     &graphData, &GraphDataModel::rebaseTo);
    logExporter.setSource(busManager.logModel());

    // The Logger's sortable view of the trace. The model keeps knowing nothing
    // about how it is displayed; the proxy is a pass-through until overwrite
    // mode is on.
    traceSortProxy.setLogModel(busManager.logModel());

    // Session files. ConfigManager only ever reads and writes these three -
    // the graph WORKSPACE (tabs, panes, scales) is not represented in C++ at
    // all and is handed over by QML during the export/import call itself.
    configManager.setDbcModel(&dbcModel);
    configManager.setGraphModel(&graphData);
    configManager.setAnalyzerModel(&analyzer);
    configManager.setMathChannels(&mathChannels);
    configManager.setLogModel(busManager.logModel());

    // ------------------------------------------------------------- persistence
    //
    // Both directions of "remember this" are wired here rather than inside the
    // objects, which keeps SettingsManager dependency-free and keeps the
    // backend classes from knowing that a settings file exists at all.
    //
    // The rule in both cases: persist what SUCCEEDED, never what was merely
    // typed. A plugin that failed to open must not become the default forever,
    // and a .dbc that would not parse must not be reopened on every launch.
    QObject::connect(&busManager, &CanBusManager::connectionChanged, &settings, [&] {
        if (busManager.isConnected()) {
            settings.rememberConnection(busManager.pluginName(),
                                        busManager.interfaceName(),
                                        busManager.bitrate());
        }
    });
    QObject::connect(&dbcModel, &DbcModel::sourceUrlChanged, &settings, [&] {
        settings.setLastDbcUrl(dbcModel.sourceUrl());
    });
    // The simulator follows the same rule: whatever state it is actually in is
    // the state that gets remembered. connectBus() switches it off, so opening
    // real hardware once is enough to stop it coming back on the next launch.
    QObject::connect(&busManager, &CanBusManager::simulatingChanged, &settings, [&] {
        settings.setSimulatorEnabled(busManager.isSimulating());
    });
    // QSettings only guarantees a flush on destruction, and a DAQ tool is the
    // kind of program that gets killed rather than closed.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &settings, [&] { settings.flush(); });

    // -------------------------------------------------------- QML registration
    //
    // These MUST be registered by instance, not by QML_SINGLETON + create().
    //
    // With QML_SINGLETON the engine decides how to build the singleton, and if
    // it does not pick up the static create() hook - which it silently did not
    // here - it default-constructs its own object. The result is two of
    // everything: QML edits one DbcModel while the decoder reads another, and
    // the graph asks an unwired GraphDataModel for a decoder that only the C++
    // copy ever had. qmlRegisterSingletonInstance() removes the choice: QML is
    // handed exactly this pointer.
    //
    // Consequences to keep in mind:
    //   - the three classes must NOT carry QML_ELEMENT / QML_SINGLETON any more
    //     (they use QML_ANONYMOUS), or the generated module registration would
    //     claim the same names and conflict;
    //   - the objects must outlive the engine, which the declaration order
    //     above guarantees;
    //   - registration is global, so it only has to happen before the engine
    //     loads any QML.
    qmlRegisterSingletonInstance("UltraCAN.Core", 1, 0, "CanBusManager", &busManager);
    qmlRegisterSingletonInstance("UltraCAN.Core", 1, 0, "DbcModel", &dbcModel);
    qmlRegisterSingletonInstance("UltraCAN.Core", 1, 0, "DbcFlatModel", &dbcRows);
    qmlRegisterSingletonInstance("UltraCAN.Core", 1, 0, "GraphDataModel", &graphData);
    qmlRegisterSingletonInstance("UltraCAN.Core", 1, 0, "MathChannelManager", &mathChannels);
    qmlRegisterSingletonInstance("UltraCAN.Core", 1, 0, "LogReplayModel", &logReplay);
    qmlRegisterSingletonInstance("UltraCAN.Core", 1, 0, "CanSenderModel", &senderModel);
    qmlRegisterSingletonInstance("UltraCAN.Core", 1, 0, "CanPlaybackModel", &playbackModel);
    qmlRegisterSingletonInstance("UltraCAN.Core", 1, 0, "CanAnalyzerModel", &analyzer);
    qmlRegisterSingletonInstance("UltraCAN.Core", 1, 0, "SettingsManager", &settings);
    qmlRegisterSingletonInstance("UltraCAN.Core", 1, 0, "LogExporter", &logExporter);
    qmlRegisterSingletonInstance("UltraCAN.Core", 1, 0, "ConfigManager", &configManager);
    qmlRegisterSingletonInstance("UltraCAN.Core", 1, 0, "TraceSortModel", &traceSortProxy);
    qmlRegisterSingletonInstance("UltraCAN.Core", 1, 0, "UdsClient", &udsClient);

    // ---------------------------------------------------------------- restore
    //
    // Done before the engine loads, so the first frame the user sees is already
    // the restored state - no visible flicker from empty to populated.
    //
    // Note what is deliberately NOT restored: the bus connection. Opening
    // hardware is a side effect on the outside world, and an application that
    // grabs a PCAN channel the moment it launches is an application that fights
    // whatever else the technician had open. The remembered plugin/interface/
    // bitrate are only pre-filled into the panel; the Connect button stays the
    // user's decision.
    busManager.logModel()->setCapacity(settings.logCapacity());

    // Deliberately gated, and the gate is CLOSED by default - see
    // SettingsManager::autoLoadDbc. The editor comes up empty; a document
    // appears only because the user opened one.
    //
    // This is the only automatic load in the process. Both explicit paths are
    // untouched and neither consults hasRestorableDbc(): the Load DBC button
    // calls DbcModel.loadDbc() straight from DbcEditorView, and importing a
    // .ucan goes through ConfigManager::applyDocument().
    if (settings.hasRestorableDbc()) {
        const QUrl dbcUrl = settings.lastDbcUrl();
        if (dbcModel.loadDbc(dbcUrl)) {
            qInfo("Restored database %s (%d messages)",
                  qPrintable(dbcUrl.fileName()), dbcModel.messageCount());
        } else {
            // A database that no longer parses is a notice, not a failure to
            // start: clear it so the next launch is clean.
            qWarning("Could not restore %s: %s",
                     qPrintable(dbcUrl.fileName()), qPrintable(dbcModel.lastError()));
            settings.setLastDbcUrl(QUrl());
        }
    }

    // The built-in traffic generator is a DEVELOPMENT AID and is now off unless
    // the user asked for it. It used to start unconditionally here, which meant
    // every launch filled the ring with fabricated 0x100/0x200/0x300 frames
    // before anyone touched the hardware; connectBus() stops the generator but
    // cannot un-record what it already wrote, so a live capture came up with a
    // fake prologue. Two changes close that off: this line, and the
    // discard-at-connect guard in CanBusManager::setConnectionState().
    //
    // When it IS on, it produces frames on its own thread, exercising exactly
    // the producer path the driver uses. Its ids match the demo database, not a
    // customer .dbc.
    busManager.setSimulating(settings.simulatorEnabled());

    // One line that proves the backend is whole. If the pointers here ever
    // differ from the ones the QML side reports, the registration above broke.
    qInfo("UltraCAN backend: bus=%p dbc=%p graph=%p sender=%p play=%p decoder=%p (%d signals)",
          static_cast<void *>(&busManager), static_cast<void *>(&dbcModel),
          static_cast<void *>(&graphData), static_cast<void *>(&senderModel),
          static_cast<void *>(&playbackModel),
          static_cast<const void *>(busManager.decoder()), busManager.decoderSignalCount());
    qInfo("UltraCAN settings: %s", qPrintable(settings.configFilePath()));

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("UltraCAN", "Main");

    return app.exec();
}
