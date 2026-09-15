import QtQuick
import QtQuick.Controls.Basic

import UltraCAN.Theme

/*!
    \qmltype ThemedSlider
    \brief Continuous value picker used for DBC signal values.

    Pair it with a numeric readout - a slider alone is never precise enough for
    engineering values.
*/
Slider {
    id: control

    implicitHeight: Theme.inputHeight
    hoverEnabled: true

    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: control.availableWidth
        height: 4
        radius: 2
        color: Theme.background
        border.width: 1
        border.color: Theme.border

        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: 2
            color: control.enabled ? Theme.accent : Theme.textDisabled
        }
    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        implicitWidth: 14
        implicitHeight: 14
        radius: 7
        color: control.pressed ? Theme.accentPressed : Theme.textPrimary
        border.width: 1
        border.color: control.hovered || control.pressed ? Theme.accent : Theme.border
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
    }
}
