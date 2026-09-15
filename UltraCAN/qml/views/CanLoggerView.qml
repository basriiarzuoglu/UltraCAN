import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs

import UltraCAN.Theme
import UltraCAN.Models
import UltraCAN.Components
import UltraCAN.Core

/*!
    \qmltype CanLoggerView
    \brief Real-time CAN trace window.

    Since phase 8 the frames come from the C++ backend: CanBusManager.logModel
    is a QAbstractListModel fed by the QCanBus device thread (or by the
    built-in simulator). This view no longer produces anything - Start/Stop
    just arms recording.

    Two things keep this fast under load:
      - the model is a ring buffer (CanBusManager.logModel.capacity), so memory
        is bounded no matter how long the trace runs;
      - the delegate is a plain Rectangle/Row (LogRow), reused by the ListView,
        with no Layout solving per row.
*/
BasePage {
    id: root

    title: qsTr("CAN Logger")
    // Deliberately static: a subtitle bound to frames.count would re-lay-out
    // the page header on every acquisition tick. Live numbers go in the status
    // strip, which is cheap to repaint.
    subtitle: qsTr("Live trace - ring buffer of %1 frames").arg(root.trace.capacity)
    contentMargins: 0

    // ---------------------------------------------------------------- state
    /*! The real trace model, fed by the CAN device thread. */
    readonly property var trace: CanBusManager.logModel

    /*! The user's PREFERENCE. Never written by the mode switch - see
        autoScrollActive. */
    property bool autoScroll: true

    /*!
        Whether the view actually follows the tail.

        The preference and the behaviour have to be two different things.
        Overwrite mode has no tail to follow - rows update in place and the
        list only grows when a brand-new id appears - so following it yanks the
        view back every time an id is seen for the first time, which is exactly
        when the user is trying to read the list.

        Deriving it here rather than writing `autoScroll = false` when overwrite
        turns on is what keeps the preference intact: switch overwrite off again
        and the view resumes following, because the user never asked it to stop.
        Clobbering the flag would leave them re-enabling something they never
        disabled.
    */
    readonly property bool autoScrollActive: root.autoScroll && !root.trace.overwriteMode
    property string filterText: ""
    property int selectedRow: -1
    property string statusMessage: ""

    // frames/s and the filtered row count, both refreshed once a second:
    // scanning a 20k ring buffer on every append would cost more than the
    // acquisition itself.
    property int framesPerSecond: 0
    property int lastTotal: 0
    property int matchedCount: 0

    // Snapshot export is gone with the Save Log button; LogExporter is still
    // used, but only for the format list the stream dialog offers.
    property bool statusIsError: false

    /*! Shared column geometry: Time, Dir, Ch, ID, Type, DLC, Data(flex). */
    readonly property var traceColumns: [ 86, 54, 62, 104, 64, 76, 52, 40, 0 ]
    readonly property var traceTitles: [ qsTr("Time (s)"), qsTr("Dir"), qsTr("Ch"),
                                         qsTr("ID"), qsTr("Count"), qsTr("Freq (Hz)"),
                                         qsTr("Type"), qsTr("DLC"), qsTr("Data") ]
    /*! Frequency (index 5) only means something when one row IS one identity.
        Handed to BOTH the header and every row delegate - they must hide the
        same index or the columns to its right drift. */
    readonly property var traceHidden: root.trace.overwriteMode ? [] : [ 5 ]

    /*! Which header cells sort, by the role name the proxy understands. */
    readonly property var sortableColumns: ({ 3: "canId", 4: "count",
                                              5: "frequency", 6: "type" })

    // --------------------------------------------------------------- filter
    /*! Substring match on the hex id, the DBC name or the payload. */
    function frameMatches(frame) {
        if (root.filterText.length === 0)
            return true
        const needle = root.filterText.toLowerCase()
        return frame.idText.toLowerCase().indexOf(needle) !== -1
            || frame.name.toLowerCase().indexOf(needle) !== -1
            || frame.payload.toLowerCase().indexOf(needle) !== -1
    }

    function matchCount() {
        if (root.filterText.length === 0)
            return root.trace.count
        let n = 0
        for (let i = 0; i < root.trace.count; ++i)
            if (frameMatches(root.trace.get(i)))
                ++n
        return n
    }

    onFilterTextChanged: matchedCount = matchCount()

    // -------------------------------------------------------------- actions
    function clearTrace() {
        root.trace.clear()
        selectedRow = -1
        framesPerSecond = 0
        lastTotal = 0
        report(qsTr("Trace cleared."), false)
    }

    function fileNameOf(fileUrl) {
        return decodeURIComponent(fileUrl.toString().split("/").pop())
    }

    function suffixOf(fileUrl) {
        const name = fileNameOf(fileUrl)
        const dot = name.lastIndexOf(".")
        return dot > 0 ? name.substring(dot + 1) : ""
    }

    function report(message, isError) {
        root.statusIsError = isError === true
        root.statusMessage = message
    }

    // ---------------------------------------------------------------- timers
    // Frames arrive from the C++ device thread, so instead of producing them
    // we just follow the tail when the row count grows.
    Connections {
        target: root.trace
        function onCountChanged() {
            // autoScrollActive, NOT autoScroll. Greying the button out only
            // stops CLICKS - `enabled: false` says nothing about the flag
            // behind it, which stayed true and kept calling this. That was the
            // whole bug: the button looked locked and the view still jumped.
            if (root.autoScrollActive)
                traceView.positionViewAtEnd()
        }
    }

    Timer {
        id: rateTimer
        interval: 1000
        repeat: true
        running: true
        onTriggered: {
            root.framesPerSecond = root.trace.totalCount - root.lastTotal
            root.lastTotal = root.trace.totalCount
            if (root.filterText.length > 0)
                root.matchedCount = root.matchCount()
        }
    }

    onStatusMessageChanged: if (statusMessage.length > 0) statusTimer.restart()
    Timer {
        id: statusTimer
        // An export error names a path; give it time to be read.
        interval: root.statusIsError ? 10000 : 4000
        onTriggered: {
            root.statusMessage = ""
            root.statusIsError = false
        }
    }

    /*!
        Opens the background log. The chosen extension decides the format, so
        one dialog answers both questions.
    */
    function beginStream(fileUrl) {
        SettingsManager.lastExportFolder = streamDialog.currentFolder
        SettingsManager.lastExportFormat = LogExporter.formatForSuffix(suffixOf(fileUrl))
        // The format comes from the chosen extension, exactly as export does,
        // so one dialog choice decides both the name and the writer.
        // armBackgroundLog(), not startBackgroundLog(): with no start condition
        // configured the two are the same call, and with one configured this
        // is the only path that honours it.
        if (!root.trace.armBackgroundLog(fileUrl))
            root.report(qsTr("Could not start streaming."), true)
    }

    /*! TraceStreamWriter::State - 0 idle, 1 armed, 2 recording. */
    readonly property int triggerState: root.trace ? root.trace.triggerState : 0

    TriggerSetupDialog {
        id: triggerDialog
        anchors.centerIn: parent
        trace: root.trace
    }

    Connections {
        target: root.trace

        function onStreamFailed(error) {
            root.report(error, true)
        }
        function onTriggerFired(timestampNs, preTriggerFrames) {
            root.report(preTriggerFrames > 0
                        ? qsTr("Triggered - recording, with %n pre-trigger frame(s) written.",
                               null, preTriggerFrames)
                        : qsTr("Triggered - recording."), false)
        }
        function onTriggerStopped(timestampNs) {
            root.report(qsTr("Stop condition met - file closed."), false)
        }
        function onStreamClosed(path, frames, bytes) {
            root.report(qsTr("Streamed %1 frames (%2 KB) to %3.")
                        .arg(frames).arg(Math.round(bytes / 1024))
                        .arg(root.fileNameOf(path)), false)
        }
    }

    FileDialog {
        id: streamDialog
        title: qsTr("Stream trace to disk")
        fileMode: FileDialog.SaveFile
        // Same three formats as the exporter: a streamed file and an exported
        // one are the same bytes, so anything the Log Player reads back from
        // one it reads back from the other.
        nameFilters: LogExporter.formatNames
        defaultSuffix: LogExporter.suffixFor(SettingsManager.lastExportFormat)
        onAccepted: root.beginStream(selectedFile)
    }

    // --------------------------------------------------------------- toolbar
    toolbar: Component {
        RowLayout {
            spacing: Theme.spacingSm

            // Start/Stop for the TRACE is gone: the trace listens from launch
            // and overwrite mode keeps it to one row per id, so there is
            // nothing to arm and nothing that fills up. Save Log went with it -
            // with the trace collapsed to one row per id there is no longer a
            // chronological record in the ring worth snapshotting, and the
            // background log below is the thing that keeps one.
            ToolbarButton {
                text: qsTr("Clear")
                glyph: "⟲"
                onClicked: root.clearTrace()
            }
            ToolbarButton {
                text: qsTr("Auto-Scroll")
                // All three read the EFFECTIVE state, so the button cannot
                // sit there lit and accented while doing nothing.
                glyph: root.autoScrollActive ? "⤓" : "⇵"
                primary: root.autoScrollActive
                enabled: !root.trace.overwriteMode
                onClicked: root.autoScroll = !root.autoScroll
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: root.trace.overwriteMode
                    ? qsTr("Not available in Overwrite Mode - the rows update in place, "
                           + "so there is no tail to follow")
                    : qsTr("Keep the newest frame in view")
            }
            ToolbarButton {
                text: qsTr("Overwrite Mode")
                glyph: root.trace.overwriteMode ? "⇌" : "≡"
                primary: root.trace.overwriteMode
                onClicked: {
                    root.trace.overwriteMode = !root.trace.overwriteMode
                    // Coming back to chronological with the preference still on:
                    // jump to the tail once, so following resumes from where the
                    // data is instead of from wherever the collapsed list left
                    // the viewport.
                    if (!root.trace.overwriteMode && root.autoScroll)
                        traceView.positionViewAtEnd()
                    root.report(root.trace.overwriteMode
                        ? qsTr("Overwrite mode - one row per ID, updated in place.")
                        : qsTr("Chronological mode - every frame appends a row. "
                               + "The ring holds %1 frames.").arg(root.trace.capacity), false)
                }
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("One row per ID / channel, updated in place, with a live count")
            }

            // The only recording control left, and it records to DISK, not to
            // the ring. Independent of everything above: it keeps writing while
            // the trace is collapsed, while the ring evicts, and whatever the
            // filters are showing.
            ToolbarButton {
                readonly property bool streaming: root.trace.streaming
                // The three states are not one button in three colours - they
                // are three different answers to "what is this doing right
                // now", and the label has to say which.
                readonly property bool armed: root.triggerState === 1
                readonly property bool recording: root.triggerState === 2 && streaming

                text: !streaming ? qsTr("Start Logging")
                     : armed     ? qsTr("Waiting for Trigger...")
                                 : qsTr("Recording...")
                glyph: !streaming ? "⭳" : (armed ? "◷" : "●")
                statusColor: !streaming ? "transparent"
                           : armed      ? Theme.warning
                                        : Theme.error

                onClicked: {
                    if (streaming) {
                        // One button for both: disarming a wait and stopping a
                        // recording are the same intent - "stop what you are
                        // doing" - and a separate Disarm would be a control
                        // that is disabled almost all the time.
                        root.trace.stopBackgroundLog()
                        return
                    }
                    if (SettingsManager.lastExportFolder.toString().length > 0)
                        streamDialog.currentFolder = SettingsManager.lastExportFolder
                    streamDialog.open()
                }

                // A pulse while armed, so a logger left waiting on a desk does
                // not look like a logger that is simply idle.
                SequentialAnimation on opacity {
                    running: parent.armed
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.45; duration: 700 }
                    NumberAnimation { to: 1.0;  duration: 700 }
                    onStopped: parent.opacity = 1
                }

                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: !streaming
                    ? qsTr("Record every frame on the bus straight to a file, with no size limit")
                    : armed
                      ? qsTr("Holding the last %1 s of traffic - press to give up waiting")
                        .arg(root.trace.preTriggerSeconds.toFixed(1))
                      : qsTr("Flush and close %1").arg(root.fileNameOf(root.trace.streamPath))
            }

            Label {
                // Only present when a filter is on. A capture that writes
                // nothing because of a filter is indistinguishable from a dead
                // bus, and this is the one line that tells them apart.
                visible: root.trace.logFilter().mode !== 0
                text: qsTr("FILTERED")
                color: Theme.warning
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSm
                font.weight: Font.DemiBold
                ToolTip.visible: filterHover.hovered
                ToolTip.text: qsTr("A pass filter is active - most frames are not being logged.")
                HoverHandler { id: filterHover }
            }

            ToolbarButton {
                text: qsTr("Filters && Triggers")
                glyph: "⚙"
                // Locked while a capture is in flight: changing the condition
                // under a running state machine would mean the file's own
                // header no longer describes how it was made.
                enabled: !root.trace.streaming
                onClicked: triggerDialog.open()
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: root.trace.streaming
                    ? qsTr("Stop the current capture to change the triggers")
                    : qsTr("Wait for a CAN ID or a signal value before writing, and keep the seconds before it")
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: Theme.inputHeight - 8
                Layout.alignment: Qt.AlignVCenter
                color: Theme.border
            }

            ThemedTextField {
                Layout.preferredWidth: 170
                placeholderText: qsTr("Filter ID, name or data...")
                onTextChanged: root.filterText = text
            }
        }
    }

    // ================================================================== body
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // -- column header
        TableHeaderBar {
            Layout.fillWidth: true
            id: traceHeader

            columns: root.traceColumns
            titles: root.traceTitles
            hiddenColumns: root.traceHidden
            rightAligned: [ 0, 4, 5, 7 ]   // time, count, frequency, DLC
            sortableColumns: root.sortableColumns
            sortRoleName: TraceSortModel.sortRoleName
            sortDescending: TraceSortModel.sortDescending
            sortEnabled: TraceSortModel.sortingAvailable
            onSortRequested: function (roleName) {
                TraceSortModel.toggleSort(roleName)
                // A fresh sort scrolls to the top: the row the user wants after
                // asking for "busiest first" is the first one, and leaving the
                // viewport where it was hides the answer.
                traceView.positionViewAtBeginning()
            }
        }

        // -- the trace
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.background

            ListView {
                id: traceView

                anchors.fill: parent
                clip: true
                // The PROXY, not the trace. It is a pass-through until
                // overwrite mode turns sorting on, so chronological order is
                // untouched.
                model: TraceSortModel
                boundsBehavior: Flickable.StopAtBounds

                // Delegate recycling is what keeps a 20k-row trace smooth.
                reuseItems: true
                cacheBuffer: 600

                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOn }

                // Scrolling by hand means "I want to look at something" -
                // stop yanking the view back to the bottom.
                //
                // movementStarted is the one that catches the MOUSE WHEEL:
                // dragStarted and flickStarted cover touch and click-drag, and
                // a wheel notch fires neither, so wheel-scrolling a live trace
                // used to fight the auto-scroll all the way down.
                //
                // Safe to listen to because positionViewAtEnd() is a direct
                // reposition, not a movement - verified: it fires none of these
                // three - so auto-scroll cannot switch itself off.
                onDragStarted: root.autoScroll = false
                onFlickStarted: root.autoScroll = false
                onMovementStarted: root.autoScroll = false

                delegate: LogRow {
                    id: frameRow

                    required property int index
                    required property var model

                    width: ListView.view.width
                    columns: root.traceColumns
                    hiddenColumns: root.traceHidden

                    // Filtered-out rows collapse instead of being removed, so the
                    // row indexes the view hands out stay contiguous.
                    visible: root.frameMatches(model)
                    height: visible ? Theme.rowHeight : 0

                    time: model.time
                    direction: model.direction
                    channel: model.channel
                    idText: model.idText
                    extended: model.extended
                    count: model.count
                    frequency: model.frequency
                    dlc: model.dlc
                    payload: model.payload

                    alternate: index % 2 === 1
                    selected: root.selectedRow === index
                    onClicked: root.selectedRow = index
                }
            }

            // empty state
            ColumnLayout {
                anchors.centerIn: parent
                visible: root.trace.count === 0
                spacing: Theme.spacingSm

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: "🛈"
                    color: Theme.textDisabled
                    font.pixelSize: 36
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: CanBusManager.connected ? qsTr("Waiting for frames...")
                                       : qsTr("Trace is empty - connect a bus to see traffic.")
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeMd
                }
            }
        }

        // -- selected frame detail
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.rowHeight + 4
            visible: root.selectedRow >= 0 && root.selectedRow < TraceSortModel.count
            color: Theme.panel

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            RowLayout {
                id: detailRow

                anchors.fill: parent
                anchors.leftMargin: Theme.spacingMd
                anchors.rightMargin: Theme.spacingMd
                spacing: Theme.spacingMd

                // The delegate hands out PROXY rows; get() indexes the SOURCE.
                // Without the mapping this panel still shows a frame - just
                // not the one that was clicked, which is the failure mode that
                // never gets noticed.
                readonly property var frame: {
                    if (root.selectedRow < 0 || root.selectedRow >= TraceSortModel.count)
                        return null
                    const src = TraceSortModel.sourceRow(root.selectedRow)
                    return src >= 0 ? root.trace.get(src) : null
                }

                Label {
                    text: qsTr("SELECTED")
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    font.letterSpacing: 1.1
                }
                Label {
                    text: detailRow.frame ? detailRow.frame.idText : ""
                    color: Theme.accent
                    font.family: Theme.monoFontFamily
                    font.pixelSize: Theme.fontSizeMd
                    font.weight: Font.DemiBold
                }
                Label {
                    text: detailRow.frame ? detailRow.frame.name : ""
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeMd
                }
                Label {
                    Layout.fillWidth: true
                    text: detailRow.frame ? qsTr("%1 - %2 bytes - %3")
                                         .arg(detailRow.frame.channel)
                                         .arg(detailRow.frame.dlc)
                                         .arg(detailRow.frame.payload)
                                       : ""
                    color: Theme.textSecondary
                    font.family: Theme.monoFontFamily
                    font.pixelSize: Theme.fontSizeSm
                    elide: Text.ElideRight
                }
                ToolbarButton {
                    text: qsTr("Deselect")
                    onClicked: root.selectedRow = -1
                }
            }
        }

        // -- status strip
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

                // A fabricated trace must never be mistakable for a real one.
                // The generator is off by default now, so this badge is the
                // only way it can ever be on without the user knowing.
                Rectangle {
                    visible: CanBusManager.simulating
                    Layout.preferredHeight: 16
                    Layout.preferredWidth: simLabel.implicitWidth + Theme.spacingSm * 2
                    radius: Theme.radiusSm
                    color: Theme.accentAlt

                    Label {
                        id: simLabel
                        anchors.centerIn: parent
                        text: qsTr("SIMULATED")
                        color: Theme.background
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.8
                    }
                }

                Rectangle {
                    id: stateDot
                    width: 8; height: 8; radius: 4
                    color: CanBusManager.connected ? Theme.success : Theme.textDisabled

                    SequentialAnimation on opacity {
                        running: CanBusManager.connected
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.25; duration: 500 }
                        NumberAnimation { to: 1.0; duration: 500 }
                        onStopped: stateDot.opacity = 1
                    }
                }
                Label {
                    // The trace always listens now, so this stopped being a
                    // recording state and became a bus state - which is the
                    // question the user was really asking it.
                    text: CanBusManager.connected ? qsTr("LISTENING") : qsTr("NO BUS")
                    color: CanBusManager.connected ? Theme.success : Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    font.weight: Font.DemiBold
                }

                Label {
                    text: root.filterText.length > 0
                          ? qsTr("%1 / %2 frames").arg(root.matchedCount)
                                                  .arg(root.trace.count)
                          : qsTr("%1 frames").arg(root.trace.count)
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                }
                Label {
                    text: qsTr("%1 fps").arg(root.framesPerSecond)
                    color: Theme.textSecondary
                    font.family: Theme.monoFontFamily
                    font.pixelSize: Theme.fontSizeSm
                }

                // -- disk stream, only while one is open
                Rectangle {
                    visible: root.trace.streaming
                    Layout.preferredHeight: 16
                    Layout.preferredWidth: streamLabel.implicitWidth + Theme.spacingSm * 2
                    radius: Theme.radiusSm
                    color: Theme.error

                    // Blinks, because a file quietly growing on disk is the
                    // kind of state a user forgets is on.
                    SequentialAnimation on opacity {
                        running: root.trace.streaming
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.45; duration: 700 }
                        NumberAnimation { to: 1.0;  duration: 700 }
                    }

                    Label {
                        id: streamLabel
                        anchors.centerIn: parent
                        text: qsTr("REC")
                        color: Theme.selectionText
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        font.weight: Font.DemiBold
                    }
                }
                Label {
                    visible: root.trace.streaming
                    // streamFrames, not trace.count - the point of the feature
                    // is that these two diverge once the ring starts dropping.
                    text: qsTr("%1 frames -> %2 (%3 KB)")
                          .arg(root.trace.streamFrames)
                          .arg(root.fileNameOf(root.trace.streamPath))
                          .arg(Math.round(root.trace.streamBytes / 1024))
                    color: Theme.textSecondary
                    font.family: Theme.monoFontFamily
                    font.pixelSize: Theme.fontSizeSm
                    elide: Text.ElideMiddle
                    Layout.maximumWidth: 320
                }
                Label {
                    text: qsTr("t = %1 s").arg(CanBusManager.busTime.toFixed(3))
                    color: Theme.textSecondary
                    font.family: Theme.monoFontFamily
                    font.pixelSize: Theme.fontSizeSm
                }
                Label {
                    visible: root.trace.droppedCount > 0
                    text: qsTr("%1 dropped (ring buffer full)").arg(root.trace.droppedCount)
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
                Label {
                    text: qsTr("buffer %1 %")
                          .arg(Math.round(root.trace.count / root.trace.capacity * 100))
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                }
            }
        }
    }

}
