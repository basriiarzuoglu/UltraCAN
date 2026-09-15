import QtQuick
import QtQuick.Controls.Basic

import UltraCAN.Theme

/*!
    \qmltype ThemedSwitch
    \brief Compact on/off toggle (cyclic transmit enable, future channel arming).
*/
Switch {
    id: control

    implicitHeight: 20
    padding: 0
    hoverEnabled: true

    indicator: Rectangle {
        implicitWidth: 34
        implicitHeight: 18
        x: control.leftPadding
        y: (control.height - height) / 2
        radius: height / 2
        color: control.checked ? Theme.accent : Theme.background
        border.width: 1
        border.color: control.checked ? Theme.accent
                                      : (control.hovered ? Theme.panelElevated : Theme.border)
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

        Rectangle {
            x: control.checked ? parent.width - width - 2 : 2
            y: 2
            width: 14
            height: 14
            radius: 7
            color: control.checked ? Theme.background : Theme.textDisabled
            Behavior on x { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }
            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
        }
    }

    contentItem: Label {
        visible: control.text.length > 0
        leftPadding: control.indicator.width + Theme.spacingSm
        text: control.text
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeMd
        verticalAlignment: Text.AlignVCenter
    }
}
