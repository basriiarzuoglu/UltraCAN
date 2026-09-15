import QtQuick
import QtQuick.Controls.Basic

import UltraCAN.Theme

/*!
    \qmltype ThemedTextField
    \brief Single-line text input styled from the Theme tokens.

    Use this instead of a bare TextField anywhere in the app so that inputs
    stay visually identical and contain zero colour literals.
*/
TextField {
    id: control

    implicitHeight: Theme.inputHeight
    leftPadding: Theme.spacingSm
    rightPadding: Theme.spacingSm
    selectByMouse: true

    color: enabled ? Theme.textPrimary : Theme.textDisabled
    placeholderTextColor: Theme.textDisabled
    selectionColor: Theme.accent
    selectedTextColor: Theme.background

    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSizeMd

    background: Rectangle {
        color: control.enabled ? Theme.background : Theme.panel
        radius: Theme.radiusSm
        border.width: 1
        border.color: control.activeFocus ? Theme.accent
                                          : (control.hovered ? Theme.panelElevated : Theme.border)
        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
    }
}
