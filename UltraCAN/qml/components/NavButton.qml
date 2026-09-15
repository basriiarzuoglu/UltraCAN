import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import UltraCAN.Theme

/*!
    \qmltype NavButton
    \brief A single sidebar navigation entry (icon + label + active marker).

    Purely presentational: it knows nothing about routing, it only reports
    clicks. The Sidebar decides what a click means.
*/
AbstractButton {
    id: control

    property string glyph: ""
    property bool active: false
    property bool collapsed: false

    implicitHeight: Theme.navItemHeight
    Layout.fillWidth: true
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    background: Rectangle {
        color: control.active ? Theme.panelElevated
                              : (control.hovered ? Qt.lighter(Theme.panel, 1.25) : "transparent")
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

        // Left accent bar marks the active destination.
        Rectangle {
            width: 3
            height: parent.height
            color: Theme.accent
            opacity: control.active ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
        }
    }

    contentItem: RowLayout {
        spacing: Theme.spacingMd

        Label {
            Layout.leftMargin: Theme.spacingLg
            Layout.preferredWidth: 16
            horizontalAlignment: Text.AlignHCenter
            text: control.glyph
            color: control.active ? Theme.accent : Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeLg
        }

        Label {
            Layout.fillWidth: true
            Layout.rightMargin: Theme.spacingMd
            visible: !control.collapsed
            opacity: control.collapsed ? 0 : 1
            text: control.text
            elide: Text.ElideRight
            color: control.active ? Theme.textPrimary : Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeMd
            font.weight: control.active ? Font.DemiBold : Font.Normal
            Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
        }
    }

    ToolTip.visible: control.collapsed && control.hovered
    ToolTip.text: control.text
    ToolTip.delay: 400
}
