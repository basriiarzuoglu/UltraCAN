import QtQuick
import QtQuick.Controls.Basic

import UltraCAN.Theme

/*!
    \qmltype TableHeaderBar
    \brief Fixed column header for the trace-style tables.

    Column geometry is shared with the rows by handing both the same
    \l columns array (pixel widths; the LAST column always takes the
    remaining space). Keeping the widths in one array is what guarantees the
    header and the rows stay aligned - a header that computes its own widths
    drifts the moment a scrollbar appears.
*/
Rectangle {
    id: root

    /*! Pixel width of each fixed column; the last entry is ignored (it flexes). */
    property var columns: []
    /*! Caption per column. */
    property var titles: []
    /*! Right-align these column indexes (numeric columns). */
    property var rightAligned: []

    /*!
        Column indexes to leave out entirely, for a column that only exists in
        some modes.

        A \c Row skips an invisible child AND the spacing next to it, so hiding
        the cell keeps the header and the rows aligned on its own - as long as
        the ROW DELEGATE hides the same index. What it does not fix is the
        flexing last column, which subtracts a fixed total and a gutter count:
        both have to shrink with the hidden column or everything to its right
        drifts by one width plus one gutter. See table_columns.md - this is the
        same arithmetic that produced the original -64 px drift. */
    property var hiddenColumns: []

    // ------------------------------------------------------------- sorting
    /*! { columnIndex: "roleName" } for the headers that sort. Anything not
        listed is inert - most columns have no meaningful order. */
    property var sortableColumns: ({})
    /*! Role name the model is currently sorted on, "" for none. */
    property string sortRoleName: ""
    property bool sortDescending: false
    /*! Greys the sortable captions when the model cannot sort right now. */
    property bool sortEnabled: false

    /*! A sortable caption was clicked. The VIEW decides what that means - this
        component owns no model and no ordering. */
    signal sortRequested(string roleName)

    function roleForColumn(index) {
        const role = root.sortableColumns[index]
        return role === undefined ? "" : role
    }

    /*! Sum of the fixed columns that are actually showing. */
    readonly property real visibleFixedTotal: {
        let sum = 0
        for (let i = 0; i < root.columns.length - 1; ++i)
            if (root.hiddenColumns.indexOf(i) === -1)
                sum += root.columns[i]
        return sum
    }
    /*! Gutters between showing columns: one fewer than the columns. */
    readonly property int visibleGutters: {
        let shown = 0
        for (let i = 0; i < root.titles.length; ++i)
            if (root.hiddenColumns.indexOf(i) === -1)
                shown += 1
        return Math.max(0, shown - 1)
    }
    /*! Horizontal gutter, must match the rows. */
    property int gutter: Theme.spacingSm

    /*! Let the user drag the boundaries between columns.

        Implemented here rather than in each view because \l columns is already
        the shared contract - the header and every row read the same array, so
        the drag has exactly one number to change and alignment cannot drift.
        The LAST column is never draggable: it has no width of its own, it is
        whatever is left. */
    property bool resizable: false
    /*! Floor for a dragged column. Small enough to tuck a column away, large
        enough that it can still be grabbed again. */
    property int minColumnWidth: 24

    /*! A drag finished moving column \a index to \a width. The view owns
        \l columns and must write it back - reassign the whole array, because
        a `var` notifies by reference and mutating one slot in place changes
        nothing on screen. */
    signal columnResized(int index, real width)
    /*! Double-click on a splitter: the view should restore its default. */
    signal columnResetRequested(int index)

    implicitHeight: Theme.rowHeight + 2
    color: Theme.panel

    // bottom rule
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.border
    }

    Row {
        id: headerRow

        anchors.fill: parent
        anchors.leftMargin: root.gutter
        anchors.rightMargin: root.gutter
        spacing: root.gutter

        Repeater {
            model: root.titles

            delegate: Item {
                id: headerCell

                required property int index
                required property string modelData

                readonly property bool last: index === root.titles.length - 1

                visible: root.hiddenColumns.indexOf(index) === -1

                width: last ? Math.max(120, root.width - 2 * root.gutter
                                            - root.visibleFixedTotal
                                            - root.gutter * root.visibleGutters)
                            : root.columns[index]
                // The grip hangs past the cell's right edge; without this it
                // is clipped to nothing and the column cannot be grabbed.
                clip: false
                height: parent.height

                readonly property string sortRole: root.roleForColumn(index)
                readonly property bool sortable: root.sortEnabled && sortRole.length > 0
                readonly property bool sortedHere: sortable && root.sortRoleName === sortRole

                Label {
                    anchors.fill: parent
                    // The arrow rides on the caption rather than in a cell of
                    // its own so it cannot fall out of alignment with it.
                    text: headerCell.sortedHere
                          ? modelData + (root.sortDescending ? "  \u2193" : "  \u2191")
                          : modelData
                    color: headerCell.sortedHere ? Theme.accent
                         : headerCell.sortable && sortArea.containsMouse ? Theme.textPrimary
                                                                         : Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.6
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: root.rightAligned.indexOf(index) !== -1
                                         ? Text.AlignRight : Text.AlignLeft
                }

                // column separator
                Rectangle {
                    id: separator

                    visible: !headerCell.last
                    anchors.right: parent.right
                    anchors.rightMargin: -root.gutter / 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 1
                    height: parent.height * 0.5
                    // Lights up under the grip so the invisible hit area
                    // announces itself before the drag starts.
                    color: (grip.containsMouse || grip.pressed) ? Theme.accent : Theme.border
                }

                // -- click to sort
                //
                // Below the resize grip in z-order by virtue of being declared
                // first: the 5 px at the cell's edge must resize, not sort, or
                // the two gestures fight over the same pixels.
                MouseArea {
                    id: sortArea

                    anchors.fill: parent
                    enabled: headerCell.sortable
                    hoverEnabled: enabled
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: root.sortRequested(headerCell.sortRole)
                }

                // -- drag handle, straddling the separator
                MouseArea {
                    id: grip

                    // The last column flexes; there is no width to drag.
                    enabled: root.resizable && !headerCell.last
                    visible: enabled
                    width: 5
                    height: parent.height
                    anchors.right: parent.right
                    anchors.rightMargin: -root.gutter / 2 - 2
                    hoverEnabled: true
                    cursorShape: Qt.SplitHCursor
                    acceptedButtons: Qt.LeftButton

                    /*! Both captured in headerRow's coordinates, NOT this
                        item's. The grip moves with the column as it resizes,
                        so a delta measured against the grip itself is a delta
                        against a moving origin - the column creeps away from
                        the cursor by the grab offset on every event. */
                    property real pressSceneX: 0
                    property real pressWidth: 0

                    onPressed: function (mouse) {
                        pressSceneX = mapToItem(headerRow, mouse.x, 0).x
                        pressWidth = root.columns[headerCell.index]
                    }
                    onPositionChanged: function (mouse) {
                        if (!pressed)
                            return
                        const nowX = mapToItem(headerRow, mouse.x, 0).x
                        root.columnResized(headerCell.index,
                                           Math.max(root.minColumnWidth,
                                                    Math.round(pressWidth + (nowX - pressSceneX))))
                    }
                    onDoubleClicked: root.columnResetRequested(headerCell.index)
                }
            }
        }
    }
}
