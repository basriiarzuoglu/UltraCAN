import QtQuick
import QtQuick.Controls.Basic

import UltraCAN.Theme

/*!
    \qmltype ThemedCheckBox
    \brief Compact check box used by the plot signal picker and future filters.

    Set \l indicatorColor to tint the box with a trace colour, which lets the
    picker double as the chart legend.
*/
CheckBox {
    id: control

    /*! Fill colour when checked. Defaults to the theme accent. */
    property color indicatorColor: Theme.accent

    implicitHeight: Theme.rowHeight
    spacing: Theme.spacingSm
    hoverEnabled: true
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSizeMd

    indicator: Rectangle {
        implicitWidth: 14
        implicitHeight: 14
        x: control.leftPadding
        y: (control.height - height) / 2
        radius: Theme.radiusSm
        color: control.checked ? control.indicatorColor : Theme.background
        border.width: 1
        border.color: control.checked ? control.indicatorColor
                                      : (control.hovered ? Theme.accent : Theme.border)
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

        Label {
            anchors.centerIn: parent
            visible: control.checked
            text: "✓"
            color: Theme.background
            font.pixelSize: 11
            font.weight: Font.Bold
        }
    }

    contentItem: Label {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        color: control.enabled ? Theme.textPrimary : Theme.textDisabled
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
