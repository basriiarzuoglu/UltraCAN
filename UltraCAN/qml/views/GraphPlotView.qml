import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs

import UltraCAN.Theme
import UltraCAN.Components
import UltraCAN.Core

/*!
    \qmltype GraphPlotView
    \brief Live scope: splittable panes, its own databases, detachable to a
           second monitor.

    \section1 What changed and why

    The old left-hand signal tree is gone. It listed the DBC Editor's document,
    which tied the scope to whatever file someone happened to be authoring, and
    it could only target one chart because there was only one. Signals are now
    picked per pane, from this view's OWN databases, through the pane's context
    menu - so "plot the brake pressure on the bottom-right pane" is one gesture
    instead of a checkbox in a shared list.

    \section1 Workspaces

    The left half is a tab strip over a \c StackLayout of \l GraphWorkspace
    items - one per tab, each an independent pane tree. "Steer PID" and "Brake
    PID" are two tabs, not two applications.

    The DBC manager on the right is deliberately OUTSIDE the tabs. Databases
    are a property of the bus, not of a view onto it, and \c GraphDataModel
    holds exactly one list of them; giving each tab its own panel would have
    been three copies of one truth.

    \section1 Detaching

    Detaching is per WORKSPACE, not per page - see \l GraphWorkspace. The tab
    stays in the strip while its charts are away, which is what makes
    "re-attach at the original index" free: the inline half never left.
*/
BasePage {
    id: root

    title: qsTr("Graph Plot")
    subtitle: GraphDataModel.databaseCount === 0
              ? qsTr("Load a database on the right to start plotting")
              : qsTr("%1 database(s) - %2 trace(s) on %3 pane(s)")
                .arg(GraphDataModel.databaseCount)
                .arg(GraphDataModel.trackedCount)
                .arg(root.currentWorkspace ? root.currentWorkspace.paneCount : 0)
    contentMargins: 0

    /*!
        The workspace the toolbar acts on.

        Written as a computed binding rather than \c workspaceStack.itemAt()
        called at the point of use, because itemAt() is a METHOD - a call to it
        creates no dependency and the Detach button would keep pointing at
        whichever tab happened to be current when it was built. Reading
        \c currentIndex and \c count inside the binding is what makes it track
        both tab switches and tab creation. Verified: a Repeater inside a
        StackLayout does not count as a page of it, so these indices are the
        tab indices.
    */
    readonly property var currentWorkspace: {
        const i = workspaceStack.currentIndex
        return (i >= 0 && i < workspaceStack.count) ? workspaceStack.itemAt(i) : null
    }

    property int nextWorkspaceSerial: 1

    /*! Appends a tab and selects it. Returns its index, or -1. */
    function addWorkspace(label) {
        const serial = root.nextWorkspaceSerial
        root.nextWorkspaceSerial += 1
        workspaceModel.append({
            wsId: "W" + serial,
            wsName: label && label.length > 0 ? label : qsTr("Workspace %1").arg(serial),
            wsDetached: false
        })
        const index = workspaceModel.count - 1
        tabBar.setCurrentIndex(index)
        return index
    }

    /*!
        Removes a tab. The last one stays - an empty StackLayout would leave
        the page with nothing to show and no way to get a tab back.

        Removing the ListModel row is what destroys the GraphWorkspace, whose
        Component.onDestruction shuts its window and drops its tracks. Doing it
        in that order matters: reaching for the item after the remove would be
        reaching for a deleted object.
    */
    function closeWorkspace(index) {
        if (workspaceModel.count <= 1)
            return
        if (index < 0 || index >= workspaceModel.count)
            return

        const name = workspaceModel.get(index).wsName
        workspaceModel.remove(index)
        tabBar.setCurrentIndex(Math.min(index, workspaceModel.count - 1))
        root.statusMessage = qsTr("Closed “%1”.").arg(name)
    }

    // ------------------------------------------------- session export / import
    //
    // The page is the only thing that knows how many tabs there are, so the
    // tab dimension is assembled here and each tab serialises itself.

    /*! { tabs: [ <GraphWorkspace.exportState()> ], current: int } */
    function exportGraphState() {
        let tabs = []
        for (let i = 0; i < workspaceStack.count; ++i) {
            const item = workspaceStack.itemAt(i)
            if (item)
                tabs.push(item.exportState())
        }
        return { tabs: tabs, current: tabBar.currentIndex }
    }

    /*! Rebuilds the whole tab strip from an imported document.

        Two passes on purpose. Appending a row creates the GraphWorkspace
        delegate, but the delegate's own children - the GraphPaneArea and the
        first pane it mints in Component.onCompleted - are only finished once
        the current call returns to the event loop. Applying a tab's layout in
        the same pass would race that construction, so the rows go in first and
        the contents follow on the next turn.
    */
    function applyGraphState(state) {
        const tabs = (state && state.tabs && state.tabs.length > 0) ? state.tabs : null
        if (!tabs) {
            root.statusMessage = qsTr("That session has no graph workspace; the tabs are unchanged.")
            return
        }

        // Tear the strip down to nothing. shutdown() on each item closes any
        // floating window and drops that tab's tracks, so no traces survive
        // into the imported layout.
        for (let i = workspaceStack.count - 1; i >= 0; --i) {
            const item = workspaceStack.itemAt(i)
            if (item)
                item.shutdown()
        }
        workspaceModel.clear()

        for (let t = 0; t < tabs.length; ++t) {
            const serial = root.nextWorkspaceSerial
            root.nextWorkspaceSerial += 1
            const label = (tabs[t] && String(tabs[t].name || "").length > 0)
                        ? String(tabs[t].name) : qsTr("Workspace %1").arg(serial)
            workspaceModel.append({ wsId: "W" + serial, wsName: label, wsDetached: false })
        }

        root.pendingImport = state
        Qt.callLater(root.finishImport)
    }

    /*! Second pass of applyGraphState(). */
    property var pendingImport: null
    /*! Sidebar disclosure for the math-channel list. Open by default: the
        Create button lives inside it, and a feature hidden behind a collapsed
        header that nothing hints at is a feature nobody finds. */
    property bool virtualsExpanded: true
    function finishImport() {
        const state = root.pendingImport
        root.pendingImport = null
        if (!state || !state.tabs)
            return

        let unresolved = 0
        for (let i = 0; i < state.tabs.length && i < workspaceStack.count; ++i) {
            const item = workspaceStack.itemAt(i)
            if (item)
                unresolved += item.applyState(state.tabs[i])
        }

        const wanted = Number(state.current)
        tabBar.setCurrentIndex((wanted >= 0 && wanted < workspaceStack.count) ? wanted : 0)

        // Said out loud rather than swallowed: a pane that came back empty
        // because its database moved looks exactly like a pane the user never
        // filled in, and they would spend the next ten minutes wondering why
        // the trace is not drawing.
        root.statusMessage = unresolved === 0
            ? qsTr("Workspace restored - %1 tab(s).").arg(state.tabs.length)
            : qsTr("Workspace restored - %1 signal(s) are not in the loaded databases.")
              .arg(unresolved)
    }

    Connections {
        target: ConfigManager

        // Synchronous: ConfigManager emits this from inside exportConfig() and
        // reads the value straight back, so there is nothing to await.
        function onGraphStateRequested() {
            ConfigManager.setGraphState(root.exportGraphState())
        }

        // Fired only after every database named in the file is loaded, which
        // is what lets applyState() resolve its signals by name.
        function onGraphStateRestored() {
            root.applyGraphState(ConfigManager.graphState())
        }
    }

    ListModel {
        id: workspaceModel
        // Rows are appended at runtime, so no ListElement literals here:
        // ListElement only accepts constants and every field below is either
        // generated (wsId) or translated (wsName).
    }

    Component.onCompleted: root.addWorkspace(qsTr("Workspace 1"))

    property string statusMessage: ""
    onStatusMessageChanged: if (statusMessage.length > 0) statusTimer.restart()
    Timer {
        id: statusTimer
        interval: 5000
        onTriggered: root.statusMessage = ""
    }

    /*!
        Only reached when the whole application is going down, now that the
        shell keeps this page alive across navigation. Each workspace closes
        its own window and drops its own tracks through
        \c Component.onDestruction; the sweep afterwards is the safety net for
        anything a pane failed to untrack, and is only safe here because no
        other page owns tracks.
    */
    Component.onDestruction: GraphDataModel.untrackAll()

    // --------------------------------------------------------------- toolbar
    toolbar: Component {
        RowLayout {
            spacing: Theme.spacingSm

            ToolbarButton {
                readonly property string iconRoot: "qrc:/qt/qml/UltraCAN/assets/icons/"

                text: GraphDataModel.paused ? qsTr("Resume") : qsTr("Pause")
                // Was glyph: "▶" / "❚❚". U+275A is not in Segoe UI, so the
                // pause state rendered as a fallback font or a tofu box
                // depending on the machine. Real artwork, and the RESOURCES
                // prefix is qrc:/qt/qml/<URI>/... - qrc:/icons/... resolves to
                // nothing and shows a blank button with no error.
                iconSource: iconRoot + (GraphDataModel.paused ? "play.png" : "pause.png")
                // Unchanged: the toggled state is still the accent fill, and
                // the white artwork reads on it as well as on the panel.
                primary: GraphDataModel.paused
                onClicked: GraphDataModel.paused = !GraphDataModel.paused
            }
            ToolbarButton {
                text: qsTr("Clear")
                glyph: "⟲"
                onClicked: GraphDataModel.clear()
            }
            ToolbarButton {
                text: qsTr("Remove all traces")
                destructive: true
                enabled: GraphDataModel.trackedCount > 0
                onClicked: {
                    GraphDataModel.untrackAll()
                    root.statusMessage = qsTr("All traces removed.")
                }
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: Theme.inputHeight - 8
                Layout.alignment: Qt.AlignVCenter
                color: Theme.border
            }

            Label {
                text: qsTr("Window")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMd
            }
            ThemedComboBox {
                Layout.preferredWidth: 90
                textRole: "label"
                // Index 4 = "10 s", the same default as before - two entries
                // were inserted above it, so the literal had to move with them.
                currentIndex: 4
                model: ListModel {
                    // 0.5 s is the floor GraphDataModel already clamps to, so
                    // nothing below it can be offered. The window is only ever
                    // SUBTRACTED from latestTime (there is no denominator), so
                    // sub-second ranges need no special handling in C++.
                    ListElement { label: "0.5 s"; seconds: 0.5 }
                    ListElement { label: "1 s";  seconds: 1.0 }
                    ListElement { label: "2 s";  seconds: 2.0 }
                    ListElement { label: "5 s";  seconds: 5.0 }
                    ListElement { label: "10 s"; seconds: 10.0 }
                    ListElement { label: "30 s"; seconds: 30.0 }
                    ListElement { label: "60 s"; seconds: 60.0 }
                }
                onActivated: GraphDataModel.windowSeconds = model.get(currentIndex).seconds
            }

            // The 1 / 2 / 2 / 4 layout presets used to sit here. They are gone
            // with the fixed four-slot layout: a preset can only name an
            // arrangement it can enumerate, and the tree no longer has a finite
            // set of them. Splitting is per-pane now - right-click a pane, or
            // use the X in its corner to close it.

            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: Theme.inputHeight - 8
                Layout.alignment: Qt.AlignVCenter
                color: Theme.border
            }

            ToolbarButton {
                readonly property var ws: root.currentWorkspace
                readonly property bool away: ws ? ws.detached : false

                // Acts on the ACTIVE TAB, not on the page. The other tabs keep
                // whatever state they are in.
                text: away ? qsTr("Re-attach") : qsTr("Detach")
                glyph: away ? "⤡" : "⤢"
                primary: away
                enabled: ws !== null
                onClicked: {
                    if (!ws)
                        return
                    if (away)
                        ws.reattach()
                    else
                        ws.detach()
                }
            }
        }
    }

    /*!
        Plots \a channelId. An empty \a paneId means "the pane the user last
        interacted with", falling back to the first one - so a single-pane
        workspace, which is most of them, never asks a question it can answer.
    */
    function plotMathChannel(channelId, paneId) {
        const workspace = root.currentWorkspace
        if (!workspace) {
            root.statusMessage = qsTr("No workspace is open.")
            return
        }

        let target = paneId
        if (target.length === 0) {
            const active = workspace.area ? workspace.area.activePaneId : ""
            const panes = workspace.paneIds()
            target = (active.length > 0 && panes.indexOf(active) >= 0)
                     ? active : (panes.length > 0 ? panes[0] : "")
        }
        if (target.length === 0) {
            root.statusMessage = qsTr("This workspace has no pane to plot on.")
            return
        }

        if (!workspace.addMathChannelTo(target, channelId))
            root.statusMessage = qsTr("That pane is gone - pick another.")
    }

    Menu {
        id: paneMenu

        property int channelId: -1
        property string channelName: ""
        property var paneList: []

        Repeater {
            model: paneMenu.paneList

            delegate: MenuItem {
                required property string modelData

                text: qsTr("Add “%1” to %2").arg(paneMenu.channelName).arg(modelData)
                onTriggered: root.plotMathChannel(paneMenu.channelId, modelData)
            }
        }
    }

    MathChannelDialog {
        id: mathDialog
        anchors.centerIn: parent
        onChannelCreated: function (channelId) {
            root.statusMessage = qsTr("Math channel created. Add it to a pane from its + menu.")
        }
    }

    FileDialog {
        id: graphDbcDialog
        title: qsTr("Load a database for the graph")
        fileMode: FileDialog.OpenFile
        nameFilters: [ qsTr("CAN databases (*.dbc)"), qsTr("All files (*)") ]
        onAccepted: {
            if (!GraphDataModel.loadGraphDbc(selectedFile))
                root.statusMessage = GraphDataModel.lastError
        }
    }

    FileDialog {
        id: logDialog
        title: qsTr("Open recorded trace")
        fileMode: FileDialog.OpenFile
        nameFilters: [ qsTr("CAN traces (*.csv *.trc *.asc)"), qsTr("All files (*)") ]
        onAccepted: {
            if (CanPlaybackModel.loadLog(selectedFile))
                root.statusMessage = qsTr("Loaded %1.").arg(CanPlaybackModel.sourceName)
            else
                root.statusMessage = CanPlaybackModel.lastError
        }
    }

    // ================================================================== body
    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        handle: Rectangle {
            implicitWidth: 4
            color: SplitHandle.pressed ? Theme.accent
                 : (SplitHandle.hovered ? Theme.panelElevated : Theme.border)
        }

        // ------------------------------------------------- LEFT: the scope
        ColumnLayout {
            SplitView.fillWidth: true
            SplitView.minimumWidth: 420
            spacing: 0

            // ------------------------------------------------- the tab strip
            //
            // A real TabBar, restyled rather than replaced: the Basic style
            // paints a light-grey strip that would be the only unthemed
            // surface in the application, so background and delegate are both
            // overridden. What TabBar still buys us is the container
            // behaviour - one checked item, arrow-key navigation, and correct
            // handling of a Repeater as its content.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                color: Theme.panel

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: Theme.border
                }

                RowLayout {
                    anchors.fill: parent
                    spacing: 0

                    TabBar {
                        id: tabBar

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        padding: 0
                        background: Rectangle { color: "transparent" }

                        Repeater {
                            model: workspaceModel

                            delegate: TabButton {
                                id: tabDelegate

                                required property int index
                                required property string wsName
                                required property bool wsDetached

                                // Content width, not an equal share: TabBar
                                // divides its width between tabs only while
                                // they have none of their own, and eight
                                // equally squeezed tabs are unreadable.
                                width: Math.min(210, Math.max(118, tabLabel.implicitWidth + 56))
                                height: tabBar.height
                                padding: 0

                                background: Rectangle {
                                    color: tabDelegate.checked ? Theme.surface
                                         : (tabDelegate.hovered ? Theme.panelElevated : Theme.panel)

                                    Rectangle {
                                        anchors.top: parent.top
                                        width: parent.width
                                        height: 2
                                        color: Theme.accent
                                        visible: tabDelegate.checked
                                    }
                                    Rectangle {
                                        anchors.right: parent.right
                                        width: 1
                                        height: parent.height
                                        color: Theme.border
                                    }
                                }

                                contentItem: RowLayout {
                                    spacing: Theme.spacingXs

                                    Item { Layout.preferredWidth: Theme.spacingSm - Theme.spacingXs }

                                    Label {
                                        // Lit whenever this tab's charts are in
                                        // their own window - otherwise a tab
                                        // showing the "it is elsewhere"
                                        // placeholder looks simply broken.
                                        visible: tabDelegate.wsDetached
                                        text: "⤢"
                                        color: Theme.accentAlt
                                        font.pixelSize: Theme.fontSizeSm
                                    }

                                    Label {
                                        id: tabLabel

                                        Layout.fillWidth: true
                                        text: tabDelegate.wsName
                                        color: tabDelegate.checked ? Theme.textPrimary
                                                                   : Theme.textSecondary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontSizeMd
                                        elide: Text.ElideRight
                                        verticalAlignment: Text.AlignVCenter
                                    }

                                    Rectangle {
                                        Layout.preferredWidth: 16
                                        Layout.preferredHeight: 16
                                        Layout.rightMargin: Theme.spacingXs
                                        Layout.alignment: Qt.AlignVCenter
                                        radius: 2
                                        // The last tab cannot be closed, for
                                        // the same reason the last pane cannot.
                                        visible: workspaceModel.count > 1
                                        color: closeArea.containsMouse ? Theme.closeHover : "transparent"

                                        Label {
                                            anchors.centerIn: parent
                                            text: "✕"
                                            color: closeArea.containsMouse ? Theme.selectionText
                                                                           : Theme.textDisabled
                                            font.pixelSize: Theme.fontSizeSm
                                        }

                                        MouseArea {
                                            id: closeArea
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            // A plain MouseArea, so the press
                                            // never reaches the TabButton
                                            // underneath and the tab is not
                                            // selected on its way out.
                                            onClicked: root.closeWorkspace(tabDelegate.index)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // -- new tab. Outside the TabBar on purpose: inside it, it
                    //    would be a checkable tab that can become "current".
                    Rectangle {
                        Layout.preferredWidth: 30
                        Layout.fillHeight: true
                        color: addArea.containsMouse ? Theme.panelElevated : Theme.panel

                        Rectangle {
                            anchors.left: parent.left
                            width: 1
                            height: parent.height
                            color: Theme.border
                        }

                        Label {
                            anchors.centerIn: parent
                            text: "+"
                            color: addArea.containsMouse ? Theme.accent : Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeLg
                        }

                        MouseArea {
                            id: addArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: root.addWorkspace("")
                        }

                        ToolTip.visible: addArea.containsMouse
                        ToolTip.delay: 500
                        ToolTip.text: qsTr("New graph workspace")
                    }
                }
            }

            // ------------------------------------------------- the workspaces
            //
            // A StackLayout, NOT a Loader per tab: switching tabs must not
            // destroy the panes of the one being left. Every workspace stays
            // instantiated and only its visibility changes, which is the same
            // guarantee the shell now gives this page.
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.background

                StackLayout {
                    id: workspaceStack
                    anchors.fill: parent
                    currentIndex: tabBar.currentIndex

                    Repeater {
                        model: workspaceModel

                        delegate: GraphWorkspace {
                            required property int index
                            required property string wsId
                            required property string wsName

                            workspaceId: wsId
                            name: wsName

                            // Mirrored back into the row so the tab delegate can
                            // bind to it. The delegates are siblings, not
                            // ancestor and descendant, so the model row is the
                            // only channel between them - and setProperty()
                            // notifies, where poking the item would not.
                            onDetachedChanged: workspaceModel.setProperty(index, "wsDetached", detached)
                            onStatusPosted: function (text) { root.statusMessage = text }
                        }
                    }
                }
            }

            // ------------------------------------------------- playback bar
            //
            // The CAN Log Player used to be its own page with its own trace
            // table. The table was the redundant half: replayed frames now go
            // through CanBusManager like any other source, so the CAN Logger
            // already shows them. What was actually worth keeping is the
            // transport, and it belongs against the graphs it drives.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                color: CanBusManager.playbackSource ? Theme.selection : Theme.panel

                Rectangle { width: parent.width; height: 1; color: Theme.border }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingMd
                    anchors.rightMargin: Theme.spacingMd
                    spacing: Theme.spacingSm

                    ToolbarButton {
                        text: qsTr("Load Log")
                        glyph: "▤"
                        onClicked: logDialog.open()
                    }
                    ToolbarButton {
                        // Same artwork as the graph's Pause, for the same
                        // reason - and so the two transports in one view do
                        // not use two different pause symbols.
                        text: CanPlaybackModel.playing ? qsTr("Pause") : qsTr("Play")
                        iconSource: "qrc:/qt/qml/UltraCAN/assets/icons/"
                                  + (CanPlaybackModel.playing ? "pause.png" : "play.png")
                        primary: !CanPlaybackModel.playing && CanPlaybackModel.loaded
                        enabled: CanPlaybackModel.loaded
                        onClicked: CanPlaybackModel.playing ? CanPlaybackModel.pause()
                                                            : CanPlaybackModel.play()
                    }
                    ToolbarButton {
                        text: qsTr("Stop")
                        glyph: "■"
                        enabled: CanPlaybackModel.loaded
                        onClicked: CanPlaybackModel.stop()
                    }
                    ToolbarButton {
                        text: qsTr("Loop")
                        glyph: "⟲"
                        primary: CanPlaybackModel.loop
                        enabled: CanPlaybackModel.loaded
                        onClicked: CanPlaybackModel.loop = !CanPlaybackModel.loop
                    }

                    ThemedComboBox {
                        Layout.preferredWidth: 78
                        textRole: "label"
                        currentIndex: 1
                        enabled: CanPlaybackModel.loaded
                        model: ListModel {
                            ListElement { label: "0.5x"; factor: 0.5 }
                            ListElement { label: "1x";   factor: 1.0 }
                            ListElement { label: "2x";   factor: 2.0 }
                            ListElement { label: "5x";   factor: 5.0 }
                            ListElement { label: "10x";  factor: 10.0 }
                        }
                        onActivated: CanPlaybackModel.speed = model.get(currentIndex).factor
                    }

                    Label {
                        Layout.preferredWidth: 74
                        text: CanPlaybackModel.formatTime(CanPlaybackModel.position)
                        color: Theme.accent
                        font.family: Theme.monoFontFamily
                        font.pixelSize: Theme.fontSizeSm
                        font.weight: Font.DemiBold
                    }

                    ThemedSlider {
                        id: timeline

                        Layout.fillWidth: true
                        enabled: CanPlaybackModel.loaded
                        from: 0
                        to: Math.max(CanPlaybackModel.duration, 0.001)

                        // Dragging must win over the clock, and the clock must
                        // take over the moment the thumb is released - a plain
                        // `value:` binding is broken by the first drag.
                        Binding {
                            target: timeline
                            property: "value"
                            value: CanPlaybackModel.position
                            when: !timeline.pressed
                            restoreMode: Binding.RestoreBindingOrValue
                        }

                        onMoved: CanPlaybackModel.seek(value)
                    }

                    Label {
                        Layout.preferredWidth: 74
                        horizontalAlignment: Text.AlignRight
                        text: CanPlaybackModel.formatTime(CanPlaybackModel.duration)
                        color: Theme.textSecondary
                        font.family: Theme.monoFontFamily
                        font.pixelSize: Theme.fontSizeSm
                    }

                    Label {
                        visible: CanBusManager.playbackSource
                        text: qsTr("VIRTUAL BUS")
                        color: Theme.accentAlt
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        font.weight: Font.DemiBold
                        // The one thing the user must never have to guess:
                        // while this is lit, the hardware is being ignored.
                        ToolTip.visible: busHover.hovered
                        ToolTip.text: qsTr("Live frames are muted while a log plays. Press Stop to return to the bus.")
                        HoverHandler { id: busHover }
                    }
                }
            }

            // -------------------------------------------------- status strip
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
                        id: liveDot
                        width: 8; height: 8; radius: 4
                        color: GraphDataModel.paused ? Theme.warning : Theme.success

                        SequentialAnimation on opacity {
                            running: !GraphDataModel.paused && GraphDataModel.trackedCount > 0
                            loops: Animation.Infinite
                            NumberAnimation { to: 0.3; duration: 600 }
                            NumberAnimation { to: 1.0; duration: 600 }
                            onStopped: liveDot.opacity = 1
                        }
                    }
                    Label {
                        text: GraphDataModel.paused ? qsTr("PAUSED")
                            : (CanBusManager.playbackSource ? qsTr("PLAYBACK") : qsTr("LIVE"))
                        color: GraphDataModel.paused ? Theme.warning
                             : (CanBusManager.playbackSource ? Theme.accentAlt : Theme.success)
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        font.weight: Font.DemiBold
                    }
                    Label {
                        text: qsTr("t = %1 s").arg(GraphDataModel.latestTime.toFixed(3))
                        color: Theme.textSecondary
                        font.family: Theme.monoFontFamily
                        font.pixelSize: Theme.fontSizeSm
                    }
                    Label {
                        text: qsTr("%1 traces").arg(GraphDataModel.trackedCount)
                        color: Theme.textSecondary
                        font.family: Theme.monoFontFamily
                        font.pixelSize: Theme.fontSizeSm
                    }
                    Label {
                        text: qsTr("%1 samples/s").arg(GraphDataModel.sampleRate)
                        color: Theme.textSecondary
                        font.family: Theme.monoFontFamily
                        font.pixelSize: Theme.fontSizeSm
                    }
                    Label {
                        text: qsTr("%1 pts").arg(GraphDataModel.pointCount)
                        color: Theme.textDisabled
                        font.family: Theme.monoFontFamily
                        font.pixelSize: Theme.fontSizeSm
                    }

                    Item { Layout.fillWidth: true }

                    Label {
                        text: root.statusMessage
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        elide: Text.ElideRight
                        Layout.maximumWidth: 360
                    }
                }
            }
        }

        // ------------------------------------------- RIGHT: DBC manager panel
        //
        // The graph's databases, like the analyzer's, are its own. Loading one
        // here does not touch the DBC Editor's document and the Editor's
        // document does not appear here.
        ColumnLayout {
            SplitView.preferredWidth: 250
            SplitView.minimumWidth: 210
            SplitView.maximumWidth: 420
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.toolbarHeight
                color: Theme.panel

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: Theme.border
                }

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    x: Theme.spacingMd
                    text: qsTr("GRAPH DATABASES")
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    font.letterSpacing: 1.1
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: graphDbcActions.implicitHeight + Theme.spacingSm * 2
                color: Theme.surface

                ColumnLayout {
                    id: graphDbcActions

                    x: Theme.spacingSm
                    y: Theme.spacingSm
                    width: parent.width - Theme.spacingSm * 2
                    spacing: Theme.spacingXs

                    ToolbarButton {
                        Layout.fillWidth: true
                        text: qsTr("Load DBC")
                        glyph: "+"
                        primary: true
                        onClicked: graphDbcDialog.open()
                    }
                    ToolbarButton {
                        Layout.fillWidth: true
                        text: qsTr("Add editor's DBC")
                        glyph: "↧"
                        enabled: DbcModel.sourceUrl.toString().length > 0
                        onClicked: {
                            if (!GraphDataModel.loadGraphDbc(DbcModel.sourceUrl))
                                root.statusMessage = GraphDataModel.lastError
                        }
                    }
                    ToolbarButton {
                        Layout.fillWidth: true
                        text: qsTr("Remove all")
                        destructive: true
                        enabled: GraphDataModel.databaseCount > 0
                        onClicked: GraphDataModel.clearGraphDbcs()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.background

                ListView {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingSm
                    clip: true
                    spacing: Theme.spacingXs
                    model: GraphDataModel.loadedDbcs

                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    delegate: Rectangle {
                        id: dbcRow

                        required property int index
                        required property var modelData

                        width: ListView.view.width
                        height: dbcInfo.implicitHeight + Theme.spacingSm * 2
                        radius: Theme.radiusSm
                        color: dbcHover.hovered ? Theme.panelElevated : Theme.panel
                        border.width: 1
                        border.color: Theme.border

                        HoverHandler { id: dbcHover }

                        ColumnLayout {
                            id: dbcInfo

                            x: Theme.spacingSm
                            y: Theme.spacingSm
                            width: parent.width - x - 26
                            spacing: 1

                            Label {
                                Layout.fillWidth: true
                                text: dbcRow.modelData.name
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeMd
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("%1 messages - %2 signals")
                                      .arg(dbcRow.modelData.messages)
                                      // "signals" is a Qt keyword macro, so the
                                      // key has to be reached by subscript.
                                      .arg(dbcRow.modelData["signals"])
                                color: Theme.textDisabled
                                font.family: Theme.monoFontFamily
                                font.pixelSize: Theme.fontSizeSm
                            }
                        }

                        Rectangle {
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: Theme.spacingXs
                            width: 18
                            height: 18
                            radius: 3
                            color: removeHover.hovered ? Theme.error : "transparent"

                            Label {
                                anchors.centerIn: parent
                                text: "✕"
                                color: removeHover.hovered ? Theme.background : Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSm
                            }

                            HoverHandler { id: removeHover }
                            TapHandler {
                                onSingleTapped: GraphDataModel.removeGraphDbc(dbcRow.index)
                            }

                            ToolTip.visible: removeHover.hovered
                            ToolTip.delay: 400
                            ToolTip.text: qsTr("Remove - traces from it are dropped")
                        }
                    }
                }

                ColumnLayout {
                    anchors.centerIn: parent
                    width: parent.width - Theme.spacingLg * 2
                    visible: GraphDataModel.databaseCount === 0
                    spacing: Theme.spacingXs

                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: "⛁"
                        color: Theme.textDisabled
                        font.pixelSize: 28
                    }
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("No databases loaded.\nThe scope has nothing to plot.")
                        color: Theme.textDisabled
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        wrapMode: Text.WordWrap
                    }
                }
            }

            // --------------------------------------------- virtual signals
            //
            // A fixed-height strip rather than a fillHeight one: the DBC list
            // above it is the panel's main content and must keep the space.
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.toolbarHeight
                color: Theme.panel

                Rectangle { width: parent.width; height: 1; color: Theme.border }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingMd
                    anchors.rightMargin: Theme.spacingSm
                    spacing: Theme.spacingXs

                    Label {
                        text: root.virtualsExpanded ? "▾" : "▸"
                        color: Theme.textDisabled
                        font.pixelSize: Theme.fontSizeSm
                    }
                    Label {
                        text: qsTr("VIRTUAL SIGNALS")
                        color: Theme.textDisabled
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        font.letterSpacing: 1.1
                    }
                    Label {
                        visible: MathChannelManager.count > 0
                        text: qsTr("(%1)").arg(MathChannelManager.count)
                        color: Theme.accent
                        font.family: Theme.monoFontFamily
                        font.pixelSize: Theme.fontSizeSm
                    }
                    Item { Layout.fillWidth: true }
                }

                TapHandler {
                    onSingleTapped: root.virtualsExpanded = !root.virtualsExpanded
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: virtualsBody.implicitHeight + Theme.spacingSm * 2
                Layout.maximumHeight: 260
                visible: root.virtualsExpanded
                color: Theme.surface
                clip: true

                ColumnLayout {
                    id: virtualsBody

                    x: Theme.spacingSm
                    y: Theme.spacingSm
                    width: parent.width - Theme.spacingSm * 2
                    spacing: Theme.spacingXs

                    ToolbarButton {
                        Layout.fillWidth: true
                        text: qsTr("Create Math Channel")
                        glyph: "+"
                        primary: true
                        onClicked: mathDialog.open()
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: MathChannelManager.count === 0
                        text: qsTr("A math channel is a formula over decoded signals. "
                                 + "Once created it appears in every pane's signal picker.")
                        color: Theme.textDisabled
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        wrapMode: Text.WordWrap
                    }

                    Repeater {
                        model: MathChannelManager.channels

                        delegate: Rectangle {
                            id: mathRow

                            required property int index
                            required property var modelData

                            Layout.fillWidth: true
                            Layout.preferredHeight: mathInfo.implicitHeight + Theme.spacingSm * 2
                            radius: Theme.radiusSm
                            color: mathHover.hovered ? Theme.panelElevated : Theme.panel
                            border.width: 1
                            border.color: mathRow.modelData.valid ? Theme.border : Theme.error

                            HoverHandler { id: mathHover }

                            // Left click plots on the pane the user last
                            // touched; right click asks which one. The pane
                            // list is read when the menu opens, not bound -
                            // a menu is a snapshot of a moment by nature.
                            TapHandler {
                                acceptedButtons: Qt.LeftButton
                                enabled: mathRow.modelData.valid
                                onSingleTapped: root.plotMathChannel(mathRow.modelData.id, "")
                            }
                            TapHandler {
                                acceptedButtons: Qt.RightButton
                                enabled: mathRow.modelData.valid
                                onSingleTapped: {
                                    paneMenu.channelId = mathRow.modelData.id
                                    paneMenu.channelName = mathRow.modelData.name
                                    paneMenu.paneList = root.currentWorkspace
                                                        ? root.currentWorkspace.paneIds() : []
                                    paneMenu.popup()
                                }
                            }

                            ToolTip.visible: mathHover.hovered && mathRow.modelData.valid
                            ToolTip.delay: 600
                            ToolTip.text: qsTr("Click to plot - right-click to choose a pane")

                            ColumnLayout {
                                id: mathInfo

                                x: Theme.spacingSm
                                y: Theme.spacingSm
                                width: parent.width - x - 26
                                spacing: 1

                                Label {
                                    Layout.fillWidth: true
                                    text: mathRow.modelData.unit.length > 0
                                          ? qsTr("%1  [%2]").arg(mathRow.modelData.name)
                                                            .arg(mathRow.modelData.unit)
                                          : mathRow.modelData.name
                                    color: Theme.textPrimary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeMd
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: mathRow.modelData.valid
                                          ? mathRow.modelData.expression
                                          : mathRow.modelData.error
                                    color: mathRow.modelData.valid ? Theme.textDisabled : Theme.error
                                    font.family: Theme.monoFontFamily
                                    font.pixelSize: Theme.fontSizeSm
                                    elide: Text.ElideRight
                                }
                            }

                            Rectangle {
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: Theme.spacingXs
                                width: 18
                                height: 18
                                radius: 3
                                color: mathRemoveHover.hovered ? Theme.error : "transparent"

                                Label {
                                    anchors.centerIn: parent
                                    text: "✕"
                                    color: mathRemoveHover.hovered ? Theme.background
                                                                   : Theme.textSecondary
                                    font.pixelSize: Theme.fontSizeSm
                                }

                                HoverHandler { id: mathRemoveHover }
                                TapHandler {
                                    onSingleTapped: MathChannelManager.removeChannel(mathRow.modelData.id)
                                }

                                ToolTip.visible: mathRemoveHover.hovered
                                ToolTip.delay: 400
                                ToolTip.text: qsTr("Delete - traces of it are dropped from every pane")
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.statusBarHeight
                color: Theme.panel

                Rectangle { width: parent.width; height: 1; color: Theme.border }

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    x: Theme.spacingSm
                    width: parent.width - Theme.spacingSm * 2
                    text: qsTr("%1 signal(s) available").arg(GraphDataModel.availableSignalCount)
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    elide: Text.ElideRight
                }
            }
        }
    }

}
