import QtQuick
import QtQuick.Controls.Basic

import UltraCAN.Theme

/*!
    \qmltype ThemedSpinBox
    \brief Integer spin box (bit positions, lengths, DLC, CAN id ...).

    \note SpinBox is integer-only by design. For real-valued DBC fields
          (factor, offset, min, max) use a ThemedTextField with a
          DoubleValidator whose locale is "C" - see DbcEditorView.
*/
SpinBox {
    id: control

    implicitHeight: Theme.inputHeight
    editable: true
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSizeMd

    contentItem: TextInput {
        text: control.displayText
        color: control.enabled ? Theme.textPrimary : Theme.textDisabled
        selectionColor: Theme.accent
        selectedTextColor: Theme.background
        selectByMouse: true
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: Qt.ImhDigitsOnly
        font: control.font
        horizontalAlignment: Qt.AlignLeft
        verticalAlignment: Qt.AlignVCenter
        leftPadding: Theme.spacingSm
    }

    background: Rectangle {
        color: control.enabled ? Theme.background : Theme.panel
        radius: Theme.radiusSm
        border.width: 1
        border.color: control.activeFocus ? Theme.accent : Theme.border
        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
    }

    up.indicator: Rectangle {
        x: control.width - width - 1
        y: 1
        height: control.height / 2 - 1
        implicitWidth: 20
        color: control.up.pressed ? Theme.accent
                                  : (control.up.hovered ? Theme.panelElevated : "transparent")
        Label {
            anchors.centerIn: parent
            text: "+"
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeMd
        }
    }

    down.indicator: Rectangle {
        x: control.width - width - 1
        y: control.height / 2
        height: control.height / 2 - 1
        implicitWidth: 20
        color: control.down.pressed ? Theme.accent
                                    : (control.down.hovered ? Theme.panelElevated : "transparent")
        Label {
            anchors.centerIn: parent
            text: "-"
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeMd
        }
    }
}
