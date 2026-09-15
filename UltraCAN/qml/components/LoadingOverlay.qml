import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import UltraCAN.Theme

/*!
    \qmltype LoadingOverlay
    \brief Modal-looking busy veil for long operations (parse, save, replay).

    Swallows input while \l active so the user cannot mutate a document that is
    being read or written.
*/
Rectangle {
    id: root

    /*! Show the veil and block input. */
    property bool active: false
    /*! Line under the spinner. */
    property string message: qsTr("Working...")

    anchors.fill: parent
    visible: opacity > 0
    opacity: active ? 1 : 0
    color: Qt.rgba(0, 0, 0, 0.55)
    z: 100

    Behavior on opacity { NumberAnimation { duration: Theme.durationNormal } }

    MouseArea {
        anchors.fill: parent
        enabled: root.active
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    ColumnLayout {
        anchors.centerIn: parent
        spacing: Theme.spacingMd

        BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            running: root.active
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: root.message
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeMd
        }
    }
}
