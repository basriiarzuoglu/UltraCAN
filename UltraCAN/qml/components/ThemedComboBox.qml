import QtQuick
import QtQuick.Controls.Basic

import UltraCAN.Theme

/*!
    \qmltype ThemedComboBox
    \brief Drop-down selector (endianness, value type, transmitter ...).
*/
ComboBox {
    id: control

    implicitHeight: Theme.inputHeight
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSizeMd

    contentItem: Label {
        leftPadding: Theme.spacingSm
        rightPadding: control.indicator.width + Theme.spacingSm
        text: control.displayText
        color: control.enabled ? Theme.textPrimary : Theme.textDisabled
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Label {
        x: control.width - width - Theme.spacingSm
        y: (control.height - height) / 2
        text: "▾"
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSizeMd
    }

    background: Rectangle {
        color: control.enabled ? Theme.background : Theme.panel
        radius: Theme.radiusSm
        border.width: 1
        border.color: control.activeFocus || control.popup.visible ? Theme.accent
                                                                   : Theme.border
        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
    }

    delegate: ItemDelegate {
        required property var model
        required property int index

        width: control.width
        height: Theme.rowHeight
        highlighted: control.highlightedIndex === index

        contentItem: Label {
            text: model[control.textRole] !== undefined ? model[control.textRole] : model.modelData
            color: parent.highlighted ? Theme.textPrimary : Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeMd
            verticalAlignment: Text.AlignVCenter
            leftPadding: Theme.spacingSm
        }

        background: Rectangle {
            color: parent.highlighted ? Theme.selection : "transparent"
        }
    }

    popup: Popup {
        y: control.height
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight, 240)
        padding: 1

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
        }

        background: Rectangle {
            color: Theme.panel
            radius: Theme.radiusSm
            border.width: 1
            border.color: Theme.border
        }
    }
}
