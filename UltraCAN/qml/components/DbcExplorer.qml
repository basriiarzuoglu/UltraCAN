import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import UltraCAN.Theme

/*!
    \qmltype DbcExplorer
    \brief Message/signal hierarchy browser shared by the DBC Editor and the
           DBC Viewer.

    Model-agnostic on purpose (Components must never import Models): hand it any
    ListModel whose elements expose \c name, \c canId, \c expanded and a nested
    \c signalList of elements exposing \c name, \c startBit and \c bitLength.

    Selection is *reported*, never owned - the host page decides what a click
    means and feeds \l selectedMessage / \l selectedSignal back in.

    \note A real QtQuick TreeView needs a QAbstractItemModel from C++. Until the
          parser lands, the hierarchy is a ListView of messages, each expanding
          into a Repeater of its signals - same visual result, pure QML.
*/
Rectangle {
    id: root

    /*! The messages ListModel. */
    property var messages: null
    /*! Currently highlighted message, -1 for none. */
    property int selectedMessage: -1
    /*! Currently highlighted signal within \l selectedMessage, -1 when the message itself is selected. */
    property int selectedSignal: -1
    /*! Caption of the pane header. */
    property string headerText: qsTr("NETWORK")
    /*! Show the filter box. */
    property bool searchEnabled: true
    /*! Live filter text (also settable from outside). */
    property string filterText: ""

    signal messageActivated(int messageIndex)
    signal signalActivated(int messageIndex, int signalIndex)

    color: Theme.panel
    clip: true

    // ------------------------------------------------------------ behaviour
    function setExpandedAll(value) {
        if (!messages)
            return
        for (let i = 0; i < messages.count; ++i)
            messages.setProperty(i, "expanded", value)
    }

    function expandAll()   { setExpandedAll(true) }
    function collapseAll() { setExpandedAll(false) }

    function toggleExpanded(messageIndex) {
        if (!messages)
            return
        messages.setProperty(messageIndex, "expanded", !messages.get(messageIndex).expanded)
    }

    function formatCanId(id) {
        return "0x" + Number(id).toString(16).toUpperCase().padStart(3, "0")
    }

    // --------------------------------------------------------------- filter
    function messageVisible(messageIndex) {
        if (root.filterText.length === 0)
            return true
        const needle = root.filterText.toLowerCase()
        const msg = messages ? messages.get(messageIndex) : null
        if (!msg)
            return false
        if (msg.name.toLowerCase().indexOf(needle) !== -1)
            return true
        for (let i = 0; i < msg.signalList.count; ++i)
            if (msg.signalList.get(i).name.toLowerCase().indexOf(needle) !== -1)
                return true
        return false
    }

    function signalVisible(messageIndex, signalIndex) {
        if (root.filterText.length === 0)
            return true
        const needle = root.filterText.toLowerCase()
        const msg = messages ? messages.get(messageIndex) : null
        if (!msg)
            return false
        if (msg.name.toLowerCase().indexOf(needle) !== -1)
            return true
        const sig = msg.signalList.get(signalIndex)
        return sig ? sig.name.toLowerCase().indexOf(needle) !== -1 : false
    }

    // ----------------------------------------------------------------- view
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
                    text: root.headerText
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    font.letterSpacing: 1.2
                }
                Label {
                    text: qsTr("%1 msg").arg(root.messages ? root.messages.count : 0)
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                }
            }
        }

        ThemedTextField {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingSm
            Layout.rightMargin: Theme.spacingSm
            Layout.bottomMargin: Theme.spacingSm
            visible: root.searchEnabled
            placeholderText: qsTr("Filter messages and signals...")
            onTextChanged: root.filterText = text
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        ListView {
            id: treeView

            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.messages
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Column {
                id: messageDelegate

                required property int index
                required property var model

                width: ListView.view.width
                visible: root.messageVisible(index)
                height: visible ? implicitHeight : 0

                // ---------- message row
                Rectangle {
                    width: parent.width
                    height: Theme.rowHeight + 4
                    color: root.selectedMessage === messageDelegate.index && root.selectedSignal < 0
                           ? Theme.selection
                           : (messageMouse.containsMouse ? Theme.panelElevated : "transparent")

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacingSm
                        anchors.rightMargin: Theme.spacingSm
                        spacing: Theme.spacingXs

                        Label {
                            Layout.preferredWidth: 14
                            text: messageDelegate.model.expanded ? "▾" : "▸"
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSm
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Label {
                            text: "✉"
                            color: Theme.accent
                            font.pixelSize: Theme.fontSizeMd
                        }
                        Label {
                            Layout.fillWidth: true
                            text: messageDelegate.model.name
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeMd
                            font.weight: Font.DemiBold      // messages are bold
                            elide: Text.ElideRight
                        }
                        Label {
                            text: root.formatCanId(messageDelegate.model.canId)
                            color: Theme.textDisabled
                            font.family: Theme.monoFontFamily
                            font.pixelSize: Theme.fontSizeSm
                        }
                    }

                    MouseArea {
                        id: messageMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: (mouse) => {
                            // Hitting the chevron area toggles, anywhere else selects.
                            if (mouse.x < Theme.spacingSm + 18)
                                root.toggleExpanded(messageDelegate.index)
                            else
                                root.messageActivated(messageDelegate.index)
                        }
                        onDoubleClicked: root.toggleExpanded(messageDelegate.index)
                    }
                }

                // ---------- signal rows
                Repeater {
                    model: messageDelegate.model.expanded ? messageDelegate.model.signalList : null

                    delegate: Rectangle {
                        id: signalDelegate

                        required property int index
                        required property var model

                        width: messageDelegate.width
                        height: visible ? Theme.rowHeight : 0
                        visible: root.signalVisible(messageDelegate.index, index)
                        color: root.selectedMessage === messageDelegate.index
                               && root.selectedSignal === signalDelegate.index
                               ? Theme.selection
                               : (signalMouse.containsMouse ? Theme.panelElevated : "transparent")

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spacingSm + Theme.treeIndent   // indentation
                            anchors.rightMargin: Theme.spacingSm
                            spacing: Theme.spacingXs

                            Label {
                                text: "•"
                                color: Theme.textDisabled
                                font.pixelSize: Theme.fontSizeMd
                            }
                            Label {
                                Layout.fillWidth: true
                                text: signalDelegate.model.name
                                color: Theme.textSecondary   // signals are lighter + indented
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeMd
                                elide: Text.ElideRight
                            }
                            Label {
                                text: qsTr("%1|%2").arg(signalDelegate.model.startBit)
                                                   .arg(signalDelegate.model.bitLength)
                                color: Theme.textDisabled
                                font.family: Theme.monoFontFamily
                                font.pixelSize: Theme.fontSizeSm
                            }
                        }

                        MouseArea {
                            id: signalMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: root.signalActivated(messageDelegate.index, signalDelegate.index)
                        }
                    }
                }
            }
        }
    }
}
