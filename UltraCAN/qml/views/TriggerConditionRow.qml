import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import UltraCAN.Theme
import UltraCAN.Components

/*!
    \qmltype TriggerConditionRow
    \brief One condition block - kind dropdown plus the fields that kind needs.

    Start and stop are the same form, so they are the same component. The
    fields are shown and hidden rather than rebuilt, so switching kind and
    switching back does not lose what was typed.
*/
ColumnLayout {
    id: root

    property string title: ""
    property string hint: ""
    property var kindLabels: []
    property var comparisonLabels: []
    property var signalNames: []

    /*! TriggerCondition::Kind - 0 none, 1 id match, 2 signal value. */
    property int kind: 0
    property string idText: "0x100"
    property bool extended: false
    property int signalIndex: -1
    property int comparison: 0
    property string thresholdText: "0"

    spacing: Theme.spacingXs

    Rectangle {
        Layout.fillWidth: true
        Layout.topMargin: Theme.spacingSm
        Layout.preferredHeight: 1
        color: Theme.border
    }

    Label {
        text: root.title
        color: Theme.textDisabled
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeSm
        font.letterSpacing: 1.1
    }

    ThemedComboBox {
        Layout.fillWidth: true
        model: root.kindLabels
        currentIndex: root.kind
        onActivated: root.kind = currentIndex
    }

    // -- ID match
    RowLayout {
        Layout.fillWidth: true
        visible: root.kind === 1
        spacing: Theme.spacingSm

        Label {
            Layout.preferredWidth: 74
            text: qsTr("CAN ID")
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSm
        }
        ThemedTextField {
            Layout.preferredWidth: 120
            text: root.idText
            placeholderText: "0x100"
            font.family: Theme.monoFontFamily
            onTextChanged: root.idText = text
        }
        ThemedCheckBox {
            text: qsTr("29-bit")
            checked: root.extended
            onToggled: root.extended = checked
        }
        Item { Layout.fillWidth: true }
    }

    // -- signal value
    ColumnLayout {
        Layout.fillWidth: true
        visible: root.kind === 2
        spacing: Theme.spacingXs

        ThemedComboBox {
            Layout.fillWidth: true
            enabled: root.signalNames.length > 0
            model: root.signalNames.length > 0 ? root.signalNames
                                               : [qsTr("No signals - load a DBC")]
            currentIndex: root.signalIndex
            onActivated: root.signalIndex = currentIndex
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSm

            ThemedComboBox {
                Layout.preferredWidth: 76
                model: root.comparisonLabels
                currentIndex: root.comparison
                onActivated: root.comparison = currentIndex
            }
            ThemedTextField {
                Layout.preferredWidth: 120
                text: root.thresholdText
                placeholderText: "0"
                font.family: Theme.monoFontFamily
                onTextChanged: root.thresholdText = text
            }
            Label {
                text: qsTr("(physical value, after scale and offset)")
                color: Theme.textDisabled
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSm
            }
            Item { Layout.fillWidth: true }
        }
    }

    Label {
        Layout.fillWidth: true
        visible: root.hint.length > 0
        text: root.hint
        color: Theme.textDisabled
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeSm
        wrapMode: Text.WordWrap
    }
}
