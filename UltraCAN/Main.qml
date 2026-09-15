import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Window
import QtQuick.Dialogs
import QtQuick.Effects

import UltraCAN.Theme
import UltraCAN.Routing
import UltraCAN.Components
import UltraCAN.Core

/*!
    Application shell.

    Responsibilities and nothing more:
      - own the window chrome and the global layout (sidebar | content | status bar)
      - bind the Sidebar to the Navigation singleton
      - keep every page alive and show one of them

    All module behaviour lives inside the views themselves.
*/
ApplicationWindow {
    id: window

    width: 1440
    height: 900
    minimumWidth: 960
    minimumHeight: 600
    visible: true
    title: qsTr("UltraCAN - %1").arg(Navigation.currentRoute.title)

    /*! Outcome of the last session export/import, shown in the status bar and
        cleared on a timer - a message that never goes away stops being read. */
    property string sessionMessage: ""
    onSessionMessageChanged: if (window.sessionMessage.length > 0) sessionMessageTimer.restart()
    Timer {
        id: sessionMessageTimer
        interval: 6000
        onTriggered: window.sessionMessage = ""
    }
    /*!
        Transparent, not Theme.background. The window itself is now just a
        borderless, invisible rectangle; every pixel the user actually sees -
        the rounded panel, its shadow - is painted by the two Rectangles
        below. Painting the window opaque here would sit BEHIND the shadow
        and show through the margin as a solid slab instead of the intended
        gap of empty (desktop-showing-through) space.
    */
    color: "transparent"

    /*!
        Does this window currently look like a floating panel?

        Only a Windowed window gets the shadow margin and the rounded
        corners. Maximised (and full-screen) must sit flush: the margin would
        otherwise show as a dead gap all around the desktop, and a rounded
        corner drawn over a screen edge reads as a rendering fault rather
        than a style. Deliberately the same `visibility === Window.Windowed`
        test WindowResizeBorders uses, so "has edges to drag" and "looks like
        it has edges" can never disagree.
    */
    readonly property bool floatingChrome: window.visibility === Window.Windowed
    /*! Every geometry below derives from these two, so the windowed and
        maximised cases can never be styled half-and-half. */
    readonly property int surfaceMargin: window.floatingChrome ? Theme.windowShadowMargin : 0
    readonly property int surfaceRadius: window.floatingChrome ? Theme.windowRadius : 0

    /*!
        Frameless, but NOT featureless.

        FramelessWindowHint alone removes the native caption AND the window
        styles that hang off it, which on Windows quietly costs the taskbar
        minimise/restore animation, Aero Snap and the Alt+Space system menu -
        the frame is gone, so WS_MINIMIZEBOX / WS_MAXIMIZEBOX / WS_SYSMENU go
        with it. Asking for the button hints explicitly puts those styles back
        without bringing the caption back: Qt maps each hint to its Win32
        style, and the shell then treats this like any other resizable window.

        The drawing is ours; the window management stays the platform's.
    */
    flags: Qt.Window
         | Qt.FramelessWindowHint
         | Qt.WindowMinimizeButtonHint
         | Qt.WindowMaximizeButtonHint
         | Qt.WindowSystemMenuHint

    // Frameless windows lose edge resizing with the frame; this hands it back
    // to the OS. Extracted to a component so the setup dialog gets the exact
    // same behaviour rather than a second copy that drifts. See its own doc.
    WindowResizeBorders {}

    // Global keyboard routing: Ctrl+1..6 jump straight to a module.
    Repeater {
        model: Navigation.routes
        Item {
            required property int index
            Shortcut {
                sequence: "Ctrl+" + (index + 1)
                onActivated: Navigation.navigateToIndex(index)
            }
        }
    }

    // =========================================================================
    //  Rounded surface + drop shadow
    // =========================================================================
    //
    // The window is frameless AND transparent (see `color` above), so nothing
    // is drawn at the true window edges at all; WindowResizeBorders above
    // still spans them (it is a SIBLING of both Rectangles below, not a
    // child), which is exactly why edge-resizing still works right up to
    // where a normal window's frame would be, through what looks like empty
    // air. Between that true edge and the visible content sits
    // Theme.windowShadowMargin of transparent space - room for the shadow's
    // blur to fall off in before it would otherwise be clipped by the window
    // bounds.
    //
    // Deliberately TWO Rectangles, not one Rectangle with `layer.effect:
    // MultiEffect{shadowEnabled:true}` on it directly - that is the pattern
    // every MultiEffect tutorial shows, and it is the wrong call for this
    // app specifically. `layer.enabled` reroutes EVERY repaint of everything
    // inside the layered item through an offscreen texture that is
    // re-rasterised whenever that subtree is dirty. Put it on the item that
    // holds the live CAN trace, the Graph Plot's charts and the scrolling
    // Logger grid, and the shadow's presence would force the whole UI
    // through an extra GPU texture pass on every one of those updates - a
    // real frame-time cost in an app whose entire job is rendering a live
    // bus at a steady rate, not a theoretical one.
    //
    // So the shadow lives on its OWN Rectangle (`shadowSource`) that has no
    // children and nothing dynamic in it: its texture is dirty only when the
    // window resizes or moves, never on a frame of traffic. `surface` -
    // which holds the actual UI - sits on top of it, exactly the same shape
    // and position, painted with the normal (fast) scene graph path and no
    // layer/effect of its own. Being opaque, `surface` fully covers
    // `shadowSource`'s own body; only the blurred halo that extends past
    // `shadowSource`'s edges peeks out around it, which is the shadow.
    Rectangle {
        id: shadowSource
        anchors.fill: parent
        anchors.margins: window.surfaceMargin
        radius: window.surfaceRadius
        color: Theme.background

        // A maximised window has no margin for a shadow to fall into, so this
        // is not merely hidden - `visible: false` takes the whole item out of
        // the scene graph, which means the layer's offscreen texture and the
        // MultiEffect pass stop being rendered at all rather than being
        // computed and then thrown away behind `surface`.
        visible: window.floatingChrome

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Theme.windowShadow
            shadowBlur: 0.6
            shadowHorizontalOffset: 0
            shadowVerticalOffset: 4
        }
    }

    Rectangle {
        id: surface
        anchors.fill: parent
        anchors.margins: window.surfaceMargin
        radius: window.surfaceRadius
        color: Theme.background
        /*!
            Straight-edge guard only - this does NOT round anything.

            Qt Quick's clip is a bounding-BOX clip (scissor rect / stencil of
            `x,y,width,height`); the radius plays no part in it. An opaque
            child anchored into a corner therefore paints straight over the
            curve, which is exactly why the first version of this window came
            out square-cornered despite the radius being set here.

            Rounding is done instead by the two children that actually own the
            four corners - the TitleBar at the top and the status bar at the
            bottom - each rounding its own outer corners to `surfaceRadius`
            with Qt 6.7+ per-corner radii. That keeps the corner treatment on
            two small, static items instead of putting `layer.enabled` on the
            whole content tree, which would push every repaint of the live
            trace, the charts and the Logger through an offscreen texture and
            a mask shader on every frame of bus traffic.
        */
        clip: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ------------------------------------------------------- title bar
            TitleBar {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.titleBarHeight
                // One source of truth for the caption: the bar, the taskbar button
                // and Alt+Tab all read window.title.
                title: window.title
                // This bar owns the window's top two corners, so it is what
                // has to round them - see the note on `surface.clip` above.
                topRadius: window.surfaceRadius
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // -------------------------------------------------- navigation
                Sidebar {
                    id: sidebar
                    Layout.fillHeight: true
                    // Auto-collapse on narrow windows; the user can still toggle manually.
                    collapsed: window.width < 1100
                    model: Navigation.routes
                    currentIndex: Navigation.currentIndex
                    onRouteSelected: (index) => Navigation.navigateToIndex(index)
                    onExportRequested: configExportDialog.open()
                    onImportRequested: configImportDialog.open()
                }

                // ----------------------------------------------- content area
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: Theme.surface
                    clip: true

                    // ------------------------------------------------- the pages
                    //
                    // This was one Loader with `sourceComponent:
                    // Navigation.currentRoute.page`, and that single binding was
                    // the state-loss bug: assigning a Loader a new
                    // sourceComponent DESTROYS the item it is holding. Leaving
                    // Graph Plot therefore deleted every ChartView, every series
                    // and the whole pane tree, and coming back built a fresh page
                    // that had never heard of them. Nothing was "reset" - the page
                    // was a different object each time.
                    //
                    // A StackLayout instead: one child per route, all of them
                    // alive, only `visible` changing as the user navigates. The
                    // Loader per child survives because its `active` LATCHES - a
                    // route is instantiated the first time it is visited and never
                    // torn down, so start-up still costs one page rather than five.
                    //
                    // Verified: a Repeater inside a StackLayout is not counted as
                    // one of its pages (StackLayout.count == the delegate count,
                    // and StackLayout.index is the route index), so currentIndex
                    // lines up with Navigation.currentIndex with no offset.
                    StackLayout {
                        id: pageStack
                        anchors.fill: parent
                        currentIndex: Navigation.currentIndex

                        Repeater {
                            model: Navigation.routes

                            delegate: Loader {
                                id: pageHost

                                required property int index
                                required property var modelData

                                readonly property bool isCurrent: pageHost.index === Navigation.currentIndex

                                /*! Latches true on first visit and is never
                                    cleared - that one-way door is the whole fix.
                                    A plain `active: isCurrent` would deactivate on
                                    the way out, which destroys the item exactly
                                    like the old Loader did.

                                    Routes marked eager start out latched: they hold
                                    state the rest of the app queries whether or not
                                    the user has been there. See NavRoute::eager. */
                                property bool visited: false
                                onIsCurrentChanged: if (pageHost.isCurrent) pageHost.visited = true
                                Component.onCompleted: {
                                    if (pageHost.isCurrent || pageHost.modelData.eager)
                                        pageHost.visited = true
                                }

                                active: pageHost.visited
                                asynchronous: true
                                sourceComponent: pageHost.modelData.page

                                // First paint of a page still fades in. There is no
                                // cross-page fade any more: with both pages alive,
                                // StackLayout swaps `visible` in one frame and
                                // there is nothing left to hide.
                                opacity: pageHost.status === Loader.Ready ? 1 : 0
                                Behavior on opacity { NumberAnimation { duration: Theme.durationNormal } }
                            }
                        }
                    }

                    BusyIndicator {
                        anchors.centerIn: parent
                        // Only ever seen on a route's FIRST visit; afterwards the
                        // page is already built and the switch is instant.
                        running: {
                            const item = pageStack.currentIndex >= 0
                                      && pageStack.currentIndex < pageStack.count
                                       ? pageStack.itemAt(pageStack.currentIndex) : null
                            return item !== null && item.status === Loader.Loading
                        }
                        visible: running
                    }
                }
            }

            // ------------------------------------------------- session files
            //
            // The shell owns these, not the Sidebar: Sidebar is model-agnostic by
            // design and never imports UltraCAN.Core.
            //
            // Note what is NOT here - any knowledge of the graph workspace. The
            // Graph Plot page answers ConfigManager's graphStateRequested /
            // graphStateRestored signals itself, which is why that page is marked
            // eager in Navigation: a page that was never opened cannot answer, and
            // the export would silently save no workspace at all.
            FileDialog {
                id: configExportDialog
                title: qsTr("Save session")
                fileMode: FileDialog.SaveFile
                defaultSuffix: "ucan"
                nameFilters: [ qsTr("UltraCAN session (*.ucan)"), qsTr("JSON (*.json)"),
                               qsTr("All files (*)") ]
                onAccepted: {
                    ConfigManager.exportConfig(selectedFile)
                    window.sessionMessage = ConfigManager.lastMessage
                }
            }

            FileDialog {
                id: configImportDialog
                title: qsTr("Open session")
                fileMode: FileDialog.OpenFile
                nameFilters: [ qsTr("UltraCAN session (*.ucan *.json)"), qsTr("All files (*)") ]
                onAccepted: {
                    ConfigManager.importConfig(selectedFile)
                    window.sessionMessage = ConfigManager.lastMessage
                }
            }

            // ------------------------------------------------------ status bar
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.statusBarHeight
                color: Theme.panel
                // Bottom two corners of the window belong to this bar, the
                // same way the top two belong to the TitleBar.
                bottomLeftRadius: window.surfaceRadius
                bottomRightRadius: window.surfaceRadius

                Rectangle { width: parent.width; height: 1; color: Theme.border }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingMd
                    anchors.rightMargin: Theme.spacingMd
                    spacing: Theme.spacingLg

                    // -- bus state: click to open the connection panel
                    Rectangle {
                        id: busIndicator

                        Layout.preferredHeight: Theme.statusBarHeight - 6
                        Layout.preferredWidth: busRow.implicitWidth + Theme.spacingSm
                        radius: Theme.radiusSm
                        color: busMouse.containsMouse ? Theme.panelElevated : "transparent"

                        RowLayout {
                            id: busRow
                            anchors.centerIn: parent
                            spacing: Theme.spacingSm

                            Rectangle {
                                width: 8; height: 8; radius: 4
                                color: {
                                    switch (CanBusManager.connectionState) {
                                    case CanBusManager.Connected:  return Theme.success
                                    case CanBusManager.Connecting: return Theme.warning
                                    case CanBusManager.Closing:    return Theme.warning
                                    default: return CanBusManager.simulating ? Theme.accentAlt
                                                                             : Theme.textDisabled
                                    }
                                }
                            }
                            Label {
                                text: qsTr("Bus: %1").arg(CanBusManager.busStatusText)
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeSm
                            }
                            Label {
                                text: "▴"
                                color: Theme.textDisabled
                                font.pixelSize: Theme.fontSizeSm
                            }
                        }

                        MouseArea {
                            id: busMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: busPanel.opened ? busPanel.close() : busPanel.open()
                        }

                        // -- connection panel -------------------------------------
                        Popup {
                            id: busPanel

                            y: -height - Theme.spacingSm
                            width: 340
                            padding: Theme.spacingMd
                            modal: false
                            focus: true
                            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

                            background: Rectangle {
                                color: Theme.panel
                                radius: Theme.radiusMd
                                border.width: 1
                                border.color: Theme.border
                            }

                            /*! True once the remembered selection has been applied.
                                A later plugin change must not drag the previous
                                plugin's interface name along with it. */
                            property bool restored: false

                            // Enumerating plugins/interfaces hits the Qt plugin
                            // loader, so it is done when the panel opens, not in a
                            // binding that would re-run on every repaint.
                            function refresh() {
                                const plugins = CanBusManager.availablePlugins()
                                pluginBox.model = plugins
                                if (pluginBox.currentIndex < 0 && plugins.length > 0) {
                                    // Order of preference: what worked last time,
                                    // then what Qt suggests, then whatever is first.
                                    const remembered = plugins.indexOf(SettingsManager.canPlugin)
                                    const preferred = plugins.indexOf(CanBusManager.preferredPlugin())
                                    pluginBox.currentIndex = remembered >= 0 ? remembered
                                                           : (preferred >= 0 ? preferred : 0)
                                }
                                refreshInterfaces()

                                // A bitrate is a plain number rather than an
                                // enumeration of what is plugged in, so it restores
                                // unconditionally.
                                if (!busPanel.restored) {
                                    const rateIndex = CanBusManager.standardBitrates()
                                                      .indexOf(SettingsManager.bitrate)
                                    if (rateIndex >= 0)
                                        bitrateBox.currentIndex = rateIndex
                                }
                                busPanel.restored = true
                            }

                            function refreshInterfaces() {
                                const interfaces = CanBusManager.availableInterfaces(pluginBox.currentText)
                                interfaceBox.model = interfaces
                                if (interfaceBox.count === 0)
                                    return
                                if (!busPanel.restored) {
                                    const remembered = interfaces.indexOf(SettingsManager.canInterface)
                                    interfaceBox.currentIndex = remembered >= 0 ? remembered : 0
                                } else if (interfaceBox.currentIndex < 0) {
                                    interfaceBox.currentIndex = 0
                                }
                            }

                            onOpened: refresh()

                            Connections {
                                target: CanBusManager
                                // The C++ side re-queries QCanBus on every call
                                // anyway; this just gives the explicit Refresh
                                // button below something to fire so a newly
                                // plugged-in analyzer shows up without closing
                                // and reopening the panel.
                                function onInterfacesRefreshed() { busPanel.refresh() }
                            }

                            ColumnLayout {
                                width: parent.width
                                spacing: Theme.spacingSm

                                Label {
                                    text: qsTr("CAN INTERFACE")
                                    color: Theme.textDisabled
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeSm
                                    font.letterSpacing: 1.1
                                }

                                PropertyField {
                                    label: qsTr("Plugin")
                                    labelWidth: 70
                                    horizontalMargin: 0
                                    ThemedComboBox {
                                        id: pluginBox
                                        Layout.fillWidth: true
                                        enabled: !CanBusManager.connected
                                        onActivated: busPanel.refreshInterfaces()
                                    }
                                }

                                PropertyField {
                                    label: qsTr("Interface")
                                    labelWidth: 70
                                    horizontalMargin: 0
                                    // Two editors in the slot, same pattern the
                                    // "Range" example in PropertyField's own doc
                                    // comment uses - no extra wrapping RowLayout
                                    // needed, editorRow already lays out multiple
                                    // children side by side.
                                    ThemedComboBox {
                                        id: interfaceBox
                                        Layout.fillWidth: true
                                        enabled: !CanBusManager.connected
                                    }
                                    ToolbarButton {
                                        // Icon-only: glyph with no text, so it
                                        // stays compact next to the combo box
                                        // rather than competing with it for width.
                                        // Left at its natural implicit size (no
                                        // forced square) so the Control machinery
                                        // centres the glyph correctly - forcing
                                        // implicitWidth would leave the internal
                                        // RowLayout narrower than the button and
                                        // the glyph left-hugged.
                                        glyph: "⟳"
                                        leftPadding: Theme.spacingSm
                                        rightPadding: Theme.spacingSm
                                        enabled: !CanBusManager.connected
                                        onClicked: CanBusManager.refreshInterfaces()

                                        ToolTip.visible: hovered
                                        ToolTip.delay: 400
                                        ToolTip.text: qsTr("Rescan for connected CAN interfaces")
                                    }
                                }

                                PropertyField {
                                    label: qsTr("Bitrate")
                                    labelWidth: 70
                                    horizontalMargin: 0
                                    suffix: qsTr("bit/s")
                                    ThemedComboBox {
                                        id: bitrateBox
                                        Layout.fillWidth: true
                                        enabled: !CanBusManager.connected
                                        model: CanBusManager.standardBitrates()
                                        // Fallback only; busPanel.refresh() replaces
                                        // this with the remembered rate on first open.
                                        currentIndex: 2          // 500 kbit/s
                                    }
                                }

                                PropertyField {
                                    label: qsTr("Simulator")
                                    labelWidth: 70
                                    horizontalMargin: 0
                                    ThemedSwitch {
                                        enabled: !CanBusManager.connected
                                        checked: CanBusManager.simulating
                                        text: checked ? qsTr("Generating traffic") : qsTr("Off")
                                        onToggled: {
                                            const wanted = checked
                                            checked = Qt.binding(function() { return CanBusManager.simulating })
                                            CanBusManager.simulating = wanted
                                        }
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Remembered once the bus actually connects.")
                                    color: Theme.textDisabled
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeSm
                                    visible: !CanBusManager.connected
                                    ToolTip.visible: hover.hovered
                                    ToolTip.text: SettingsManager.configFilePath
                                    HoverHandler { id: hover }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    Layout.maximumWidth: busPanel.availableWidth
                                    visible: CanBusManager.lastError.length > 0
                                    text: CanBusManager.lastError
                                    color: Theme.error
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeSm
                                    // WrapAnywhere, not WordWrap: driver messages can be
                                    // one unbroken run of characters, which WordWrap
                                    // cannot break - it just overflows the popup.
                                    wrapMode: Text.WrapAnywhere
                                    maximumLineCount: 3
                                    elide: Text.ElideRight
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.topMargin: Theme.spacingXs
                                    spacing: Theme.spacingSm

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("%1 error frames").arg(CanBusManager.errorFrameCount)
                                        visible: CanBusManager.errorFrameCount > 0
                                        color: Theme.warning
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontSizeSm
                                    }
                                    Item { Layout.fillWidth: true; visible: CanBusManager.errorFrameCount === 0 }

                                    ToolbarButton {
                                        text: qsTr("Disconnect")
                                        destructive: true
                                        visible: CanBusManager.connected
                                                 || CanBusManager.connectionState === CanBusManager.Connecting
                                        onClicked: CanBusManager.disconnectBus()
                                    }
                                    ToolbarButton {
                                        text: qsTr("Connect")
                                        primary: true
                                        visible: !CanBusManager.connected
                                                 && CanBusManager.connectionState !== CanBusManager.Connecting
                                        enabled: interfaceBox.count > 0
                                        onClicked: CanBusManager.connectBus(pluginBox.currentText,
                                                                            interfaceBox.currentText,
                                                                            Number(bitrateBox.currentText))
                                    }
                                }
                            }
                        }
                    }

                    Label {
                        text: window.sessionMessage
                        visible: window.sessionMessage.length > 0
                        color: ConfigManager.lastError.length > 0 ? Theme.error : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        elide: Text.ElideRight
                        Layout.maximumWidth: 420
                    }

                    Item { Layout.fillWidth: true }

                    Label {
                        text: Navigation.currentRoute.title
                        color: Theme.textDisabled
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                    }
                }
            }
        }
    }
}
