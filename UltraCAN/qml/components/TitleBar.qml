import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls.Basic

import UltraCAN.Theme

/*!
    \qmltype TitleBar
    \brief Custom caption bar for a frameless ApplicationWindow.

    \section2 Why the OS does the moving

    A naive frameless title bar moves the window from QML: it stores the press
    position and writes \c {window.x/y} on every mouse move. That is wrong in
    three separate ways and all three are visible to the user:

    \list
    \li \b Lag. Every step is QML -> Qt -> compositor, one whole frame behind
        the cursor. The window visibly trails the mouse.
    \li \b {No Aero Snap}. Windows only offers snap zones, snap assist and the
        Win+arrow layouts to a window that is being moved through the SHELL's
        move loop. A window whose position is being assigned is, as far as the
        shell is concerned, not being dragged at all.
    \li \b {No cross-desktop behaviour}. Edge snapping, multi-monitor DPI
        transitions and touch drag are all implemented by the platform.
    \endlist

    So this bar never sets a coordinate. It calls QWindow::startSystemMove(),
    which hands the still-pressed mouse to the platform's own move loop - the
    same one the native caption uses. From that instant Windows owns the drag
    and every shell feature comes back for free. QWindow::startSystemResize()
    does the same job for the eight resize strips in Main.qml.

    Both are \c {public slots} on QWindow, so they are callable straight from
    QML with no C++ shim (verified against Qt 6.8.3's qwindow.h).

    \section2 Drag versus double-click

    These two want the same press, so the split is by gesture, not by area:
    \l DragHandler only activates once the pointer passes the drag threshold,
    while \l TapHandler with \c DragThreshold policy withdraws as soon as it
    does. A click stays a click, a double-click still maximises, and a drag
    reaches the OS with the button still down (which startSystemMove requires).

    The handlers live on \l dragSurface, which stops short of the caption
    buttons, so a press on Close can never be reinterpreted as a window drag.
*/
Rectangle {
    id: root

    // ---------------------------------------------------------------- api
    /*! The window this bar drives. Defaults to the one it is displayed in, so
        the common case needs no wiring at all. */
    property Window win: Window.window

    /*! Caption text. Bind it to the window title to keep the bar, the taskbar
        button and Alt+Tab telling the same story. */
    property string title: ""

    /*! Where the caption icons live. Defaulted rather than hardcoded at each
        use site: UltraCAN.Components must not depend on the application module,
        so the app can override this if the assets ever move. */
    property url iconRoot: "qrc:/qt/qml/UltraCAN/assets/icons/"

    /*! Set false for a tool window that may not be resized. Also disables
        double-click-to-maximise, so the two cannot disagree. */
    property bool maximizeEnabled: true

    /*! Caption buttons a DIALOG has no use for. Hidden rather than disabled:
        a greyed-out Minimise on a modal dialog is noise, not information. */
    property bool minimizeVisible: true
    property bool maximizeVisible: true

    /*!
        Radius for the bar's TOP two corners, and for the Close button's own
        top-right corner behind it.

        A rounded frameless window cannot get its top corners from the
        surface underneath: Qt Quick's \c clip is a bounding-box clip, so an
        opaque bar anchored across the top paints straight over the curve. So
        the bar rounds itself, and the window passes its own radius down.

        Defaults to 0, which is the square bar every existing caller already
        gets - the frameless setup dialog included.
    */
    property int topRadius: 0

    // ------------------------------------------------------------ geometry
    implicitHeight: Theme.titleBarHeight
    color: Theme.panel
    // Per-corner radii (Qt 6.7+), not `radius`: the BOTTOM corners sit
    // against the content and must stay square.
    topLeftRadius: root.topRadius
    topRightRadius: root.topRadius

    // The window is frameless, so this line is the only thing separating the
    // caption from the content.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.border
    }

    // ------------------------------------------------------------ behaviour
    readonly property bool maximized: root.win !== null
                                      && root.win.visibility === Window.Maximized

    /*! Restore-then-move, the way the native caption does it.

        Dragging a maximised window has to un-maximise it first, and if we only
        called showNormal() the window would snap back to wherever it last was
        - possibly a different monitor - while the cursor stayed put. Windows
        instead keeps the grab point at the same RELATIVE position across the
        restored width, so the window appears to shrink around the cursor. */
    function beginSystemMove() {
        if (!root.win)
            return

        if (root.win.visibility === Window.Maximized) {
            const grabX = moveHandler.centroid.scenePosition.x
            const ratio = root.win.width > 0 ? grabX / root.win.width : 0.5
            const globalX = root.win.x + grabX

            root.win.showNormal()
            root.win.x = Math.round(globalX - ratio * root.win.width)
        }

        root.win.startSystemMove()
    }

    function toggleMaximized() {
        if (!root.win || !root.maximizeEnabled)
            return
        root.win.visibility = (root.win.visibility === Window.Maximized)
                            ? Window.Windowed
                            : Window.Maximized
    }

    // --------------------------------------------------------------- pieces
    /*!
        One caption button.

        Declared at document top level because an inline component may not be
        nested inside another object, and deliberately self-contained: it reads
        Theme (a singleton, always in scope) and never an id from the enclosing
        document, which an inline component cannot rely on resolving.
    */
    component CaptionButton: Rectangle {
        id: btn

        property alias source: glyph.source
        property color hoverColor: Theme.panelElevated
        property color pressedColor: Theme.border
        property string tooltipText: ""

        signal activated()

        width: Theme.windowButtonWidth
        height: Theme.titleBarHeight

        color: area.containsPress ? btn.pressedColor
             : area.containsMouse ? btn.hoverColor
                                  : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

        Image {
            id: glyph
            anchors.centerIn: parent
            width: 16
            height: 16
            // The assets are 32 px; decoding at native size and letting the
            // scene graph downscale with mipmaps keeps the 1 px strokes from
            // shimmering on fractional-DPI displays.
            sourceSize.width: 32
            sourceSize.height: 32
            fillMode: Image.PreserveAspectFit
            smooth: true
            mipmap: true
            // The glyphs are white PNGs, so weight is expressed as opacity
            // rather than colour - no tinting effect, no extra module.
            opacity: area.containsMouse ? 1.0 : 0.72
            Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
        }

        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            onClicked: btn.activated()

            // Taking the exclusive grab on press is also what stops the bar's
            // DragHandler from turning a caption click into a window move.
            ToolTip.visible: containsMouse && btn.tooltipText.length > 0
            ToolTip.text: btn.tooltipText
            ToolTip.delay: 600
        }
    }

    // ---------------------------------------------------- draggable surface
    Item {
        id: dragSurface

        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: captionButtons.left

        DragHandler {
            id: moveHandler
            // Nothing in the scene moves; the OS moves the window.
            target: null
            // Never steal a press that a child (a caption button, a menu) has
            // already claimed.
            grabPermissions: DragHandler.TakeOverForbidden
            onActiveChanged: if (active) root.beginSystemMove()
        }

        TapHandler {
            acceptedButtons: Qt.LeftButton
            // Withdraws once the drag threshold is passed, so this handler and
            // the DragHandler above can share one press without fighting.
            gesturePolicy: TapHandler.DragThreshold
            onDoubleTapped: root.toggleMaximized()
        }

        // A RowLayout rather than a Row: Text.elide only does anything when
        // something has given the Text a width, and a Row never does - it
        // sizes to the child's implicit width, so a long caption would slide
        // under the buttons instead of truncating.
        RowLayout {
            anchors.left: parent.left
            anchors.leftMargin: Theme.spacingMd
            anchors.right: parent.right
            anchors.rightMargin: Theme.spacingMd
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spacingSm

            Image {
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
                Layout.alignment: Qt.AlignVCenter
                source: root.iconRoot + "ultracan_logo.png"
                /*!
                    Decoded at 48 and drawn at 24.

                    The source is 256 px; decoding it at native size to paint a
                    24 px square wastes memory, and decoding it AT 24 leaves
                    nothing for a 150%/200% display to work with - the star's
                    thin points are the first thing to go. 48 covers up to 2x
                    exactly, and mipmap handles the fractional scales between.
                */
                sourceSize.width: 48
                sourceSize.height: 48
                // The artwork is square, but PreserveAspectFit means a future
                // wordmark dropped in here cannot come out stretched.
                fillMode: Image.PreserveAspectFit
                smooth: true
                mipmap: true
            }

            Text {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                text: root.title
                elide: Text.ElideRight
                // Follow the OS convention of dimming an inactive caption, so
                // the focused window is identifiable at a glance.
                color: (root.win && root.win.active) ? Theme.textPrimary
                                                     : Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMd
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
            }
        }
    }

    // ----------------------------------------------------- caption buttons
    Row {
        id: captionButtons

        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        spacing: 0

        CaptionButton {
            source: root.iconRoot + "minimize-sign.png"
            visible: root.minimizeVisible
            tooltipText: qsTr("Minimize")
            onActivated: if (root.win) root.win.showMinimized()
        }

        CaptionButton {
            source: root.iconRoot + "maximize.png"
            visible: root.maximizeVisible
            enabled: root.maximizeEnabled
            opacity: enabled ? 1.0 : 0.4
            tooltipText: root.maximized ? qsTr("Restore Down") : qsTr("Maximize")
            onActivated: root.toggleMaximized()
        }

        CaptionButton {
            source: root.iconRoot + "close.png"
            hoverColor: Theme.closeHover
            pressedColor: Theme.closeHoverPressed
            tooltipText: qsTr("Close")
            onActivated: if (root.win) root.win.close()
            // Rightmost button, so its hover fill IS the window's top-right
            // corner. Without this the corner is correctly round until the
            // pointer touches Close and then squares off in red - the same
            // bounding-box-clip problem one level further down.
            topRightRadius: root.topRadius
        }
    }
}
