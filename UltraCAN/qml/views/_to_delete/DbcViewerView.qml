import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs

import UltraCAN.Theme
import UltraCAN.Models
import UltraCAN.Components
import UltraCAN.Core

/*!
    \qmltype DbcViewerView
    \brief Read-only datasheet browser for the loaded CAN database.

    Same explorer and the same document as DbcEditorView, but the right pane is
    a formal inspector: every value is displayed, none is editable. Because no
    control ever steals a binding here, the sheets bind straight to the model -
    no Loader recycling like the editor needs.
*/
BasePage {
    id: root

    title: qsTr("DBC Viewer")
    subtitle: qsTr("%1 - read only").arg(DbcModel.databaseName)
    contentMargins: 0

    // ---------------------------------------------------------------- state
    property int selectedMessage: 0
    property int selectedSignal: -1
    property bool allExpanded: true
    property string statusMessage: ""

    readonly property bool hasSelection: selectedMessage >= 0
    readonly property bool signalSelected: hasSelection && selectedSignal >= 0

    /*! Live handles on the selected objects - plain bindings, always current. */
    readonly property var currentMessage: hasSelection ? DbcModel.message(selectedMessage) : null
    readonly property var currentSignal: signalSelected ? DbcModel.signalAt(selectedMessage, selectedSignal)
                                                        : null

    // -- file I/O (real, through the C++ DbcParser) ---------------------------
    property bool busy: false
    property string busyMessage: ""
    property string pendingName: ""
    /*! The URL the C++ model is given - the file name alone is not enough. */
    property url pendingUrl

    function select(messageIndex, signalIndex) {
        selectedMessage = messageIndex
        selectedSignal = signalIndex
    }

    // ------------------------------------------------------------ formatting
    function formatCanId(id) {
        return "0x" + Number(id).toString(16).toUpperCase().padStart(3, "0")
    }

    function byteOrderName(order) {
        return DbcModel.byteOrderNames[order] !== undefined
                ? DbcModel.byteOrderNames[order]
                : qsTr("Unknown")
    }

    function valueTypeName(type) {
        return DbcModel.valueTypeNames[type] !== undefined
                ? DbcModel.valueTypeNames[type]
                : qsTr("Unknown")
    }

    /*! "phys = raw * 0.125 + 0" - the decoding rule, spelled out. */
    function scalingFormula(sig) {
        if (!sig)
            return ""
        const offset = sig.offset
        const sign = offset < 0 ? "-" : "+"
        return "phys = raw * %1 %2 %3".arg(sig.factor).arg(sign).arg(Math.abs(offset))
    }

    // --------------------------------------------------------------- file I/O
    function fileNameOf(fileUrl) {
        return decodeURIComponent(fileUrl.toString().split("/").pop())
    }

    function beginLoad(fileUrl) {
        pendingUrl = fileUrl
        pendingName = fileNameOf(fileUrl)
        busyMessage = qsTr("Parsing %1 ...").arg(pendingName)
        busy = true
        ioTimer.restart()
    }

    // The parse is synchronous C++; the short delay only lets the busy veil
    // paint first.
    Timer {
        id: ioTimer
        interval: 50
        onTriggered: {
            root.busy = false
            if (DbcModel.loadDbc(root.pendingUrl)) {
                root.select(DbcModel.messageCount() > 0 ? 0 : -1, -1)
                root.statusMessage = DbcModel.warnings.length > 0
                                     ? qsTr("Loaded %1 - %2 messages, %3 signals, %4 lines skipped.")
                                       .arg(DbcModel.databaseName)
                                       .arg(DbcModel.messageCount())
                                       .arg(root.totalSignalCount())
                                       .arg(DbcModel.warnings.length)
                                     : qsTr("Loaded %1 - %2 messages, %3 signals.")
                                       .arg(DbcModel.databaseName)
                                       .arg(DbcModel.messageCount())
                                       .arg(root.totalSignalCount())
            } else {
                root.statusMessage = DbcModel.lastError
            }
        }
    }

    FileDialog {
        id: openDialog
        title: qsTr("Open CAN database")
        fileMode: FileDialog.OpenFile
        nameFilters: [ qsTr("CAN databases (*.dbc)"), qsTr("All files (*)") ]
        onAccepted: root.beginLoad(selectedFile)
    }

    function totalSignalCount() {
        let n = 0
        for (let i = 0; i < DbcModel.messages.count; ++i)
            n += DbcModel.messages.get(i).signalList.count
        return n
    }

    onStatusMessageChanged: if (statusMessage.length > 0) statusTimer.restart()
    Timer {
        id: statusTimer
        interval: 4000
        onTriggered: root.statusMessage = ""
    }

    // ----------------------------------------------------------------- toolbar
    toolbar: Component {
        RowLayout {
            spacing: Theme.spacingSm

            ToolbarButton {
                text: qsTr("Load DBC")
                glyph: "▤"
                enabled: !root.busy
                onClicked: openDialog.open()
            }
            ToolbarButton {
                text: root.allExpanded ? qsTr("Collapse All") : qsTr("Expand All")
                glyph: root.allExpanded ? "▴" : "▾"
                onClicked: {
                    root.allExpanded = !root.allExpanded
                    explorer.setExpandedAll(root.allExpanded)
                }
            }
        }
    }

    // ================================================================== body
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        SplitView {
            id: splitView

            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            handle: Rectangle {
                implicitWidth: 4
                color: SplitHandle.pressed ? Theme.accent
                                           : (SplitHandle.hovered ? Theme.panelElevated : Theme.border)
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
            }

            // --------------------------------------------------- LEFT: explorer
            DbcExplorer {
                id: explorer

                SplitView.preferredWidth: splitView.width * 0.3
                SplitView.minimumWidth: 220
                SplitView.maximumWidth: splitView.width * 0.6

                messages: DbcModel.messages
                selectedMessage: root.selectedMessage
                selectedSignal: root.selectedSignal

                onMessageActivated: (messageIndex) => root.select(messageIndex, -1)
                onSignalActivated: (messageIndex, signalIndex) => root.select(messageIndex, signalIndex)
            }

            // -------------------------------------------------- RIGHT: datasheet
            Rectangle {
                SplitView.fillWidth: true
                SplitView.minimumWidth: 380
                color: Theme.surface

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    // -- sheet header
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.headerHeight
                        color: Theme.panel

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spacingLg
                            anchors.rightMargin: Theme.spacingLg
                            spacing: Theme.spacingMd

                            Rectangle {
                                Layout.preferredHeight: 18
                                Layout.preferredWidth: badgeLabel.implicitWidth + Theme.spacingMd
                                visible: root.hasSelection
                                radius: Theme.radiusSm
                                color: root.signalSelected ? Theme.accentAlt : Theme.accent

                                Label {
                                    id: badgeLabel
                                    anchors.centerIn: parent
                                    text: root.signalSelected ? qsTr("SIGNAL") : qsTr("MESSAGE")
                                    color: Theme.background
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeSm
                                    font.weight: Font.Bold
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: root.signalSelected
                                      ? (root.currentSignal ? root.currentSignal.name : "")
                                      : (root.currentMessage ? root.currentMessage.name : qsTr("No selection"))
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeLg
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }

                            Label {
                                text: qsTr("READ ONLY")
                                color: Theme.textDisabled
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeSm
                                font.letterSpacing: 1.2
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

                    // -- the sheet
                    Loader {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        sourceComponent: !root.hasSelection ? emptySheet
                                                            : (root.signalSelected ? signalSheet : messageSheet)
                    }
                }
            }
        }

        // -------------------------------------------------------- status strip
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

                Label {
                    Layout.fillWidth: true
                    text: root.statusMessage
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    elide: Text.ElideRight
                }
                Label {
                    text: qsTr("%1 messages - %2 signals")
                          .arg(DbcModel.messages.count).arg(root.totalSignalCount())
                    color: Theme.textDisabled
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

    // ========================================================= sheet templates
    Component {
        id: emptySheet

        Item {
            Label {
                anchors.centerIn: parent
                text: qsTr("Select a message or a signal to inspect it.")
                color: Theme.textDisabled
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMd
            }
        }
    }

    // ---------------------------------------------------------- MESSAGE sheet
    Component {
        id: messageSheet

        ScrollView {
            id: messageScroll
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                width: messageScroll.availableWidth
                spacing: Theme.spacingXs

                Item { Layout.preferredHeight: Theme.spacingMd }

                SheetSection { text: qsTr("IDENTIFICATION") }

                ReadOnlyPropertyRow {
                    label: qsTr("Name")
                    value: root.currentMessage ? root.currentMessage.name : ""
                    highlighted: true
                }
                ReadOnlyPropertyRow {
                    label: qsTr("CAN ID")
                    mono: true
                    value: root.currentMessage
                           ? qsTr("%1   (%2)").arg(root.formatCanId(root.currentMessage.canId))
                                              .arg(root.currentMessage.canId)
                           : ""
                }
                ReadOnlyPropertyRow {
                    label: qsTr("Length")
                    mono: true
                    unit: qsTr("bytes")
                    value: root.currentMessage ? String(root.currentMessage.dlc) : ""
                }

                Item { Layout.preferredHeight: Theme.spacingMd }
                SheetSection { text: qsTr("TIMING AND NODES") }

                ReadOnlyPropertyRow {
                    label: qsTr("Transmitter")
                    value: root.currentMessage ? root.currentMessage.transmitter : ""
                }
                ReadOnlyPropertyRow {
                    label: qsTr("Cycle time")
                    mono: true
                    unit: qsTr("ms")
                    value: root.currentMessage
                           ? (root.currentMessage.cycleMs > 0 ? String(root.currentMessage.cycleMs)
                                                              : qsTr("event driven"))
                           : ""
                }

                Item { Layout.preferredHeight: Theme.spacingMd }
                SheetSection { text: qsTr("DOCUMENTATION") }

                ReadOnlyPropertyRow {
                    label: qsTr("Comment")
                    value: root.currentMessage ? root.currentMessage.comment : ""
                }

                Item { Layout.preferredHeight: Theme.spacingMd }
                SheetSection {
                    text: qsTr("SIGNALS (%1)").arg(root.currentMessage ? root.currentMessage.signalList.count : 0)
                }

                // -- compact signal table -------------------------------------
                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spacingLg
                    Layout.rightMargin: Theme.spacingLg
                    Layout.preferredHeight: signalTable.implicitHeight + 2
                    color: Theme.background
                    radius: Theme.radiusSm
                    border.width: 1
                    border.color: Theme.border

                    ColumnLayout {
                        id: signalTable
                        anchors.fill: parent
                        anchors.margins: 1
                        spacing: 0

                        // header
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Theme.rowHeight
                            color: Theme.panel

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.spacingSm
                                anchors.rightMargin: Theme.spacingSm
                                spacing: Theme.spacingSm

                                TableHeaderCell { text: qsTr("Signal");  Layout.fillWidth: true }
                                TableHeaderCell { text: qsTr("Start");   Layout.preferredWidth: 44 }
                                TableHeaderCell { text: qsTr("Len");     Layout.preferredWidth: 36 }
                                TableHeaderCell { text: qsTr("Order");   Layout.preferredWidth: 96 }
                                TableHeaderCell { text: qsTr("Factor");  Layout.preferredWidth: 64 }
                                TableHeaderCell { text: qsTr("Offset");  Layout.preferredWidth: 64 }
                                TableHeaderCell { text: qsTr("Unit");    Layout.preferredWidth: 56 }
                            }
                        }

                        Repeater {
                            model: root.currentMessage ? root.currentMessage.signalList : null

                            delegate: Rectangle {
                                id: signalRow

                                required property int index
                                required property var model

                                Layout.fillWidth: true
                                Layout.preferredHeight: Theme.rowHeight
                                color: rowMouse.containsMouse ? Theme.panelElevated
                                                              : (index % 2 ? Theme.background : Theme.plotBackground)

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.spacingSm
                                    anchors.rightMargin: Theme.spacingSm
                                    spacing: Theme.spacingSm

                                    TableCell {
                                        text: signalRow.model.name
                                        Layout.fillWidth: true
                                        color: Theme.textPrimary
                                    }
                                    TableCell { text: String(signalRow.model.startBit);  Layout.preferredWidth: 44; mono: true }
                                    TableCell { text: String(signalRow.model.bitLength); Layout.preferredWidth: 36; mono: true }
                                    TableCell { text: root.byteOrderName(signalRow.model.byteOrder); Layout.preferredWidth: 96 }
                                    TableCell { text: String(signalRow.model.factor);    Layout.preferredWidth: 64; mono: true }
                                    TableCell { text: String(signalRow.model.offset);    Layout.preferredWidth: 64; mono: true }
                                    TableCell { text: signalRow.model.unit;              Layout.preferredWidth: 56 }
                                }

                                MouseArea {
                                    id: rowMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.select(root.selectedMessage, signalRow.index)
                                }
                            }
                        }
                    }
                }

                Item { Layout.preferredHeight: Theme.spacingLg }
            }
        }
    }

    // ----------------------------------------------------------- SIGNAL sheet
    Component {
        id: signalSheet

        ScrollView {
            id: signalScroll
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                width: signalScroll.availableWidth
                spacing: Theme.spacingXs

                Item { Layout.preferredHeight: Theme.spacingMd }

                SheetSection { text: qsTr("IDENTIFICATION") }

                ReadOnlyPropertyRow {
                    label: qsTr("Name")
                    value: root.currentSignal ? root.currentSignal.name : ""
                    highlighted: true
                }
                ReadOnlyPropertyRow {
                    label: qsTr("Message")
                    value: root.currentMessage
                           ? qsTr("%1  (%2)").arg(root.currentMessage.name)
                                             .arg(root.formatCanId(root.currentMessage.canId))
                           : ""
                }
                ReadOnlyPropertyRow {
                    label: qsTr("Receiver")
                    value: root.currentSignal ? root.currentSignal.receiver : ""
                }

                Item { Layout.preferredHeight: Theme.spacingMd }
                SheetSection { text: qsTr("BIT LAYOUT") }

                ReadOnlyPropertyRow {
                    label: qsTr("Start bit")
                    mono: true
                    value: root.currentSignal ? String(root.currentSignal.startBit) : ""
                }
                ReadOnlyPropertyRow {
                    label: qsTr("Length")
                    mono: true
                    unit: qsTr("bits")
                    value: root.currentSignal ? String(root.currentSignal.bitLength) : ""
                }
                ReadOnlyPropertyRow {
                    label: qsTr("Occupies")
                    mono: true
                    value: root.currentSignal
                           ? qsTr("bits %1 .. %2").arg(root.currentSignal.startBit)
                                                  .arg(root.currentSignal.startBit
                                                       + root.currentSignal.bitLength - 1)
                           : ""
                }
                ReadOnlyPropertyRow {
                    label: qsTr("Endianness")
                    value: root.currentSignal ? root.byteOrderName(root.currentSignal.byteOrder) : ""
                }
                ReadOnlyPropertyRow {
                    label: qsTr("Value type")
                    value: root.currentSignal ? root.valueTypeName(root.currentSignal.valueType) : ""
                }

                Item { Layout.preferredHeight: Theme.spacingMd }
                SheetSection { text: qsTr("SCALING") }

                ReadOnlyPropertyRow {
                    label: qsTr("Factor")
                    mono: true
                    value: root.currentSignal ? String(root.currentSignal.factor) : ""
                }
                ReadOnlyPropertyRow {
                    label: qsTr("Offset")
                    mono: true
                    value: root.currentSignal ? String(root.currentSignal.offset) : ""
                }
                ReadOnlyPropertyRow {
                    label: qsTr("Range")
                    mono: true
                    unit: root.currentSignal ? root.currentSignal.unit : ""
                    value: root.currentSignal
                           ? qsTr("%1 .. %2").arg(root.currentSignal.minimum)
                                             .arg(root.currentSignal.maximum)
                           : ""
                }
                ReadOnlyPropertyRow {
                    label: qsTr("Unit")
                    value: root.currentSignal ? root.currentSignal.unit : ""
                }

                // decoding rule, spelled out like a datasheet footnote
                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.labelColumnWidth + Theme.spacingLg + Theme.spacingMd
                    Layout.rightMargin: Theme.spacingLg
                    Layout.topMargin: Theme.spacingSm
                    Layout.preferredHeight: formulaLabel.implicitHeight + Theme.spacingMd
                    color: Theme.background
                    radius: Theme.radiusSm
                    border.width: 1
                    border.color: Theme.border

                    Label {
                        id: formulaLabel
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Theme.spacingMd
                        anchors.rightMargin: Theme.spacingMd
                        text: root.scalingFormula(root.currentSignal)
                        color: Theme.accent
                        font.family: Theme.monoFontFamily
                        font.pixelSize: Theme.fontSizeMd
                        wrapMode: Text.WordWrap
                    }
                }

                Item { Layout.preferredHeight: Theme.spacingMd }
                SheetSection { text: qsTr("DOCUMENTATION") }

                ReadOnlyPropertyRow {
                    label: qsTr("Comment")
                    value: root.currentSignal ? root.currentSignal.comment : ""
                }

                Item { Layout.preferredHeight: Theme.spacingLg }
            }
        }
    }

    // ------------------------------------------------------ inline helpers
    component SheetSection: Label {
        Layout.fillWidth: true
        Layout.leftMargin: Theme.spacingLg
        Layout.topMargin: Theme.spacingXs
        Layout.bottomMargin: Theme.spacingXs
        color: Theme.textDisabled
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeSm
        font.letterSpacing: 1.1
        font.weight: Font.DemiBold
    }

    component TableHeaderCell: Label {
        color: Theme.textDisabled
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeSm
        font.weight: Font.DemiBold
        elide: Text.ElideRight
    }

    component TableCell: Label {
        property bool mono: false
        color: Theme.textSecondary
        font.family: mono ? Theme.monoFontFamily : Theme.fontFamily
        font.pixelSize: Theme.fontSizeSm
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }
}
