import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs

import UltraCAN.Theme
import UltraCAN.Core
import UltraCAN.Components

/*!
    \qmltype CanLogPlayerView
    \brief Replays a recorded trace: transport controls, a scrubber and the
           same trace table the live logger uses.

    Phase 12 - the playback engine now lives in C++ (\c CanPlaybackModel):
    the file readers (.csv / PCAN .trc / Vector .asc), the frame store
    (a contiguous std::vector, so 500k frames are one allocation), the
    wall-clock playback loop and the binary-search seek.

    What stayed in QML is only what QML owns: \l followPlayhead - a view
    concern, not a playback concern. Everything else - position, playing,
    speed, loop, currentIndex - is read straight off the singleton, so the
    UI can never drift out of sync with the engine the way two clocks would.
*/
BasePage {
    id: root

    title: qsTr("CAN Log Player")
    subtitle: CanPlaybackModel.loaded
              ? qsTr("%1 - %2 frames, %3")
                .arg(CanPlaybackModel.sourceName)
                .arg(CanPlaybackModel.count)
                .arg(CanPlaybackModel.formatTime(CanPlaybackModel.duration))
              : qsTr("No recording loaded")
    contentMargins: 0

    // ---------------------------------------------------------------- state
    /*! Suspends auto-scroll while the user is reading somewhere else. */
    property bool followPlayhead: true
    property string statusMessage: ""
    property bool statusIsError: false

    // loading
    property bool busy: false
    property string busyMessage: ""
    property string pendingName: ""
    property url pendingUrl: ""

    readonly property var traceColumns: [ 86, 54, 62, 104, 52, 40, 0 ]
    readonly property var traceTitles: [ qsTr("Time (s)"), qsTr("Dir"), qsTr("Ch"),
                                         qsTr("ID"), qsTr("Type"), qsTr("DLC"), qsTr("Data") ]

    // ------------------------------------------------------------- transport
    // Thin wrappers: the engine owns the state, these only add the view-side
    // side effect (re-arming auto-scroll, parking the list at the top).
    function play() {
        root.followPlayhead = true
        CanPlaybackModel.play()
    }

    function stop() {
        CanPlaybackModel.stop()
        traceView.positionViewAtBeginning()
    }

    function seek(seconds) {
        root.followPlayhead = true
        CanPlaybackModel.seek(seconds)
    }

    /*! The engine moves the playhead; the view follows it. One connection
        instead of a scroll call inside every seek path. */
    Connections {
        target: CanPlaybackModel

        function onCurrentIndexChanged() {
            if (root.followPlayhead && CanPlaybackModel.currentIndex >= 0)
                traceView.positionViewAtIndex(CanPlaybackModel.currentIndex, ListView.Center)
        }

        function onPlayingChanged() {
            // The engine pauses itself at the end of a non-looping recording.
            if (!CanPlaybackModel.playing && CanPlaybackModel.loaded
                    && CanPlaybackModel.position >= CanPlaybackModel.duration)
                root.report(qsTr("End of recording."), false)
        }
    }

    // --------------------------------------------------------------- loading
    function fileNameOf(fileUrl) {
        return decodeURIComponent(fileUrl.toString().split("/").pop())
    }

    function beginLoad(fileUrl) {
        pendingUrl = fileUrl
        pendingName = fileNameOf(fileUrl)
        busyMessage = qsTr("Reading %1 ...").arg(pendingName)
        busy = true
        CanPlaybackModel.pause()
        // loadLog() reads and parses on this thread. Deferring it by one frame
        // lets the overlay actually reach the screen first - otherwise a 200 MB
        // trace freezes on the previous frame and the spinner never appears.
        loadTimer.restart()
    }

    function finishLoad() {
        const ok = CanPlaybackModel.loadLog(root.pendingUrl)
        root.busy = false

        if (!ok) {
            root.report(CanPlaybackModel.lastError, true)
            return
        }

        root.followPlayhead = true
        traceView.positionViewAtBeginning()

        let message = qsTr("Loaded %1 - %2 frames over %3 s (%4 fps average).")
                      .arg(CanPlaybackModel.sourceName)
                      .arg(CanPlaybackModel.count)
                      .arg(CanPlaybackModel.duration.toFixed(3))
                      .arg(Math.round(CanPlaybackModel.averageRate()))
        // Unreadable lines are skipped, not fatal - but the user must be told,
        // or a half-parsed trace looks like a short one.
        if (CanPlaybackModel.warnings.length > 0)
            message += " " + qsTr("%1 line(s) skipped.").arg(CanPlaybackModel.warnings.length)
        root.report(message, false)
    }

    Timer {
        id: loadTimer
        interval: 16
        onTriggered: root.finishLoad()
    }

    FileDialog {
        id: openDialog
        title: qsTr("Open recorded trace")
        fileMode: FileDialog.OpenFile
        nameFilters: [ qsTr("CAN traces (*.csv *.trc *.asc)"), qsTr("All files (*)") ]
        onAccepted: root.beginLoad(selectedFile)
    }

    // --------------------------------------------------------------- status
    function report(message, isError) {
        root.statusIsError = isError === true
        root.statusMessage = message
    }

    onStatusMessageChanged: if (statusMessage.length > 0) statusTimer.restart()
    Timer {
        id: statusTimer
        // Errors stay up long enough to read a file path; notices do not.
        interval: root.statusIsError ? 10000 : 4000
        onTriggered: {
            root.statusMessage = ""
            root.statusIsError = false
        }
    }

    // --------------------------------------------------------------- toolbar
    toolbar: Component {
        RowLayout {
            spacing: Theme.spacingSm

            ToolbarButton {
                text: qsTr("Load Log")
                glyph: "▤"
                enabled: !root.busy
                onClicked: openDialog.open()
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: Theme.inputHeight - 8
                Layout.alignment: Qt.AlignVCenter
                color: Theme.border
            }

            ToolbarButton {
                text: CanPlaybackModel.playing ? qsTr("Pause") : qsTr("Play")
                glyph: CanPlaybackModel.playing ? "❚❚" : "▶"
                primary: !CanPlaybackModel.playing
                enabled: CanPlaybackModel.loaded
                onClicked: CanPlaybackModel.playing ? CanPlaybackModel.pause() : root.play()
            }
            ToolbarButton {
                text: qsTr("Stop")
                glyph: "■"
                enabled: CanPlaybackModel.loaded
                onClicked: root.stop()
            }
            ToolbarButton {
                text: qsTr("Loop")
                glyph: "⟲"
                primary: CanPlaybackModel.loop
                enabled: CanPlaybackModel.loaded
                onClicked: CanPlaybackModel.loop = !CanPlaybackModel.loop
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: Theme.inputHeight - 8
                Layout.alignment: Qt.AlignVCenter
                color: Theme.border
            }

            Label {
                text: qsTr("Speed")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMd
            }
            ThemedComboBox {
                Layout.preferredWidth: 82
                textRole: "label"
                currentIndex: 1
                model: ListModel {
                    ListElement { label: "0.5x"; factor: 0.5 }
                    ListElement { label: "1x";   factor: 1.0 }
                    ListElement { label: "2x";   factor: 2.0 }
                    ListElement { label: "5x";   factor: 5.0 }
                    ListElement { label: "10x";  factor: 10.0 }
                }
                // The engine scales elapsed wall-clock time, so changing this
                // mid-playback is seamless: no reseek, no accumulated drift.
                onActivated: CanPlaybackModel.speed = model.get(currentIndex).factor
            }
        }
    }

    // ================================================================== body
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ------------------------------------------------------- the scrubber
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 46
            color: Theme.panel

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.border
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingLg
                anchors.rightMargin: Theme.spacingLg
                spacing: Theme.spacingMd

                Label {
                    Layout.preferredWidth: 76
                    text: CanPlaybackModel.formatTime(CanPlaybackModel.position)
                    color: Theme.accent
                    font.family: Theme.monoFontFamily
                    font.pixelSize: Theme.fontSizeMd
                    font.weight: Font.DemiBold
                }

                ThemedSlider {
                    id: timeline

                    Layout.fillWidth: true
                    enabled: CanPlaybackModel.loaded
                    from: 0
                    to: Math.max(CanPlaybackModel.duration, 0.001)

                    // Dragging must win over the clock, and the clock must take
                    // over again the moment the thumb is released - a plain
                    // `value:` binding would be broken by the first drag.
                    Binding {
                        target: timeline
                        property: "value"
                        value: CanPlaybackModel.position
                        when: !timeline.pressed
                        restoreMode: Binding.RestoreBindingOrValue
                    }

                    onMoved: root.seek(value)
                }

                Label {
                    Layout.preferredWidth: 76
                    horizontalAlignment: Text.AlignRight
                    text: CanPlaybackModel.formatTime(CanPlaybackModel.duration)
                    color: Theme.textSecondary
                    font.family: Theme.monoFontFamily
                    font.pixelSize: Theme.fontSizeMd
                }
            }
        }

        // -- column header (shared with the CAN Logger)
        TableHeaderBar {
            Layout.fillWidth: true
            columns: root.traceColumns
            titles: root.traceTitles
            rightAligned: [ 0, 5 ]
        }

        // -- trace
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.background

            ListView {
                id: traceView

                anchors.fill: parent
                clip: true
                model: CanPlaybackModel.frames
                boundsBehavior: Flickable.StopAtBounds
                reuseItems: true
                cacheBuffer: 600

                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOn }

                // Reading somewhere else beats following the playhead; the
                // Follow button (and any seek) hands control back.
                onDragStarted: root.followPlayhead = false
                onFlickStarted: root.followPlayhead = false

                delegate: LogRow {
                    id: frameRow

                    required property int index
                    required property var model

                    width: ListView.view.width
                    columns: root.traceColumns

                    time: model.time
                    direction: model.direction
                    channel: model.channel
                    idText: model.idText
                    extended: model.extended
                    dlc: model.dlc
                    payload: model.payload

                    alternate: index % 2 === 1
                    // Only the delegates on screen re-evaluate this, so moving
                    // the playhead never re-renders the list.
                    active: CanPlaybackModel.currentIndex === frameRow.index
                    onClicked: root.seek(model.time)
                }
            }

            // -- follow-playhead affordance
            ToolbarButton {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Theme.spacingLg
                visible: CanPlaybackModel.loaded && !root.followPlayhead
                text: qsTr("Follow playhead")
                glyph: "⤓"
                primary: true
                onClicked: {
                    root.followPlayhead = true
                    if (CanPlaybackModel.currentIndex >= 0)
                        traceView.positionViewAtIndex(CanPlaybackModel.currentIndex, ListView.Center)
                }
            }

            // -- empty state
            ColumnLayout {
                anchors.centerIn: parent
                visible: !CanPlaybackModel.loaded
                spacing: Theme.spacingSm

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: "▶"
                    color: Theme.textDisabled
                    font.pixelSize: 40
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Load a recorded trace to replay it.")
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeMd
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("CSV, PCAN .trc and Vector .asc are read directly.")
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                }
                ToolbarButton {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Load Log")
                    glyph: "▤"
                    primary: true
                    onClicked: openDialog.open()
                }
            }
        }

        // ------------------------------------------------------ status strip
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.statusBarHeight
            color: Theme.panel

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingMd
                anchors.rightMargin: Theme.spacingMd
                spacing: Theme.spacingLg

                Rectangle {
                    id: playDot
                    width: 8; height: 8; radius: 4
                    color: CanPlaybackModel.playing ? Theme.success : Theme.textDisabled

                    SequentialAnimation on opacity {
                        running: CanPlaybackModel.playing
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.3; duration: 500 }
                        NumberAnimation { to: 1.0; duration: 500 }
                        onStopped: playDot.opacity = 1
                    }
                }
                Label {
                    text: CanPlaybackModel.playing ? qsTr("PLAYING")
                                                   : (CanPlaybackModel.loaded ? qsTr("PAUSED")
                                                                              : qsTr("NO LOG"))
                    color: CanPlaybackModel.playing ? Theme.success : Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    font.weight: Font.DemiBold
                }
                Label {
                    text: qsTr("frame %1 / %2")
                          .arg(CanPlaybackModel.currentIndex + 1).arg(CanPlaybackModel.count)
                    color: Theme.textSecondary
                    font.family: Theme.monoFontFamily
                    font.pixelSize: Theme.fontSizeSm
                }
                Label {
                    text: qsTr("%1x").arg(CanPlaybackModel.speed)
                    color: CanPlaybackModel.speed !== 1 ? Theme.accent : Theme.textSecondary
                    font.family: Theme.monoFontFamily
                    font.pixelSize: Theme.fontSizeSm
                }
                Label {
                    visible: CanPlaybackModel.loop
                    text: qsTr("loop")
                    color: Theme.accent
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                }
                Label {
                    visible: !root.followPlayhead && CanPlaybackModel.loaded
                    text: qsTr("follow off")
                    color: Theme.warning
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                }

                Item { Layout.fillWidth: true }

                Label {
                    text: root.statusMessage
                    color: root.statusIsError ? Theme.error : Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    elide: Text.ElideRight
                }
            }
        }
    }

    LoadingOverlay {
        active: root.busy
        message: root.busyMessage
    }
}
