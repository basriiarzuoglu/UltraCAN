import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs

import UltraCAN.Theme
import UltraCAN.Models
import UltraCAN.Components
import UltraCAN.Core

/*!
    \qmltype CanMessageSenderView
    \brief Transmit-job list (left) and frame builder (right).

    Since phase 11 the transmit path is entirely C++: CanSenderModel owns one
    QTimer per cyclic job and writes through CanBusManager, which puts the frame
    on the hardware (when connected) and echoes it into the Logger's trace. This
    view builds jobs and reports state; it never sends anything itself.
*/
BasePage {
    id: root

    title: qsTr("CAN Message Sender")
    subtitle: qsTr("Build raw or DBC-encoded frames and put them on the bus")
    contentMargins: 0

    // ---------------------------------------------------------------- state
    property int selectedJob: -1
    property string statusMessage: ""
    property int framesPerSecond: 0
    property int lastTotal: 0

    /*! Raw builder payload, one entry per byte. Kept out of the controls so the
        Add action reads plain data rather than scraping text fields. */
    property var rawBytes: [ 0, 0, 0, 0, 0, 0, 0, 0 ]
    /*! DBC builder values: signalIndex -> physical value. */
    property var signalValues: ({})

    // ------------------------------------------------------------- transmit
    //
    // Transmission is entirely C++ now: CanSenderModel owns a QTimer per cyclic
    // job and writes through CanBusManager. The view only asks and reports.

    function sendOnce(jobIndex) {
        const job = CanSenderModel.jobAt(jobIndex)
        const ok = CanSenderModel.sendOnce(jobIndex)
        statusMessage = ok ? qsTr("Sent %1 once.").arg(job.idText)
                           : qsTr("Could not send %1: %2").arg(job.idText)
                                                          .arg(CanSenderModel.lastError)
    }

    function sendAllOnce() {
        const sent = CanSenderModel.sendAllOnce()
        statusMessage = qsTr("Sent %1 of %2 jobs once.").arg(sent).arg(CanSenderModel.count)
    }

    Timer {
        interval: 1000
        repeat: true
        running: true
        onTriggered: {
            root.framesPerSecond = CanSenderModel.totalSent - root.lastTotal
            root.lastTotal = CanSenderModel.totalSent
        }
    }

    onStatusMessageChanged: if (statusMessage.length > 0) statusTimer.restart()
    Timer {
        id: statusTimer
        interval: 4000
        onTriggered: root.statusMessage = ""
    }

    // ------------------------------------------------------- builder helpers
    function setRawByte(index, hexText) {
        let bytes = root.rawBytes.slice()
        const parsed = parseInt(hexText, 16)
        bytes[index] = isNaN(parsed) ? 0 : (parsed & 0xFF)
        root.rawBytes = bytes
    }

    function rawPayloadText(dlc) {
        return CanSenderModel.bytesToHex(root.rawBytes.slice(0, dlc))
    }

    function resetSignalValues(messageIndex) {
        let values = ({})
        const signalList = DbcModel.signalsOf(messageIndex)
        if (signalList) {
            for (let i = 0; i < signalList.count; ++i) {
                const sig = signalList.get(i)
                // Start at 0 when the signal can represent it, otherwise at its floor.
                values[i] = (sig.minimum <= 0 && sig.maximum >= 0) ? 0 : sig.minimum
            }
        }
        root.signalValues = values
    }

    function setSignalValue(signalIndex, value) {
        let values = Object.assign({}, root.signalValues)
        values[signalIndex] = value
        root.signalValues = values
    }

    /*! Hex payload for the selected DBC message, encoded by the C++ packer -
        the same bit layout CanDecoder reads back. */
    function dbcPayloadHex(messageIndex) {
        return CanSenderModel.encodePayload(messageIndex, root.signalValues)
    }

    // ------------------------------------------------------------- add jobs
    function addRawJob() {
        const parsedId = parseInt(rawIdField.text.replace(/^0[xX]/, ""), 16)
        if (isNaN(parsedId)) {
            statusMessage = qsTr("Cannot add: the CAN ID is not valid hex.")
            return
        }
        const dlc = rawDlcBox.value
        // Asked BEFORE the add, or the row we are about to update is the row
        // the answer describes and every add looks like a replacement.
        const replaced = CanSenderModel.indexOfFrame(
                             parsedId, rawExtendedSwitch.checked,
                             rawChannelBox.currentIndex) >= 0
        const index = CanSenderModel.addJob({
            name: rawNameField.text.length > 0 ? rawNameField.text : qsTr("Raw frame"),
            canId: parsedId,
            extended: rawExtendedSwitch.checked,
            channel: CanSenderModel.channelNames[rawChannelBox.currentIndex],
            dlc: dlc,
            payload: rawPayloadText(dlc),
            cycleMs: rawCycleBox.value,
            origin: "raw"
        })
        root.selectedJob = index
        // Silently replacing a row is how a user ends up believing they queued
        // two frames. Name which one moved.
        statusMessage = replaced
            ? qsTr("Updated %1 - it was already in the transmit list.")
              .arg(CanSenderModel.formatId(parsedId, rawExtendedSwitch.checked))
            : qsTr("Added %1 to the transmit list.")
              .arg(CanSenderModel.formatId(parsedId, rawExtendedSwitch.checked))
    }

    function addDbcJob() {
        const messageIndex = dbcMessageBox.currentIndex
        const msg = DbcModel.message(messageIndex)
        if (!msg) {
            statusMessage = qsTr("Cannot add: no message selected.")
            return
        }
        const replaced = CanSenderModel.indexOfFrame(
                             msg.canId, msg.extended, dbcChannelBox.currentIndex) >= 0
        const index = CanSenderModel.addJob({
            name: msg.name,
            canId: msg.canId,
            // The database knows whether the id is 11- or 29-bit; never assume.
            extended: msg.extended,
            channel: CanSenderModel.channelNames[dbcChannelBox.currentIndex],
            dlc: msg.dlc,
            payload: root.dbcPayloadHex(messageIndex),
            cycleMs: dbcCycleBox.value,
            origin: "dbc"
        })
        root.selectedJob = index
        statusMessage = replaced
            ? qsTr("Updated %1 - %2 signals re-encoded into the existing row.")
              .arg(msg.name).arg(msg.signalList.count)
            : qsTr("Added %1 (%2 signals encoded).")
              .arg(msg.name).arg(msg.signalList.count)
    }

    Component.onCompleted: resetSignalValues(0)

    // --------------------------------------------------------------- toolbar
    toolbar: Component {
        RowLayout {
            spacing: Theme.spacingSm

            ToolbarButton {
                text: qsTr("Send All Once")
                glyph: "↑"
                enabled: CanSenderModel.count > 0 && CanSenderModel.canTransmit
                onClicked: root.sendAllOnce()
                ToolTip.visible: hovered && !CanSenderModel.canTransmit
                ToolTip.text: qsTr("Connect the bus first")
            }
            ToolbarButton {
                text: qsTr("Clear List")
                glyph: "✕"
                destructive: true
                enabled: CanSenderModel.count > 0
                onClicked: {
                    CanSenderModel.clearJobs()
                    root.selectedJob = -1
                    root.statusMessage = qsTr("Transmit list cleared.")
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

            // ---------------------------------------------- LEFT: transmit jobs
            Rectangle {
                SplitView.preferredWidth: splitView.width * 0.35
                SplitView.minimumWidth: 320
                SplitView.maximumWidth: splitView.width * 0.6
                color: Theme.panel

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    Item {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Theme.toolbarHeight

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spacingMd
                            anchors.rightMargin: Theme.spacingMd

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("TRANSMIT JOBS")
                                color: Theme.textDisabled
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeSm
                                font.letterSpacing: 1.2
                            }
                            Label {
                                text: qsTr("%1 armed / %2")
                                      .arg(CanSenderModel.armedCount)
                                      .arg(CanSenderModel.count)
                                color: Theme.textDisabled
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeSm
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

                    ListView {
                        id: jobList

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: CanSenderModel.jobs
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        delegate: Rectangle {
                            id: jobRow

                            required property int index
                            required property var model

                            readonly property bool cyclic: model.cycleMs > 0

                            width: ListView.view.width
                            height: 52
                            color: root.selectedJob === index ? Theme.selection
                                                              : (jobHover.hovered ? Theme.panelElevated
                                                                                  : "transparent")

                            HoverHandler { id: jobHover }
                            TapHandler { onTapped: root.selectedJob = jobRow.index }

                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: 1
                                color: Theme.border
                                opacity: 0.5
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.spacingSm
                                anchors.rightMargin: Theme.spacingSm
                                spacing: Theme.spacingSm

                                // arm / disarm cyclic transmission
                                ThemedSwitch {
                                    Layout.alignment: Qt.AlignVCenter
                                    enabled: jobRow.cyclic
                                    opacity: jobRow.cyclic ? 1 : 0.35
                                    checked: jobRow.model.enabled
                                    onToggled: {
                                        const wanted = checked
                                        // Restore the binding the click just broke.
                                        checked = Qt.binding(function() { return jobRow.model.enabled })
                                        CanSenderModel.setEnabled(jobRow.index, wanted)
                                    }
                                    ToolTip.visible: hovered && (!jobRow.cyclic || !CanSenderModel.canTransmit)
                                    ToolTip.text: !jobRow.cyclic
                                                  ? qsTr("Manual jobs are sent with the Send button")
                                                  : qsTr("Armed - starts when the bus connects")
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0

                                    RowLayout {
                                        spacing: Theme.spacingSm

                                        Label {
                                            text: jobRow.model.idText
                                            color: Theme.accent
                                            font.family: Theme.monoFontFamily
                                            font.pixelSize: Theme.fontSizeMd
                                            font.weight: Font.DemiBold
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: jobRow.model.name
                                            color: Theme.textPrimary
                                            font.family: Theme.fontFamily
                                            font.pixelSize: Theme.fontSizeMd
                                            elide: Text.ElideRight
                                        }
                                    }

                                    RowLayout {
                                        spacing: Theme.spacingSm

                                        Label {
                                            text: CanSenderModel.formatCycle(jobRow.model.cycleMs)
                                            color: jobRow.cyclic ? Theme.accentAlt : Theme.textDisabled
                                            font.family: Theme.fontFamily
                                            font.pixelSize: Theme.fontSizeSm
                                        }
                                        Label {
                                            text: jobRow.model.channel + " - " + jobRow.model.payload
                                            color: Theme.textDisabled
                                            font.family: Theme.monoFontFamily
                                            font.pixelSize: Theme.fontSizeSm
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                    }
                                }

                                Label {
                                    Layout.alignment: Qt.AlignVCenter
                                    text: qsTr("%1x").arg(jobRow.model.sentCount)
                                    color: Theme.textDisabled
                                    font.family: Theme.monoFontFamily
                                    font.pixelSize: Theme.fontSizeSm
                                }

                                ToolbarButton {
                                    Layout.alignment: Qt.AlignVCenter
                                    text: qsTr("Send")
                                    glyph: "↑"
                                    enabled: CanSenderModel.canTransmit
                                    onClicked: root.sendOnce(jobRow.index)
                                    ToolTip.visible: hovered && !CanSenderModel.canTransmit
                                    ToolTip.text: qsTr("Connect the bus first")
                                }
                                ToolbarButton {
                                    Layout.alignment: Qt.AlignVCenter
                                    text: "✕"
                                    destructive: true
                                    onClicked: {
                                        CanSenderModel.removeJob(jobRow.index)
                                        root.selectedJob = -1
                                    }
                                }
                            }
                        }
                    }

                    // empty state
                    Label {
                        Layout.fillWidth: true
                        Layout.margins: Theme.spacingLg
                        visible: CanSenderModel.count === 0
                        text: qsTr("No transmit jobs yet.\nBuild one on the right and press Add to Transmit List.")
                        color: Theme.textDisabled
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeMd
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                    }
                }
            }

            // ------------------------------------------------ RIGHT: the builder
            Rectangle {
                SplitView.fillWidth: true
                SplitView.minimumWidth: 420
                color: Theme.surface

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    // -- mode tabs
                    TabBar {
                        id: modeTabs

                        Layout.fillWidth: true
                        background: Rectangle {
                            color: Theme.panel
                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: 1
                                color: Theme.border
                            }
                        }

                        ModeTab { text: qsTr("Raw Mode") }
                        ModeTab { text: qsTr("DBC Mode") }
                        ModeTab { text: qsTr("Log Replay") }
                    }

                    StackLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        currentIndex: modeTabs.currentIndex

                        // ============================================ RAW MODE
                        ScrollView {
                            id: rawScroll
                            contentWidth: availableWidth
                            clip: true

                            ColumnLayout {
                                width: rawScroll.availableWidth
                                spacing: Theme.spacingSm

                                Item { Layout.preferredHeight: Theme.spacingMd }

                                BuilderSection { text: qsTr("FRAME") }

                                PropertyField {
                                    label: qsTr("Name")
                                    ThemedTextField {
                                        id: rawNameField
                                        Layout.fillWidth: true
                                        text: "Raw frame"
                                        placeholderText: qsTr("Shown in the transmit list")
                                    }
                                }
                                PropertyField {
                                    label: qsTr("CAN ID")
                                    required_: true
                                    suffix: qsTr("hex")
                                    ThemedTextField {
                                        id: rawIdField
                                        Layout.fillWidth: true
                                        text: "0x100"
                                        font.family: Theme.monoFontFamily
                                        validator: RegularExpressionValidator {
                                            regularExpression: /^(0[xX])?[0-9a-fA-F]{1,8}$/
                                        }
                                    }
                                }
                                PropertyField {
                                    label: qsTr("Frame type")
                                    ThemedSwitch {
                                        id: rawExtendedSwitch
                                        Layout.alignment: Qt.AlignVCenter
                                        text: checked ? qsTr("Extended (29 bit)") : qsTr("Standard (11 bit)")
                                    }
                                }
                                PropertyField {
                                    label: qsTr("Channel")
                                    ThemedComboBox {
                                        id: rawChannelBox
                                        Layout.fillWidth: true
                                        model: CanSenderModel.channelNames
                                    }
                                }
                                PropertyField {
                                    label: qsTr("DLC")
                                    suffix: qsTr("bytes")
                                    ThemedSpinBox {
                                        id: rawDlcBox
                                        Layout.fillWidth: true
                                        from: 0
                                        to: 8
                                        value: 8
                                    }
                                }
                                PropertyField {
                                    label: qsTr("Cycle time")
                                    suffix: qsTr("ms")
                                    ThemedSpinBox {
                                        id: rawCycleBox
                                        Layout.fillWidth: true
                                        from: 0
                                        to: 10000
                                        stepSize: 10
                                        value: 100
                                    }
                                }
                                Label {
                                    Layout.leftMargin: Theme.spacingLg + Theme.labelColumnWidth + Theme.spacingMd
                                    text: rawCycleBox.value === 0 ? qsTr("0 = single shot (manual send only)")
                                                                  : qsTr("Sent every %1 ms while armed").arg(rawCycleBox.value)
                                    color: Theme.textDisabled
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeSm
                                }

                                Item { Layout.preferredHeight: Theme.spacingMd }
                                BuilderSection { text: qsTr("PAYLOAD") }

                                RowLayout {
                                    Layout.leftMargin: Theme.spacingLg
                                    Layout.rightMargin: Theme.spacingLg
                                    spacing: Theme.spacingSm

                                    Repeater {
                                        model: 8

                                        delegate: ColumnLayout {
                                            required property int index
                                            spacing: 2

                                            Label {
                                                Layout.alignment: Qt.AlignHCenter
                                                text: qsTr("B%1").arg(index)
                                                color: Theme.textDisabled
                                                font.family: Theme.monoFontFamily
                                                font.pixelSize: Theme.fontSizeSm
                                            }
                                            ThemedTextField {
                                                Layout.preferredWidth: 44
                                                enabled: index < rawDlcBox.value
                                                opacity: enabled ? 1 : 0.4
                                                text: "00"
                                                maximumLength: 2
                                                horizontalAlignment: Text.AlignHCenter
                                                font.family: Theme.monoFontFamily
                                                validator: RegularExpressionValidator {
                                                    regularExpression: /^[0-9a-fA-F]{0,2}$/
                                                }
                                                onTextChanged: root.setRawByte(index, text)
                                            }
                                        }
                                    }

                                    Item { Layout.fillWidth: true }
                                }

                                Label {
                                    Layout.leftMargin: Theme.spacingLg
                                    text: qsTr("Frame: %1").arg(root.rawPayloadText(rawDlcBox.value))
                                    color: Theme.textSecondary
                                    font.family: Theme.monoFontFamily
                                    font.pixelSize: Theme.fontSizeMd
                                }

                                Item { Layout.preferredHeight: Theme.spacingMd }

                                ToolbarButton {
                                    Layout.leftMargin: Theme.spacingLg
                                    text: qsTr("Add to Transmit List")
                                    glyph: "+"
                                    primary: true
                                    onClicked: root.addRawJob()
                                }

                                Item { Layout.preferredHeight: Theme.spacingLg }
                            }
                        }

                        // ============================================ DBC MODE
                        ScrollView {
                            id: dbcScroll
                            contentWidth: availableWidth
                            clip: true

                            ColumnLayout {
                                width: dbcScroll.availableWidth
                                spacing: Theme.spacingSm

                                Item { Layout.preferredHeight: Theme.spacingMd }

                                BuilderSection { text: qsTr("MESSAGE") }

                                PropertyField {
                                    label: qsTr("DBC message")
                                    required_: true
                                    ThemedComboBox {
                                        id: dbcMessageBox
                                        Layout.fillWidth: true
                                        model: DbcModel.messages
                                        textRole: "name"
                                        onCurrentIndexChanged: root.resetSignalValues(currentIndex)
                                    }
                                }
                                ReadOnlyPropertyRow {
                                    label: qsTr("CAN ID")
                                    mono: true
                                    value: {
                                        const msg = DbcModel.message(dbcMessageBox.currentIndex)
                                        return msg ? DbcModel.formatCanId(msg.canId) : ""
                                    }
                                }
                                ReadOnlyPropertyRow {
                                    label: qsTr("DLC")
                                    mono: true
                                    unit: qsTr("bytes")
                                    value: {
                                        const msg = DbcModel.message(dbcMessageBox.currentIndex)
                                        return msg ? String(msg.dlc) : ""
                                    }
                                }
                                PropertyField {
                                    label: qsTr("Channel")
                                    ThemedComboBox {
                                        id: dbcChannelBox
                                        Layout.fillWidth: true
                                        model: CanSenderModel.channelNames
                                    }
                                }
                                PropertyField {
                                    label: qsTr("Cycle time")
                                    suffix: qsTr("ms")
                                    ThemedSpinBox {
                                        id: dbcCycleBox
                                        Layout.fillWidth: true
                                        from: 0
                                        to: 10000
                                        stepSize: 10
                                        // The fallback already existed but only
                                        // covered "no message selected". The real
                                        // gap is a message whose database carries
                                        // no BA_ "GenMsgCycleTime": DbcMessageData
                                        // defaults cycleMs to 0, and 0 is a
                                        // meaningful value here - single shot - so
                                        // it sailed through as if the DBC had asked
                                        // for it. Most .dbc files in the wild do not
                                        // declare the attribute, which is why DBC
                                        // mode came up single-shot while Raw mode
                                        // came up at 100.
                                        //
                                        // A cycle the database DOES declare still
                                        // wins: that is real information about the
                                        // frame and better than any default.
                                        value: {
                                            const msg = DbcModel.message(dbcMessageBox.currentIndex)
                                            return (msg && msg.cycleMs > 0) ? msg.cycleMs : 100
                                        }
                                    }
                                }

                                Item { Layout.preferredHeight: Theme.spacingMd }
                                BuilderSection { text: qsTr("SIGNAL VALUES") }

                                // One row per signal of the selected message.
                                Repeater {
                                    model: DbcModel.signalsOf(dbcMessageBox.currentIndex)

                                    delegate: PropertyField {
                                        id: signalField

                                        required property int index
                                        required property var model

                                        label: model.name
                                        suffix: model.unit

                                        ThemedSlider {
                                            Layout.fillWidth: true
                                            from: signalField.model.minimum
                                            to: signalField.model.maximum
                                            stepSize: signalField.model.factor
                                            value: root.signalValues[signalField.index] !== undefined
                                                   ? root.signalValues[signalField.index]
                                                   : signalField.model.minimum
                                            onMoved: root.setSignalValue(signalField.index, value)
                                        }

                                        Label {
                                            Layout.preferredWidth: 78
                                            horizontalAlignment: Text.AlignRight
                                            text: {
                                                const v = root.signalValues[signalField.index]
                                                // Fixed three decimals like
                                                // every other value readout -
                                                // a width that changes with the
                                                // factor cannot be scanned.
                                                return v === undefined ? "-"
                                                                       : Number(v).toFixed(3)
                                            }
                                            color: Theme.textPrimary
                                            font.family: Theme.monoFontFamily
                                            font.pixelSize: Theme.fontSizeMd
                                        }
                                    }
                                }

                                Item { Layout.preferredHeight: Theme.spacingMd }
                                BuilderSection { text: qsTr("ENCODED PAYLOAD") }

                                // Live preview of the packed bytes - this is the
                                // same packer the transmit path uses.
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: Theme.spacingLg
                                    Layout.rightMargin: Theme.spacingLg
                                    Layout.preferredHeight: Theme.inputHeight + Theme.spacingSm
                                    color: Theme.background
                                    radius: Theme.radiusSm
                                    border.width: 1
                                    border.color: Theme.border

                                    Label {
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.leftMargin: Theme.spacingMd
                                        anchors.rightMargin: Theme.spacingMd
                                        text: root.dbcPayloadHex(dbcMessageBox.currentIndex)
                                        color: Theme.accent
                                        font.family: Theme.monoFontFamily
                                        font.pixelSize: Theme.fontSizeLg
                                        elide: Text.ElideRight
                                    }
                                }

                                Item { Layout.preferredHeight: Theme.spacingMd }

                                ToolbarButton {
                                    Layout.leftMargin: Theme.spacingLg
                                    text: qsTr("Add to Transmit List")
                                    glyph: "+"
                                    primary: true
                                    onClicked: root.addDbcJob()
                                }

                                Item { Layout.preferredHeight: Theme.spacingLg }
                            }
                        }

                        // ========================================= LOG REPLAY
                        //
                        // The physical replay engine lives HERE and only here.
                        // The Graph Plot's transport replays a recording into
                        // the application; this one puts it on the wire, and
                        // the two are different C++ objects so that no state on
                        // the analyst's side can ever reach a transmitter.
                        ScrollView {
                            id: replayScroll
                            contentWidth: availableWidth
                            clip: true

                            ColumnLayout {
                                width: replayScroll.availableWidth
                                spacing: Theme.spacingSm

                                Item { Layout.preferredHeight: Theme.spacingMd }

                                // -- the warning that has to be read before the
                                //    controls, not after them.
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: Theme.spacingMd
                                    Layout.rightMargin: Theme.spacingMd
                                    Layout.preferredHeight: hazardText.implicitHeight
                                                            + Theme.spacingSm * 2
                                    radius: Theme.radiusSm
                                    color: Theme.panel
                                    border.width: 1
                                    border.color: LogReplayModel.armed ? Theme.error : Theme.border

                                    Label {
                                        id: hazardText
                                        x: Theme.spacingSm
                                        y: Theme.spacingSm
                                        width: parent.width - Theme.spacingSm * 2
                                        wrapMode: Text.WordWrap
                                        color: LogReplayModel.armed ? Theme.error
                                                                    : Theme.textSecondary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontSizeSm
                                        text: LogReplayModel.armed
                                            ? qsTr("ARMED. Pressing Play writes every frame in "
                                                 + "this recording to the CAN hardware with its "
                                                 + "original timing.")
                                            : qsTr("This tab transmits a recording on the real "
                                                 + "bus. Frames reach any controller attached to "
                                                 + "it and can move actuators. Arm it only on a "
                                                 + "bus you intend to drive.")
                                    }
                                }

                                BuilderSection { text: qsTr("RECORDING") }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: Theme.spacingMd
                                    Layout.rightMargin: Theme.spacingMd
                                    spacing: Theme.spacingSm

                                    ToolbarButton {
                                        text: qsTr("Load Log File")
                                        glyph: "▤"
                                        // The doorway into this whole tab -
                                        // give it the accent fill the same way
                                        // Main.qml's Connect button does, so
                                        // it reads as a clickable action, not
                                        // a caption.
                                        primary: !LogReplayModel.loaded
                                        // Locked while transmitting: swapping
                                        // the recording under a live run is not
                                        // a thing to make possible.
                                        enabled: !LogReplayModel.transmitting
                                        onClicked: replayFileDialog.open()
                                    }
                                    ToolbarButton {
                                        text: qsTr("Clear")
                                        enabled: LogReplayModel.loaded
                                                 && !LogReplayModel.transmitting
                                        onClicked: LogReplayModel.clear()
                                    }
                                    Item { Layout.fillWidth: true }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: Theme.spacingMd
                                    Layout.rightMargin: Theme.spacingMd
                                    wrapMode: Text.WordWrap
                                    color: LogReplayModel.loaded ? Theme.textPrimary
                                                                 : Theme.textDisabled
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeSm
                                    text: LogReplayModel.loaded
                                        ? qsTr("%1 - %2 frames over %3")
                                          .arg(LogReplayModel.sourceName)
                                          .arg(LogReplayModel.count)
                                          .arg(LogReplayModel.formatTime(LogReplayModel.duration))
                                        : qsTr("No recording loaded. CSV, PCAN .trc and "
                                             + "Vector .asc are read.")
                                }

                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: Theme.spacingMd
                                    Layout.rightMargin: Theme.spacingMd
                                    visible: LogReplayModel.warnings.length > 0
                                    wrapMode: Text.WordWrap
                                    text: LogReplayModel.warnings.join("  ")
                                    color: Theme.warning
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeSm
                                }

                                BuilderSection { text: qsTr("TRANSPORT") }

                                // -- timeline
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: Theme.spacingMd
                                    Layout.rightMargin: Theme.spacingMd
                                    spacing: Theme.spacingSm

                                    Label {
                                        Layout.preferredWidth: 74
                                        text: LogReplayModel.formatTime(LogReplayModel.position)
                                        color: Theme.accent
                                        font.family: Theme.monoFontFamily
                                        font.pixelSize: Theme.fontSizeSm
                                        font.weight: Font.DemiBold
                                    }

                                    ThemedSlider {
                                        id: replayTimeline

                                        Layout.fillWidth: true
                                        enabled: LogReplayModel.loaded
                                        from: 0
                                        to: Math.max(LogReplayModel.duration, 0.001)

                                        // Dragging must win over the pacer, and
                                        // the pacer must take over the moment
                                        // the thumb is released - a plain
                                        // `value:` binding is broken by the
                                        // first drag.
                                        Binding {
                                            target: replayTimeline
                                            property: "value"
                                            value: LogReplayModel.position
                                            when: !replayTimeline.pressed
                                            restoreMode: Binding.RestoreBindingOrValue
                                        }

                                        onMoved: LogReplayModel.seek(value)
                                    }

                                    Label {
                                        Layout.preferredWidth: 74
                                        horizontalAlignment: Text.AlignRight
                                        text: LogReplayModel.formatTime(LogReplayModel.duration)
                                        color: Theme.textSecondary
                                        font.family: Theme.monoFontFamily
                                        font.pixelSize: Theme.fontSizeSm
                                    }
                                }

                                // -- transport row
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: Theme.spacingMd
                                    Layout.rightMargin: Theme.spacingMd
                                    spacing: Theme.spacingSm

                                    ToolbarButton {
                                        text: qsTr("Play")
                                        glyph: "▶"
                                        // Three conditions, all of them real:
                                        // something to send, somewhere to send
                                        // it, and permission to do so.
                                        enabled: LogReplayModel.loaded
                                                 && LogReplayModel.busReady
                                                 && LogReplayModel.armed
                                                 && !LogReplayModel.playing
                                        statusColor: LogReplayModel.transmitting ? Theme.error
                                                                                 : "transparent"
                                        onClicked: LogReplayModel.play()
                                    }
                                    ToolbarButton {
                                        text: qsTr("Pause")
                                        glyph: "❚❚"
                                        enabled: LogReplayModel.playing
                                        onClicked: LogReplayModel.pause()
                                    }
                                    ToolbarButton {
                                        text: qsTr("Stop")
                                        glyph: "■"
                                        destructive: true
                                        enabled: LogReplayModel.loaded
                                        onClicked: LogReplayModel.stop()
                                    }
                                    ToolbarButton {
                                        text: qsTr("Loop")
                                        glyph: "⟲"
                                        primary: LogReplayModel.loop
                                        enabled: LogReplayModel.loaded
                                        onClicked: LogReplayModel.loop = !LogReplayModel.loop
                                    }

                                    ThemedComboBox {
                                        Layout.preferredWidth: 88
                                        textRole: "label"
                                        currentIndex: 1
                                        enabled: LogReplayModel.loaded
                                        model: ListModel {
                                            ListElement { label: "0.5x"; factor: 0.5 }
                                            ListElement { label: "1x";   factor: 1.0 }
                                            ListElement { label: "2x";   factor: 2.0 }
                                            ListElement { label: "5x";   factor: 5.0 }
                                            ListElement { label: "10x";  factor: 10.0 }
                                        }
                                        onActivated: LogReplayModel.speed =
                                                     model.get(currentIndex).factor
                                    }

                                    Item { Layout.fillWidth: true }
                                }

                                BuilderSection { text: qsTr("SAFETY INTERLOCK") }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: Theme.spacingMd
                                    Layout.rightMargin: Theme.spacingMd
                                    spacing: Theme.spacingSm

                                    ToolbarButton {
                                        text: LogReplayModel.armed ? qsTr("Disarm")
                                                                   : qsTr("Arm Transmission")
                                        glyph: LogReplayModel.armed ? "⏻" : "⚠"
                                        enabled: LogReplayModel.loaded && LogReplayModel.busReady
                                        // The safety interlock's own control:
                                        // always tinted (never the plain
                                        // resting style), and ToolbarButton
                                        // gives tinted buttons a heavier
                                        // border and a hover fill for exactly
                                        // this reason - it must never read as
                                        // flat text.
                                        statusColor: LogReplayModel.armed ? Theme.error
                                                                          : Theme.warning
                                        onClicked: {
                                            if (LogReplayModel.armed) {
                                                LogReplayModel.armed = false
                                                return
                                            }
                                            armConfirm.open()
                                        }
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        color: !LogReplayModel.busReady ? Theme.warning
                                             : LogReplayModel.armed ? Theme.error
                                                                    : Theme.textDisabled
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.fontSizeSm
                                        text: !LogReplayModel.busReady
                                            ? qsTr("No CAN interface connected - nothing can be "
                                                 + "transmitted.")
                                            : LogReplayModel.armed
                                              ? qsTr("Armed. Stop clears this automatically.")
                                              : qsTr("Play does nothing until this is armed.")
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: Theme.spacingMd
                                    Layout.rightMargin: Theme.spacingMd
                                    visible: LogReplayModel.lastError.length > 0
                                    wrapMode: Text.WordWrap
                                    text: LogReplayModel.lastError
                                    color: Theme.error
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeSm
                                }

                                Item { Layout.preferredHeight: Theme.spacingLg }
                            }
                        }
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

                Rectangle {
                    id: txDot
                    width: 8; height: 8; radius: 4
                    color: !CanSenderModel.canTransmit ? Theme.textDisabled
                                                       : (CanSenderModel.armedCount > 0 ? Theme.accent
                                                                                        : Theme.textDisabled)

                    SequentialAnimation on opacity {
                        running: CanSenderModel.canTransmit && CanSenderModel.armedCount > 0
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.3; duration: 500 }
                        NumberAnimation { to: 1.0; duration: 500 }
                        onStopped: txDot.opacity = 1
                    }
                }
                Label {
                    // "armed but offline" is its own state: the jobs are ready,
                    // the bus is not, and nothing is going out.
                    text: !CanSenderModel.canTransmit
                          ? (CanSenderModel.armedCount > 0 ? qsTr("ARMED - BUS OFFLINE") : qsTr("BUS OFFLINE"))
                          : (CanSenderModel.armedCount > 0 ? qsTr("TRANSMITTING") : qsTr("IDLE"))
                    color: !CanSenderModel.canTransmit
                           ? Theme.warning
                           : (CanSenderModel.armedCount > 0 ? Theme.accent : Theme.textSecondary)
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    font.weight: Font.DemiBold
                }
                Label {
                    text: qsTr("%1 jobs").arg(CanSenderModel.count)
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                }
                Label {
                    text: qsTr("%1 sent").arg(CanSenderModel.totalSent)
                    color: Theme.textSecondary
                    font.family: Theme.monoFontFamily
                    font.pixelSize: Theme.fontSizeSm
                }
                Label {
                    text: qsTr("%1 fps").arg(root.framesPerSecond)
                    color: Theme.textSecondary
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
                }
            }
        }
    }

    // ------------------------------------------------------------ log replay
    FileDialog {
        id: replayFileDialog
        title: qsTr("Load a recording to transmit")
        nameFilters: [ qsTr("Trace files (*.csv *.trc *.asc)"), qsTr("All files (*)") ]
        onAccepted: {
            if (!LogReplayModel.loadLog(selectedFile))
                root.statusMessage = LogReplayModel.lastError
            else
                root.statusMessage = qsTr("Loaded %1 for replay.").arg(LogReplayModel.sourceName)
        }
    }

    Dialog {
        id: armConfirm

        anchors.centerIn: Overlay.overlay
        modal: true
        /*!
            Fixed, not derived. The previous version left the Dialog's own
            width to be computed from its contentItem while that contentItem
            (a bare Label) had its own literal `width: 400` - two things each
            trying to size themselves off the other, which QML detects and
            reports as "Binding loop detected for property implicitWidth".
            Pinning the Dialog's width breaks the cycle at the top: everything
            below sizes FROM this value, nothing feeds back into it.
        */
        width: 400
        title: qsTr("Arm physical transmission?")
        /*!
            No `standardButtons`. Those render with the Basic style's
            unstyled defaults - a plain OS-palette header and generic
            OK/Cancel buttons - which is exactly the "native white dialog on
            a dark app" look this was rewritten to get rid of. header/footer
            below are the same custom-chrome pattern SignalValuesDialog.qml
            already uses, so every themed Dialog in the app now looks and is
            built the same way.
        */

        onAccepted: {
            LogReplayModel.armed = true
            if (!LogReplayModel.armed)
                root.statusMessage = LogReplayModel.lastError    // bus went away
        }

        background: Rectangle {
            color: Theme.panel
            radius: Theme.radiusMd
            border.width: 1
            border.color: Theme.error
        }

        header: Item {
            implicitHeight: Theme.headerHeight
            Label {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Theme.spacingMd
                anchors.rightMargin: Theme.spacingMd
                anchors.verticalCenter: parent.verticalCenter
                text: armConfirm.title
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeLg
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.border
            }
        }

        contentItem: Label {
            // availableWidth (= Dialog.width - left/rightPadding) follows
            // FROM the Dialog's fixed width above; it does not feed back
            // into it, so this can never re-open the binding loop that fix
            // closed.
            width: armConfirm.availableWidth
            wrapMode: Text.WordWrap
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeMd
            text: qsTr("“%1” will be written to the CAN hardware frame by frame, with the "
                     + "original inter-frame timing, exactly as if the recorded ECUs were "
                     + "present on this bus.\n\nAny controller attached can act on those "
                     + "frames. Confirm only if this is a bus you intend to drive.")
                  .arg(LogReplayModel.sourceName)
        }

        footer: Item {
            implicitHeight: Theme.headerHeight

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: Theme.border
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingMd
                anchors.rightMargin: Theme.spacingMd
                spacing: Theme.spacingSm

                Item { Layout.fillWidth: true }

                ToolbarButton {
                    // Plain resting style (Theme.panel fill, Theme.border
                    // outline) reads as the safe, reversible choice next to
                    // an accent-filled hazard action.
                    text: qsTr("Cancel")
                    onClicked: armConfirm.reject()
                }
                ToolbarButton {
                    // "Arm", not "OK": this is a confirmation for a specific
                    // hazardous action, and the button should name it rather
                    // than make the user re-read the dialog to know what
                    // accepting does. primary: true gives it the same accent
                    // fill Main.qml's Connect button uses for its one
                    // call-to-action.
                    text: qsTr("Arm")
                    primary: true
                    onClicked: armConfirm.accept()
                }
            }
        }
    }

    Connections {
        target: LogReplayModel

        function onCompleted() {
            root.statusMessage = qsTr("Replay finished - transmission disarmed.")
        }
        function onLooped() {
            root.statusMessage = qsTr("Replay looped.")
        }
    }

    // -------------------------------------------------------- inline helpers
    component BuilderSection: Label {
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

    component ModeTab: TabButton {
        id: tab

        implicitHeight: Theme.toolbarHeight
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeMd

        contentItem: Label {
            text: tab.text
            color: tab.checked ? Theme.textPrimary : Theme.textSecondary
            font: tab.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            color: tab.checked ? Theme.surface
                               : (tab.hovered ? Theme.panelElevated : Theme.panel)

            // active-tab underline
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 2
                color: Theme.accent
                visible: tab.checked
            }
        }
    }
}
