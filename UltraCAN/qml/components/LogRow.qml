import QtQuick
import QtQuick.Controls.Basic

import UltraCAN.Theme

/*!
    \qmltype LogRow
    \brief One CAN frame in the trace window.

    Deliberately built from plain Items and Labels rather than a RowLayout:
    this delegate is instantiated and recycled dozens of times per second, and
    a Layout would re-solve constraints on every single one. Column widths come
    from the same \l columns array the TableHeaderBar uses.
*/
Rectangle {
    id: root

    /*! Shared column geometry - see TableHeaderBar. */
    property var columns: []
    /*! Horizontal gutter, must match the header. */
    property int gutter: Theme.spacingSm

    // -- frame fields
    property real time: 0
    property string direction: "Rx"
    property string channel: ""
    property string idText: ""
    property bool extended: false
    property int dlc: 0
    /*! Frames of this identity seen this session. 0 hides the cell rather than
        printing a zero that would read as "none arrived". */
    property int count: 0
    /*! Arrival rate in Hz, 0 when the identity has gone quiet. */
    property real frequency: 0
    /*! Column indexes to leave out - must be the SAME list the header gets, or
        the two drift. See TableHeaderBar::hiddenColumns. */
    property var hiddenColumns: []

    function columnShown(i) { return root.hiddenColumns.indexOf(i) === -1 }
    /*! Width of the flexing last cell: the leftovers after the columns that
        are actually showing, gutters included. */
    readonly property real flexWidth: {
        let fixed = 0
        let shown = 0
        for (let i = 0; i < root.columns.length - 1; ++i) {
            if (!root.columnShown(i))
                continue
            fixed += root.columns[i]
            shown += 1
        }
        return Math.max(120, root.width - 2 * root.gutter - fixed - root.gutter * shown)
    }
    // NOTE: must NOT be called "data" - that is Item's default property, and
    // shadowing it makes every child object in this file fail to parse with
    // "Cannot assign to property of unknown type QString".
    property string payload: ""

    /*! Zebra striping. */
    property bool alternate: false
    property bool selected: false
    /*! Playback playhead: draws an accent bar on the leading edge. */
    property bool active: false

    signal clicked()

    readonly property color directionColor: direction === "Tx" ? Theme.accent : Theme.success

    height: Theme.rowHeight
    color: selected || active ? Theme.selection
                              : (hover.hovered ? Theme.panelElevated
                                               : (alternate ? Theme.plotBackground : Theme.background))

    // playhead marker (CAN Log Player)
    Rectangle {
        width: 3
        height: parent.height
        color: Theme.accent
        visible: root.active
    }

    HoverHandler { id: hover }
    TapHandler { onTapped: root.clicked() }

    Row {
        anchors.fill: parent
        anchors.leftMargin: root.gutter
        anchors.rightMargin: root.gutter
        spacing: root.gutter

        // 0 - time
        Cell {
            width: root.columns[0]
            text: root.time.toFixed(4)
            mono: true
            color: Theme.textSecondary
            horizontalAlignment: Text.AlignRight
        }

        // 1 - direction (colour coded)
        Item {
            width: root.columns[1]
            height: parent.height

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: dirLabel.implicitWidth + Theme.spacingSm
                height: 15
                radius: 2
                color: Qt.rgba(root.directionColor.r, root.directionColor.g, root.directionColor.b, 0.16)

                Label {
                    id: dirLabel
                    anchors.centerIn: parent
                    text: root.direction
                    color: root.directionColor
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    font.weight: Font.DemiBold
                }
            }
        }

        // 2 - channel
        Cell {
            width: root.columns[2]
            text: root.channel
            color: Theme.textSecondary
        }

        // 3 - identifier
        Cell {
            width: root.columns[3]
            text: root.idText
            mono: true
            color: Theme.textPrimary
        }

        // 4 - how many of this identity have arrived
        Cell {
            width: root.columns[4]
            text: root.count > 0 ? String(root.count) : ""
            mono: true
            // Dimmer than the payload: it is context for the row, not the row.
            color: Theme.textSecondary
            horizontalAlignment: Text.AlignRight
        }

        // 5 - arrival rate. Overwrite mode only: in a chronological trace
        //     every row is one frame and a per-row Hz means nothing.
        Cell {
            width: root.columns[5]
            visible: root.columnShown(5)
            text: root.frequency > 0 ? root.frequency.toFixed(1) : "-"
            mono: true
            // A rate that has fallen to zero is a message that STOPPED, which
            // is usually the interesting one on the screen.
            color: root.frequency > 0 ? Theme.textSecondary : Theme.warning
            horizontalAlignment: Text.AlignRight
        }

        // 6 - frame type
        Cell {
            width: root.columns[6]
            text: root.extended ? qsTr("Ext") : qsTr("Std")
            color: root.extended ? Theme.accentAlt : Theme.textDisabled
        }

        // 7 - length
        Cell {
            width: root.columns[7]
            text: String(root.dlc)
            mono: true
            color: Theme.textSecondary
            horizontalAlignment: Text.AlignRight
        }

        // 8 - payload (flexes)
        //
        // Width comes from flexWidth, which counts only the columns that are
        // SHOWING. The old version hardcoded the column and gutter counts,
        // which silently becomes wrong the moment a column can be hidden. See
        // table_columns.md - this arithmetic has to match TableHeaderBar's
        // exactly or everything to the right drifts.
        Cell {
            width: root.flexWidth
            text: root.payload
            mono: true
            color: Theme.textPrimary
        }
    }

    // hairline between rows
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.surface
        opacity: 0.6
    }

    component Cell: Label {
        property bool mono: false
        height: Theme.rowHeight
        font.family: mono ? Theme.monoFontFamily : Theme.fontFamily
        font.pixelSize: Theme.fontSizeSm
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }
}
