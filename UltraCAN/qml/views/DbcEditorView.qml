import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs

import UltraCAN.Theme
import UltraCAN.Components
import UltraCAN.Core

/*!
    \qmltype DbcEditorView
    \brief One full-width hierarchical grid over the whole database, with a
           Setup button per row opening the detailed editor.

    \section2 What replaced what

    The previous layout was an explorer tree beside a property inspector. It
    worked, but it showed one object at a time: comparing the start bits of
    four signals meant clicking each in turn, and the inspector column ate a
    third of the window to display a dozen fields. The grid inverts that -
    everything is visible at once, and editing moves into a dialog that is only
    on screen while you are actually editing.

    \section2 Where the rows come from

    \l DbcFlatModel, not DbcModel. DbcModel is still the single source of truth
    and every mutation goes through it; the flat model only publishes the row
    sequence a grid needs (messages, plus the signals of expanded messages)
    with a role per column. See its class comment for the rebuild policy.

    \section2 Live values

    The value column is decoded off the live bus, and that decode is switched
    on here and only here - \c liveValuesEnabled follows this page's lifetime,
    so navigating away stops the work rather than leaving it running behind
    another view.
*/
BasePage {
    id: root

    title: qsTr("DBC Editor")
    subtitle: DbcModel.databaseName + (DbcModel.modified ? qsTr(" - modified") : "")
    contentMargins: 0

    // ---------------------------------------------------------------- state
    /*!
        Selection is held as MODEL indices, never as a row number.

        A row number is only valid until the next rebuild: expand a message
        above the selection and every row below it shifts, so a stored row
        would quietly come to mean a different signal. Message/signal indices
        survive that, and \l currentRow re-derives the row from them.
    */
    property int selectedMessage: -1
    property int selectedSignal: -1
    /*! Palette index of the selected row, handed to the setup dialog so its
        swatches match the grid's. */
    property int selectedColorIndex: 0

    /*! Row of the selection in the flat model, or -1.

        DbcFlatModel.count is named as a dependency: it is notified on every
        structural rebuild (QML re-runs a binding when the NOTIFY signal fires,
        whether or not the number changed), which is exactly when a row number
        can go stale. */
    readonly property int currentRow: {
        DbcFlatModel.count
        if (root.selectedMessage < 0)
            return -1
        return root.selectedSignal >= 0
             ? DbcFlatModel.rowForSignal(root.selectedMessage, root.selectedSignal)
             : DbcFlatModel.rowForMessage(root.selectedMessage)
    }

    /*! Transient hint shown in the editor's own status strip. */
    property string statusMessage: ""

    // -- file I/O -------------------------------------------------------------
    property bool busy: false
    property string busyMessage: ""
    property string pendingName: ""
    property url pendingUrl
    /*! 0 = idle, 1 = load, 2 = save. */
    property int pendingOperation: 0

    readonly property bool hasSelection: selectedMessage >= 0
    readonly property bool signalSelected: selectedSignal >= 0

    function select(messageIndex, signalIndex, colorIndex) {
        root.selectedMessage = messageIndex
        root.selectedSignal = signalIndex
        root.selectedColorIndex = colorIndex
    }

    // Keep the selected row on screen after it moves - adding a signal to a
    // collapsed message, say, expands nothing and scrolls nowhere otherwise.
    onCurrentRowChanged: if (currentRow >= 0)
                             grid.positionViewAtIndex(currentRow, ListView.Contain)

    // The live decode runs only while this page exists. Binding it to the
    // page's own lifetime is the whole gate: there is no other switch, so it
    // cannot be left on by accident.
    Component.onCompleted: DbcFlatModel.liveValuesEnabled = true
    Component.onDestruction: DbcFlatModel.liveValuesEnabled = false

    // ------------------------------------------------------------- file I/O
    /*! "file:///C:/db/powertrain.dbc" -> "powertrain.dbc" */
    function fileNameOf(fileUrl) {
        return decodeURIComponent(fileUrl.toString().split("/").pop())
    }

    function beginLoad(fileUrl) {
        pendingUrl = fileUrl
        pendingName = fileNameOf(fileUrl)
        pendingOperation = 1
        busyMessage = qsTr("Parsing %1 ...").arg(pendingName)
        busy = true
        ioTimer.restart()
    }

    function beginSave(fileUrl) {
        pendingUrl = fileUrl
        pendingName = fileNameOf(fileUrl)
        pendingOperation = 2
        busyMessage = qsTr("Writing %1 ...").arg(pendingName)
        busy = true
        ioTimer.restart()
    }

    /*!
        The work itself is synchronous C++; the short timer only lets the busy
        veil paint before the parse blocks the GUI thread.
    */
    function finishIo() {
        busy = false

        if (pendingOperation === 1) {
            if (DbcModel.loadDbc(pendingUrl)) {
                root.currentRow = DbcFlatModel.count > 0 ? 0 : -1
                statusMessage = DbcModel.warnings.length > 0
                                ? qsTr("Loaded %1 - %2 messages, %3 lines skipped.")
                                  .arg(DbcModel.databaseName)
                                  .arg(DbcModel.messageCount())
                                  .arg(DbcModel.warnings.length)
                                : qsTr("Loaded %1 - %2 messages.")
                                  .arg(DbcModel.databaseName)
                                  .arg(DbcModel.messageCount())
            } else {
                statusMessage = DbcModel.lastError
            }
        } else if (pendingOperation === 2) {
            statusMessage = DbcModel.saveDbc(pendingUrl)
                            ? qsTr("Saved %1.").arg(DbcModel.databaseName)
                            : DbcModel.lastError
        }

        pendingOperation = 0
    }

    Timer {
        id: ioTimer
        interval: 50            // just long enough for the busy veil to paint
        onTriggered: root.finishIo()
    }

    FileDialog {
        id: openDialog
        title: qsTr("Open CAN database")
        fileMode: FileDialog.OpenFile
        nameFilters: [ qsTr("CAN databases (*.dbc)"), qsTr("All files (*)") ]
        onAccepted: root.beginLoad(selectedFile)
    }

    FileDialog {
        id: saveDialog
        title: qsTr("Save CAN database as")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "dbc"
        nameFilters: [ qsTr("CAN databases (*.dbc)"), qsTr("All files (*)") ]
        onAccepted: root.beginSave(selectedFile)
    }

    CanChannelSetupDialog {
        id: setupDialog
    }

    // ------------------------------------------------------------- actions
    /*! Opens the detail editor. Called with the delegate's OWN indices rather
        than a row number, so it cannot open the wrong message if the row list
        rebuilds between the click and the dialog appearing. */
    function openSetup(messageIndex, signalIndex, colorIndex) {
        if (messageIndex < 0)
            return
        root.select(messageIndex, signalIndex, colorIndex)
        setupDialog.openFor(messageIndex, signalIndex, colorIndex)
    }

    function actionAddMessage() {
        // The flat model rebuilds on the next event-loop turn, so the row does
        // not exist yet - but the MESSAGE index does, and currentRow re-derives
        // itself as soon as it does exist.
        root.select(DbcModel.addMessage(), -1, root.selectedColorIndex)
        statusMessage = qsTr("Message added.")
    }

    function actionAddSignal() {
        if (selectedMessage < 0)
            return
        const index = DbcModel.addSignal(selectedMessage)
        if (index >= 0) {
            root.select(selectedMessage, index, root.selectedColorIndex)
            statusMessage = qsTr("Signal added.")
        }
    }

    function actionDelete() {
        if (!hasSelection)
            return
        const message = selectedMessage
        if (signalSelected) {
            DbcModel.removeSignal(message, selectedSignal)
            root.select(message, -1, root.selectedColorIndex)
            statusMessage = qsTr("Signal deleted.")
        } else {
            DbcModel.removeMessage(message)
            const remaining = DbcModel.messageCount()
            root.select(remaining > 0 ? Math.min(message, remaining - 1) : -1, -1, 0)
            statusMessage = qsTr("Message deleted.")
        }
    }

    // ----------------------------------------------------------- toolbar
    toolbar: Component {
        RowLayout {
            spacing: Theme.spacingSm

            ToolbarButton {
                text: qsTr("Load DBC")
                onClicked: openDialog.open()
            }
            ToolbarButton {
                text: qsTr("Save DBC")
                primary: DbcModel.modified
                onClicked: saveDialog.open()
            }

            Rectangle {   // separator
                Layout.preferredWidth: 1
                Layout.preferredHeight: Theme.inputHeight - 8
                Layout.alignment: Qt.AlignVCenter
                color: Theme.border
            }

            ToolbarButton {
                text: qsTr("Add Message")
                onClicked: root.actionAddMessage()
            }
            ToolbarButton {
                text: qsTr("Add Signal")
                enabled: root.selectedMessage >= 0
                onClicked: root.actionAddSignal()
            }
            ToolbarButton {
                text: qsTr("Delete")
                destructive: true
                enabled: root.hasSelection
                onClicked: root.actionDelete()
            }

            Rectangle {   // separator
                Layout.preferredWidth: 1
                Layout.preferredHeight: Theme.inputHeight - 8
                Layout.alignment: Qt.AlignVCenter
                color: Theme.border
            }

            ToolbarButton {
                text: qsTr("Expand All")
                onClicked: DbcModel.setExpandedAll(true)
            }
            ToolbarButton {
                text: qsTr("Collapse All")
                onClicked: DbcModel.setExpandedAll(false)
            }
        }
    }

    // =========================================================== column plan
    /*!
        Static column METADATA - one list driving the header and every cell, so
        a column can never be added to one and forgotten in the other.

        \c flex marks the column that absorbs whatever the fixed ones leave;
        exactly one column should have it.

        This array is \c readonly and never mutated, and that is load-bearing:
        it is the model of two Repeaters, and replacing it would make both
        destroy and rebuild every delegate. Widths therefore live in their own
        array below.
    */
    readonly property var columns: [
        { key: "arbText",       title: qsTr("ARB."),        flex: false, align: Text.AlignHCenter },
        { key: "formatText",    title: qsTr("Format"),      flex: false, align: Text.AlignHCenter },
        { key: "bitsText",      title: qsTr("Bits"),        flex: false, align: Text.AlignHCenter },
        { key: "frequencyText", title: qsTr("Frequency"),   flex: false, align: Text.AlignRight   },
        { key: "name",          title: qsTr("Name"),        flex: false, align: Text.AlignLeft    },
        { key: "description",   title: qsTr("Description"), flex: true,  align: Text.AlignLeft    },
        { key: "unit",          title: qsTr("Unit"),        flex: false, align: Text.AlignHCenter },
        { key: "minimum",       title: qsTr("Min"),         flex: false, align: Text.AlignRight   },
        { key: "liveText",      title: qsTr("Value"),       flex: false, align: Text.AlignHCenter },
        { key: "maximum",       title: qsTr("Max"),         flex: false, align: Text.AlignRight   }
    ]

    /*!
        Live column widths, in the same order as \l columns. Index 0 is 140 and
        not the old 74 because a 29-bit id renders as "1FFFFFFFh" - nine
        monospace characters plus two gutters - and was being elided.

        \b{Replaced wholesale, never mutated in place.} Writing
        \c{columnWidths[i] = w} changes the value and emits nothing, so every
        binding reading it keeps the old number; assigning a NEW array notifies
        once and re-evaluates exactly the widths that read it. That is also why
        the widths are not fields inside \l columns: mutating an object inside
        a var array is invisible to QML, and replacing that array would rebuild
        every delegate on every mouse-move of a drag.
    */
    property var columnWidths: root.defaultColumnWidths.slice()

    /*! The shipped widths, kept so a double-click on a separator can restore
        one column without the user having to remember what it was. */
    readonly property var defaultColumnWidths: [ 140, 80, 82, 86, 220, 0, 62, 74, 168, 74 ]

    /*! Narrow enough to hide a column's content, wide enough to grab again. */
    readonly property int minColumnWidth: 36
    readonly property int setupColumnWidth: 68

    /*! Column \a index is the flexible one, so it has no width of its own. */
    function isFlexColumn(index) {
        return index >= 0 && index < columns.length && columns[index].flex === true
    }

    /*! Sets one column's width, clamped. Called per mouse-move during a drag. */
    function setColumnWidth(index, w) {
        if (index < 0 || index >= root.columnWidths.length || root.isFlexColumn(index))
            return
        const clamped = Math.max(root.minColumnWidth, Math.round(w))
        if (root.columnWidths[index] === clamped)
            return
        // slice() then assign: a new array object is what carries the change
        // notification. Mutating the existing one is silent.
        let next = root.columnWidths.slice()
        next[index] = clamped
        root.columnWidths = next
    }

    /*! Restores one column to its shipped width. */
    function resetColumnWidth(index) {
        if (index < 0 || index >= root.defaultColumnWidths.length)
            return
        root.setColumnWidth(index, root.defaultColumnWidths[index])
    }

    /*! Width for column \a index, resolving the flexible one against the view. */
    function columnWidthAt(index) {
        return root.isFlexColumn(index) ? grid.slackWidth : root.columnWidths[index]
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ------------------------------------------------------ column header
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.rowHeight
            color: Theme.panelElevated
            z: 1

            Row {
                id: headerRow

                anchors.fill: parent
                spacing: 0

                Repeater {
                    model: root.columns

                    delegate: Item {
                        id: headerCell

                        required property var modelData
                        required property int index

                        width: root.columnWidthAt(index)
                        height: parent.height

                        Label {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spacingSm
                            anchors.rightMargin: Theme.spacingSm
                            verticalAlignment: Text.AlignVCenter
                            horizontalAlignment: modelData.align
                            text: modelData.title
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSm
                            font.letterSpacing: 1.1
                        }

                        Rectangle {
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 1
                            // Brighten while the separator is being dragged, so
                            // it is obvious which column is moving.
                            color: resizeGrip.pressed ? Theme.accent
                                 : resizeGrip.containsMouse ? Theme.textSecondary
                                                            : Theme.border
                        }

                        /*!
                            Column resize handle.

                            \section2 Why the delta is measured against headerRow

                            The tempting one-liner is
                            \c{width = width + mouse.x}. It does not judder -
                            the old width cancels, so it settles immediately -
                            but it is wrong by a constant: \c{mouse.x} is
                            measured from this handle's own left edge, and this
                            handle straddles the separator, so the first move
                            event silently adds however far into the 7 px grip
                            the user happened to click. Simulated: the same 50 px
                            drag yields 190, 193 or 196 px depending only on
                            where inside the handle it was grabbed, and dragging
                            out and back to the starting pixel leaves the column
                            3 px wider than it began.

                            Measuring the cursor in a frame that does NOT move -
                            headerRow - removes the offset: the width is always
                            \c{width at press + (cursor now - cursor at press)},
                            which depends on the pointer and nothing else. It
                            also survives the minimum-width clamp, returning to
                            the exact starting width when the cursor does.
                        */
                        MouseArea {
                            id: resizeGrip

                            // Not on the flexible column: it has no width of its
                            // own to set, and dragging it would silently turn it
                            // into a fixed column.
                            enabled: !root.isFlexColumn(headerCell.index)
                            visible: enabled

                            anchors.right: parent.right
                            anchors.rightMargin: -3      // straddle the separator
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 7                     // 5 is hard to hit at 100% DPI

                            hoverEnabled: true
                            cursorShape: Qt.SplitHCursor
                            acceptedButtons: Qt.LeftButton
                            // The header sits above a Flickable; without this a
                            // fast horizontal drag can be stolen as a flick.
                            preventStealing: true

                            property real pressSceneX: 0
                            property int pressWidth: 0

                            onPressed: function (mouse) {
                                pressSceneX = mapToItem(headerRow, mouse.x, 0).x
                                pressWidth = root.columnWidths[headerCell.index]
                            }

                            onPositionChanged: function (mouse) {
                                if (!pressed)
                                    return
                                const nowX = mapToItem(headerRow, mouse.x, 0).x
                                root.setColumnWidth(headerCell.index,
                                                    pressWidth + (nowX - pressSceneX))
                            }

                            // Double-click restores this one column's default,
                            // which is the cheapest possible undo for a drag
                            // that went somewhere silly.
                            onDoubleClicked: root.resetColumnWidth(headerCell.index)
                        }
                    }
                }

                Item {
                    width: root.setupColumnWidth
                    height: parent.height
                    Label {
                        anchors.centerIn: parent
                        text: qsTr("Setup")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        font.letterSpacing: 1.1
                    }
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.border
            }
        }

        // -------------------------------------------------------------- rows
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.surface
            clip: true

            ListView {
                id: grid

                anchors.fill: parent
                model: DbcFlatModel
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                reuseItems: true
                currentIndex: root.currentRow

                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                /*! Width left for the one flexible column. Re-evaluates on every
                    resize because it reads root.columnWidths, which is replaced
                    rather than mutated. */
                readonly property real slackWidth: {
                    let fixed = root.setupColumnWidth
                    for (let i = 0; i < root.columns.length; ++i) {
                        if (!root.isFlexColumn(i))
                            fixed += root.columnWidths[i]
                    }
                    return Math.max(root.minColumnWidth, grid.width - fixed)
                }

                delegate: Rectangle {
                    id: rowItem

                    required property int index
                    required property int kind
                    required property int depth
                    required property int colorIndex
                    required property int messageIndex
                    required property int signalIndex
                    required property bool expanded
                    required property bool hasChildren
                    required property int signalCount
                    required property string arbText
                    required property string formatText
                    required property string bitsText
                    required property string frequencyText
                    required property string name
                    required property string description
                    required property string unit
                    required property var minimum
                    required property var maximum
                    required property string liveText
                    required property var liveFraction

                    readonly property bool isMessage: rowItem.kind === 0
                    readonly property bool selected: rowItem.index === root.currentRow

                    width: ListView.view.width
                    height: Theme.rowHeight

                    // A message row is a header: it carries the panel fill so the
                    // eye can find the group boundaries without a tree line.
                    color: selected ? Theme.selection
                         : isMessage ? Theme.panel
                         : (rowMouse.containsMouse ? Theme.panelElevated : "transparent")

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
                        onClicked: root.select(rowItem.messageIndex, rowItem.signalIndex,
                                               rowItem.colorIndex)
                        onDoubleClicked: root.openSetup(rowItem.messageIndex,
                                                        rowItem.signalIndex,
                                                        rowItem.colorIndex)
                    }

                    Row {
                        anchors.fill: parent
                        spacing: 0

                        Repeater {
                            model: root.columns

                            delegate: Item {
                                required property var modelData
                                required property int index

                                readonly property bool isNameColumn: modelData.key === "name"
                                readonly property bool isValueColumn: modelData.key === "liveText"

                                width: root.columnWidthAt(index)
                                height: parent.height

                                /*! The raw role value for this cell. */
                                readonly property var cell: {
                                    switch (modelData.key) {
                                    case "arbText":       return rowItem.arbText
                                    case "formatText":    return rowItem.formatText
                                    case "bitsText":      return rowItem.bitsText
                                    case "frequencyText": return rowItem.frequencyText
                                    case "name":          return rowItem.name
                                    case "description":   return rowItem.description
                                    case "unit":          return rowItem.unit
                                    case "minimum":       return rowItem.minimum
                                    case "maximum":       return rowItem.maximum
                                    case "liveText":      return rowItem.liveText
                                    default:              return ""
                                    }
                                }

                                // -------------------------------- value column
                                // A bar behind the number, the way a DAQ tool
                                // shows a channel: the number for precision, the
                                // fill for "where in range is this".
                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: 2
                                    height: parent.height - 6
                                    visible: isValueColumn
                                             && rowItem.liveFraction !== undefined
                                    width: Math.max(0, (parent.width - 4)
                                                       * (rowItem.liveFraction || 0))
                                    radius: Theme.radiusSm
                                    color: Theme.seriesColor(rowItem.colorIndex)
                                    opacity: 0.28
                                }

                                // ------------------------------- name / indent
                                Row {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: Theme.spacingSm
                                              + (isNameColumn ? rowItem.depth * Theme.treeIndent : 0)
                                    anchors.rightMargin: Theme.spacingSm
                                    spacing: Theme.spacingXs
                                    visible: isNameColumn

                                    // Disclosure triangle, message rows only.
                                    Item {
                                        width: 14
                                        height: 14
                                        anchors.verticalCenter: parent.verticalCenter

                                        Label {
                                            anchors.centerIn: parent
                                            visible: rowItem.isMessage && rowItem.hasChildren
                                            text: rowItem.expanded ? "\u25BE" : "\u25B8"
                                            color: Theme.textSecondary
                                            font.pixelSize: Theme.fontSizeSm
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            anchors.margins: -4      // easier to hit
                                            enabled: rowItem.isMessage && rowItem.hasChildren
                                            onClicked: DbcFlatModel.toggleExpanded(rowItem.index)
                                        }
                                    }

                                    // Colour swatch: the identity the bit matrix,
                                    // the setup dialog and the value bar all share.
                                    Rectangle {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 10
                                        height: 10
                                        radius: 2
                                        color: Theme.seriesColor(rowItem.colorIndex)
                                    }

                                    Label {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: parent.width - 14 - 10 - 2 * Theme.spacingXs
                                        text: rowItem.name
                                        elide: Text.ElideRight
                                        color: rowItem.selected ? Theme.selectionText
                                                                : Theme.textPrimary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontSizeMd
                                        font.bold: rowItem.isMessage
                                    }
                                }

                                // -------------------------------- every other
                                Label {
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.spacingSm
                                    anchors.rightMargin: Theme.spacingSm
                                    visible: !isNameColumn
                                    verticalAlignment: Text.AlignVCenter
                                    horizontalAlignment: modelData.align
                                    elide: Text.ElideRight

                                    text: {
                                        const v = parent.cell
                                        if (v === undefined || v === null)
                                            return "-"
                                        // Every number in this application is
                                        // shown to three decimals; a column that
                                        // sometimes says 0 and sometimes 0.0 is
                                        // unreadable in a stack of rows.
                                        return (typeof v === "number") ? v.toFixed(3) : v
                                    }
                                    color: {
                                        if (rowItem.selected)
                                            return Theme.selectionText
                                        if (isValueColumn)
                                            return Theme.textPrimary
                                        return rowItem.isMessage ? Theme.textSecondary
                                                                 : Theme.textPrimary
                                    }
                                    font.family: isValueColumn ? Theme.monoFontFamily
                                                               : Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeMd
                                }

                                Rectangle {
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: 1
                                    color: Theme.border
                                    opacity: 0.5
                                }
                            }
                        }

                        // ------------------------------------- setup button
                        Item {
                            width: root.setupColumnWidth
                            height: parent.height

                            ToolbarButton {
                                anchors.centerIn: parent
                                width: root.setupColumnWidth - Theme.spacingSm
                                // An explicit HEIGHT, not just implicitHeight:
                                // centerIn was always centring the button, but
                                // the label inside it was placed by the
                                // Control's vertical padding rather than
                                // centred. ToolbarButton now zeroes that
                                // padding; the explicit height keeps the two
                                // facts - button box and content box - the
                                // same rectangle.
                                height: Theme.rowHeight - 8
                                leftPadding: 0
                                rightPadding: 0
                                text: qsTr("Setup")
                                onClicked: root.openSetup(rowItem.messageIndex,
                                                          rowItem.signalIndex,
                                                          rowItem.colorIndex)
                            }
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: Theme.border
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: DbcFlatModel.count === 0
                text: qsTr("No database loaded.\nUse Load DBC, or Add Message to start one.")
                horizontalAlignment: Text.AlignHCenter
                color: Theme.textDisabled
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMd
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
                spacing: Theme.spacingMd

                Label {
                    Layout.fillWidth: true
                    text: root.statusMessage
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    elide: Text.ElideRight
                }
                Label {
                    text: qsTr("%1 messages - %2 rows").arg(DbcModel.messages.count)
                                                       .arg(DbcFlatModel.count)
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                }
                Label {
                    text: DbcFlatModel.liveValuesEnabled && CanBusManager.connected
                          ? qsTr("LIVE") : qsTr("static")
                    color: DbcFlatModel.liveValuesEnabled && CanBusManager.connected
                           ? Theme.success : Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                }
            }
        }
    }

    LoadingOverlay {
        active: root.busy
        message: root.busyMessage
    }

    // Clear the transient status line a few seconds after it was set.
    onStatusMessageChanged: if (statusMessage.length > 0) statusTimer.restart()
    Timer {
        id: statusTimer
        interval: 4000
        onTriggered: root.statusMessage = ""
    }
}
