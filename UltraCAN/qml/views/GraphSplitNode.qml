import QtQuick

import UltraCAN.Theme

/*!
    \qmltype GraphSplitNode
    \brief One interior node of the scope's layout tree: two slots and a
           draggable divider between them.

    \section1 Why not a SplitView

    SplitView is the obvious type and it is the wrong tool for a tree that is
    built at RUNTIME. Two of its behaviours would have to be taken on trust:
    how it distributes space between two children that both fill, and whether
    its attached properties (\c {SplitView.fillWidth} and friends) can be
    assigned from JavaScript to an item that was reparented in after the fact.
    Neither is something this file can check, and both decide whether a split
    lands at 50/50 or somewhere arbitrary.

    A node only ever holds exactly two things. At that size the whole of
    SplitView reduces to one number - \l ratio - and the geometry becomes
    arithmetic that can be verified by reading it. Nothing is inherited, and a
    50/50 split is 50/50 by construction rather than by policy.

    \section1 It does not know what it contains

    The slots are bare Items. GraphPaneArea reparents panes - or further split
    nodes - into them, which is what lets a pane be MOVED into a deeper level
    of the tree instead of being destroyed and rebuilt. That distinction is the
    whole reason the tree is shaped this way: rebuilding a pane would take its
    ChartView, its series and every point already plotted with it.
*/
Item {
    id: root

    /*! false = side by side (a | b); true = stacked (a above b). */
    property bool vertical: false

    /*! Fraction of the long axis given to slot A. Clamped so a slot can always
        be grabbed again after being dragged to the edge. */
    property real ratio: 0.5

    /*! Thickness of the divider. Also the gap between the slots, so a handle
        never overlaps content. */
    readonly property int handleThickness: 6

    /*! The two children. GraphPaneArea parents into these. */
    readonly property alias slotA: slotAItem
    readonly property alias slotB: slotBItem

    /*!
        Emitted after a DRAG, so the host can persist the new ratio into the
        layout tree - the node itself holds no authority over the layout.

        Deliberately not named after the property: declaring
        \c {property real ratio} already generates its own change signal, and a
        second declaration of that name is a duplicate override that fails at
        load. The distinction earns its keep anyway - the generated signal also
        fires when the host ASSIGNS a ratio while mounting the tree, which would
        make the host write its own value straight back to itself, and it
        carries no arguments, so this handler's parameter would have been
        undefined.
    */
    signal ratioEdited(real value)

    /*! Long-axis extent available to the two slots once the handle is removed. */
    readonly property real usable: Math.max(0, (vertical ? height : width) - handleThickness)
    /*! Keeps a slot from collapsing to nothing, which would leave its own
        handle unreachable and the pane inside it unclickable. */
    readonly property real minSlot: 48
    readonly property real clampedRatio: {
        if (usable <= 2 * minSlot)
            return 0.5                       // too small to honour the minimum
        return Math.max(minSlot / usable, Math.min(1 - minSlot / usable, root.ratio))
    }
    readonly property real sizeA: Math.round(usable * clampedRatio)

    Item {
        id: slotAItem
        x: 0
        y: 0
        width: root.vertical ? root.width : root.sizeA
        height: root.vertical ? root.sizeA : root.height
        clip: true
    }

    Item {
        id: slotBItem
        x: root.vertical ? 0 : root.sizeA + root.handleThickness
        y: root.vertical ? root.sizeA + root.handleThickness : 0
        width: root.vertical ? root.width : Math.max(0, root.usable - root.sizeA)
        height: root.vertical ? Math.max(0, root.usable - root.sizeA) : root.height
        clip: true
    }

    // ------------------------------------------------------------- divider
    Rectangle {
        id: handle

        x: root.vertical ? 0 : root.sizeA
        y: root.vertical ? root.sizeA : 0
        width: root.vertical ? root.width : root.handleThickness
        height: root.vertical ? root.handleThickness : root.height

        color: drag.pressed ? Theme.accent
             : drag.containsMouse ? Theme.panelElevated
                                  : Theme.border
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

        MouseArea {
            id: drag

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: root.vertical ? Qt.SplitVCursor : Qt.SplitHCursor
            acceptedButtons: Qt.LeftButton
            // The panes below carry their own MouseArea for box zoom; without
            // this a drag that starts on the handle can be stolen by one.
            preventStealing: true

            property real pressPos: 0
            property real pressRatio: 0.5

            onPressed: function (mouse) {
                // Measured against the NODE, which does not move, rather than
                // against the handle, which does - the same reason the DBC
                // editor's column handles map into their header row.
                const p = mapToItem(root, mouse.x, mouse.y)
                pressPos = root.vertical ? p.y : p.x
                pressRatio = root.clampedRatio
            }

            onPositionChanged: function (mouse) {
                if (!pressed || root.usable <= 0)
                    return
                const p = mapToItem(root, mouse.x, mouse.y)
                const now = root.vertical ? p.y : p.x
                root.ratio = pressRatio + (now - pressPos) / root.usable
                root.ratioEdited(root.clampedRatio)
            }

            /*! Back to an even split - the cheapest undo for a drag that went
                somewhere silly. */
            onDoubleClicked: {
                root.ratio = 0.5
                root.ratioEdited(0.5)
            }
        }
    }
}
