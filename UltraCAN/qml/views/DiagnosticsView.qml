import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import UltraCAN.Theme
import UltraCAN.Components
import UltraCAN.Core

/*!
    \qmltype DiagnosticsView
    \brief UDS (ISO 14229) / OBD-II tester over ISO-TP.

    A request-response console rather than a live view: pick the ECU's
    address pair, fire a service, read what came back. Everything on this
    page drives the one \c UdsClient singleton, which owns the ISO-TP
    transport underneath it.

    The console is the point of the page. A diagnostic session is a
    conversation, and the useful artefact is the transcript - so requests,
    responses, negative responses and (optionally) the raw ISO-TP frames all
    land in the same ordered list rather than in separate widgets.
*/
BasePage {
    id: root

    title: qsTr("Diagnostics")
    subtitle: qsTr("UDS / OBD-II over ISO-TP - read and clear diagnostic trouble codes")

    /*! Newest entry first? No: a transcript reads top to bottom, and a
        response that appears above its own request is confusing. The view
        auto-scrolls to the bottom instead. */
    property int maxConsoleRows: 500

    function stamp() {
        return Qt.formatTime(new Date(), "hh:mm:ss.zzz")
    }

    function log(kind, text) {
        consoleModel.append({ "time": root.stamp(), "kind": kind, "text": text })
        while (consoleModel.count > root.maxConsoleRows)
            consoleModel.remove(0)
        consoleList.positionViewAtEnd()
    }

    /*! Reads a hex field, returning -1 for anything that is not a clean
        hex number - never 0, which is a legitimate CAN id and would silently
        address the wrong ECU. */
    function parseHexId(text) {
        const cleaned = text.trim().replace(/^0[xX]/, "")
        if (cleaned.length === 0 || !/^[0-9a-fA-F]+$/.test(cleaned))
            return -1
        return parseInt(cleaned, 16)
    }

    function applyAddressing() {
        const tx = root.parseHexId(txIdField.text)
        const rx = root.parseHexId(rxIdField.text)
        if (tx < 0 || rx < 0) {
            root.log("error", qsTr("Tx and Rx must both be hex ids, for example 7E0 and 7E8."))
            return false
        }
        if (tx === rx) {
            root.log("error", qsTr("Tx and Rx cannot be the same id - the tester would answer itself."))
            return false
        }
        UdsClient.extendedIds = extendedBox.checked
        UdsClient.txId = tx
        UdsClient.rxId = rx
        return true
    }

    /*! Every service button funnels through here so the address fields are
        always applied before the request goes out - otherwise a user who
        edits an id and clicks straight on "Read DTCs" talks to the old ECU. */
    function run(action) {
        if (!root.applyAddressing())
            return
        action()
    }

    toolbar: RowLayout {
        spacing: Theme.spacingSm

        Label {
            text: UdsClient.busReady ? qsTr("Bus ready") : qsTr("No interface")
            color: UdsClient.busReady ? Theme.success : Theme.warning
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSm
        }
        ToolbarButton {
            text: qsTr("Clear Console")
            enabled: consoleModel.count > 0
            onClicked: consoleModel.clear()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacingSm

        // ------------------------------------------------------- addressing
        SectionLabel { text: qsTr("ECU ADDRESSING") }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingMd

            PropertyField {
                label: qsTr("Tx ID")
                labelWidth: 54
                horizontalMargin: 0
                Layout.preferredWidth: 190
                ThemedTextField {
                    id: txIdField
                    Layout.fillWidth: true
                    text: "7E0"
                    placeholderText: qsTr("7E0")
                    // Request id: the tester talks, the ECU listens.
                    validator: RegularExpressionValidator {
                        regularExpression: /(0[xX])?[0-9a-fA-F]{1,8}/
                    }
                    onEditingFinished: root.applyAddressing()
                }
            }

            PropertyField {
                label: qsTr("Rx ID")
                labelWidth: 54
                horizontalMargin: 0
                Layout.preferredWidth: 190
                ThemedTextField {
                    id: rxIdField
                    Layout.fillWidth: true
                    text: "7E8"
                    placeholderText: qsTr("7E8")
                    validator: RegularExpressionValidator {
                        regularExpression: /(0[xX])?[0-9a-fA-F]{1,8}/
                    }
                    onEditingFinished: root.applyAddressing()
                }
            }

            ThemedCheckBox {
                id: extendedBox
                text: qsTr("29-bit ids")
                onToggled: root.applyAddressing()
            }

            ThemedCheckBox {
                id: traceFramesBox
                text: qsTr("Trace ISO-TP frames")
                // Off by default: a long response is dozens of CFs, and the
                // transcript is easier to read without them until something
                // goes wrong at the transport level.
            }

            Item { Layout.fillWidth: true }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDisabled
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSm
            text: qsTr("Physical addressing, normal (non-extended) ISO-TP. "
                     + "OBD-II legislated ECUs answer 7E0/7E8 through 7E7/7EF; "
                     + "functional broadcast is 7DF.")
        }

        // ---------------------------------------------------------- services
        SectionLabel { text: qsTr("SERVICES") }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSm

            ToolbarButton {
                text: qsTr("Read DTCs")
                glyph: "⚑"
                primary: true
                enabled: UdsClient.busReady && !UdsClient.busy
                onClicked: root.run(function () { UdsClient.requestReadDtc(0xFF) })
            }
            ToolbarButton {
                text: qsTr("Clear DTCs")
                glyph: "⌫"
                destructive: true
                enabled: UdsClient.busReady && !UdsClient.busy
                // Confirmed, not immediate: this erases the freeze-frame data
                // an intermittent fault may only produce once.
                onClicked: if (root.applyAddressing()) clearConfirm.open()
            }
            ToolbarButton {
                text: qsTr("Extended Session")
                enabled: UdsClient.busReady && !UdsClient.busy
                onClicked: root.run(function () { UdsClient.diagnosticSessionControl(0x03) })
            }
            ToolbarButton {
                text: qsTr("Tester Present")
                enabled: UdsClient.busReady && !UdsClient.busy
                onClicked: root.run(function () { UdsClient.testerPresent() })
            }
            ToolbarButton {
                text: qsTr("Abort")
                enabled: UdsClient.busy
                onClicked: UdsClient.abort()
            }

            BusyIndicator {
                running: UdsClient.busy
                visible: running
                implicitWidth: Theme.inputHeight
                implicitHeight: Theme.inputHeight
            }

            Item { Layout.fillWidth: true }
        }

        // ---------------------------------------------------- custom request
        SectionLabel { text: qsTr("CUSTOM REQUEST") }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSm

            ThemedTextField {
                id: customField
                Layout.fillWidth: true
                placeholderText: qsTr("Raw service bytes in hex, e.g. 22 01 00 (ReadDataByIdentifier 0x0100)")
                font.family: Theme.monoFontFamily
                enabled: UdsClient.busReady && !UdsClient.busy
                onAccepted: sendCustom.clicked()
            }
            ToolbarButton {
                id: sendCustom
                text: qsTr("Send")
                glyph: "▸"
                primary: true
                enabled: UdsClient.busReady && !UdsClient.busy && customField.text.trim().length > 0
                onClicked: root.run(function () {
                    if (!UdsClient.sendHexRequest(customField.text))
                        root.log("error", UdsClient.lastError)
                })
            }
        }

        // ----------------------------------------------------------- console
        SectionLabel { text: qsTr("CONSOLE") }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 140
            color: Theme.background
            radius: Theme.radiusSm
            border.width: 1
            border.color: Theme.border
            clip: true

            ListView {
                id: consoleList
                anchors.fill: parent
                anchors.margins: Theme.spacingSm
                model: ListModel { id: consoleModel }
                spacing: 1
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                delegate: RowLayout {
                    id: row
                    required property string time
                    required property string kind
                    required property string text

                    width: ListView.view ? ListView.view.width : 0
                    spacing: Theme.spacingSm

                    Label {
                        text: row.time
                        color: Theme.textDisabled
                        font.family: Theme.monoFontFamily
                        font.pixelSize: Theme.fontSizeSm
                    }
                    Label {
                        Layout.preferredWidth: 30
                        text: row.kind === "tx" ? "TX"
                            : row.kind === "rx" ? "RX"
                            : row.kind === "error" ? "!!"
                            : row.kind === "dtc" ? "DTC" : "--"
                        color: row.kind === "tx" ? Theme.accent
                             : row.kind === "rx" ? Theme.success
                             : row.kind === "error" ? Theme.error
                             : row.kind === "dtc" ? Theme.warning
                                                  : Theme.textDisabled
                        font.family: Theme.monoFontFamily
                        font.pixelSize: Theme.fontSizeSm
                        font.weight: Font.DemiBold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: row.text
                        color: row.kind === "error" ? Theme.error : Theme.textPrimary
                        font.family: Theme.monoFontFamily
                        font.pixelSize: Theme.fontSizeSm
                        wrapMode: Text.WrapAnywhere
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: consoleModel.count === 0
                text: qsTr("Nothing sent yet.")
                color: Theme.textDisabled
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMd
            }
        }
    }

    // ---------------------------------------------------------------- wiring
    Connections {
        target: UdsClient

        function onRequestSent(data) {
            root.log("tx", qsTr("request  %1").arg(UdsClient.toHex(data)))
        }
        function onResponseReceived(data) {
            root.log("rx", qsTr("response %1").arg(UdsClient.toHex(data)))
        }
        function onDtcRead(dtcs) {
            if (dtcs.length === 0) {
                root.log("dtc", qsTr("No diagnostic trouble codes stored."))
                return
            }
            root.log("dtc", qsTr("%1 diagnostic trouble code(s):").arg(dtcs.length))
            for (let i = 0; i < dtcs.length; ++i)
                root.log("dtc", "    " + dtcs[i])
        }
        function onNegativeResponse(service, nrc, text) {
            root.log("error", qsTr("service 0x%1 rejected - 0x%2 %3")
                     .arg(service.toString(16).toUpperCase())
                     .arg(nrc.toString(16).toUpperCase())
                     .arg(text))
        }
        function onResponsePending(service) {
            root.log("info", qsTr("ECU asked for more time on service 0x%1...")
                     .arg(service.toString(16).toUpperCase()))
        }
        function onErrorOccurred(text) {
            root.log("error", text)
        }
    }

    /*! Transport-level tracing, off unless the user asks for it. */
    Connections {
        target: UdsClient.transport
        enabled: traceFramesBox.checked

        function onFramePassed(outgoing, kind, bytes) {
            root.log(outgoing ? "tx" : "rx", qsTr("  %1  %2").arg(kind).arg(bytes))
        }
    }

    Dialog {
        id: clearConfirm

        anchors.centerIn: Overlay.overlay
        modal: true
        width: 420
        title: qsTr("Clear diagnostic trouble codes?")

        onAccepted: UdsClient.clearDtc(0xFFFFFF)

        background: Rectangle {
            color: Theme.panel
            radius: Theme.radiusMd
            border.width: 1
            border.color: Theme.warning
        }

        header: Item {
            implicitHeight: Theme.headerHeight
            Label {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Theme.spacingMd
                anchors.rightMargin: Theme.spacingMd
                anchors.verticalCenter: parent.verticalCenter
                text: clearConfirm.title
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
            width: clearConfirm.availableWidth
            wrapMode: Text.WordWrap
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeMd
            text: qsTr("This erases every stored code on the ECU at 0x%1, along with the "
                     + "freeze-frame data recorded with them.\n\nAn intermittent fault may "
                     + "not reproduce, so anything not written down now is gone.")
                  .arg(UdsClient.txId.toString(16).toUpperCase())
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
                    text: qsTr("Cancel")
                    onClicked: clearConfirm.reject()
                }
                ToolbarButton {
                    text: qsTr("Clear Codes")
                    destructive: true
                    onClicked: clearConfirm.accept()
                }
            }
        }
    }

    // -------------------------------------------------------- inline helpers
    component SectionLabel: Label {
        Layout.fillWidth: true
        Layout.topMargin: Theme.spacingXs
        color: Theme.textDisabled
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeSm
        font.letterSpacing: 1.1
        font.weight: Font.DemiBold
    }
}
