import QtQuick
import QtQuick.Window

import UltraCAN.Theme

/*!
    \qmltype WindowResizeBorders
    \brief Eight invisible strips hugging the inside of a frameless window's
           edge, each handing its drag to the platform's own resize loop.

    Drop one into any frameless Window:

    \code
    Window {
        flags: Qt.Window | Qt.FramelessWindowHint
        WindowResizeBorders {}          // that is the whole integration
    }
    \endcode

    \section2 Why the OS does the resizing

    Resizing by assigning \c width / \c height from QML puts a full layout pass
    between the cursor and the frame on every mouse move, which the user sees
    as the window rubber-banding behind the pointer. QWindow::startSystemResize()
    hands the still-pressed mouse to the compositor instead - the same loop the
    native frame uses - so the resize is as smooth as any other window's and the
    edge/corner snap behaviour comes with it.

    It must be called while the button is still DOWN: it adopts the live press
    and cannot start a drag of its own. Hence \c onPressed, not \c onClicked.

    \section2 One arithmetic rule for all eight

    Every geometry falls out of the same four edge bits (Qt.TopEdge = 1,
    LeftEdge = 2, RightEdge = 4, BottomEdge = 8): a named edge pins the strip to
    that side at \l thickness, an unnamed one stretches it between the corners.
    A corner simply names two edges - which is also exactly what
    startSystemResize() wants as its argument.

    Extracted from Main.qml when the setup dialog went frameless: converting a
    window to frameless without this silently removes edge resizing, which is a
    regression the user only discovers when they try to make the window bigger.
*/
Item {
    id: root

    /*! The window to resize. Defaults to the one this item is shown in. */
    property Window win: Window.window

    /*! Strip thickness. Below ~5 px it is hard to hit; above ~8 px it starts
        eating clicks meant for the content underneath. */
    property int thickness: Theme.resizeBorder

    /*! A maximised or full-screen window has no edges to drag, and live strips
        would let the user resize out of that state and desynchronise whatever
        caption button reflects it. */
    readonly property bool resizable: root.win !== null
                                      && root.win.visibility === Window.Windowed

    anchors.fill: parent
    // Above the content, or the page underneath swallows the press.
    z: 100

    Repeater {
        model: [
            { edges: Qt.TopEdge    | Qt.LeftEdge,  cursor: Qt.SizeFDiagCursor },
            { edges: Qt.TopEdge,                   cursor: Qt.SizeVerCursor   },
            { edges: Qt.TopEdge    | Qt.RightEdge, cursor: Qt.SizeBDiagCursor },
            { edges: Qt.RightEdge,                 cursor: Qt.SizeHorCursor   },
            { edges: Qt.BottomEdge | Qt.RightEdge, cursor: Qt.SizeFDiagCursor },
            { edges: Qt.BottomEdge,                cursor: Qt.SizeVerCursor   },
            { edges: Qt.BottomEdge | Qt.LeftEdge,  cursor: Qt.SizeBDiagCursor },
            { edges: Qt.LeftEdge,                  cursor: Qt.SizeHorCursor   }
        ]

        delegate: MouseArea {
            id: grip

            required property var modelData

            readonly property int edges: modelData.edges
            readonly property bool onLeft:   (edges & Qt.LeftEdge)   !== 0
            readonly property bool onRight:  (edges & Qt.RightEdge)  !== 0
            readonly property bool onTop:    (edges & Qt.TopEdge)    !== 0
            readonly property bool onBottom: (edges & Qt.BottomEdge) !== 0

            x: onLeft ? 0 : (onRight ? root.width - root.thickness : root.thickness)
            y: onTop  ? 0 : (onBottom ? root.height - root.thickness : root.thickness)
            width:  (onLeft || onRight) ? root.thickness
                                        : Math.max(0, root.width  - 2 * root.thickness)
            height: (onTop || onBottom) ? root.thickness
                                        : Math.max(0, root.height - 2 * root.thickness)

            enabled: root.resizable
            visible: enabled
            hoverEnabled: true
            cursorShape: modelData.cursor
            acceptedButtons: Qt.LeftButton

            onPressed: if (root.win) root.win.startSystemResize(grip.edges)
        }
    }
}
