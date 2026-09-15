import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import UltraCAN.Theme

/*!
    \qmltype PropertyField
    \brief One labelled row of a property form: [ Label ][ editor(s) ].

    The editor is a default slot, so any control - or several - can be placed
    inside and laid out with the usual Layout attached properties:

    \code
    PropertyField {
        label: qsTr("Start Bit")
        ThemedSpinBox { Layout.fillWidth: true; from: 0; to: 63 }
    }

    PropertyField {
        label: qsTr("Range")
        ThemedTextField { Layout.fillWidth: true }   // min
        ThemedTextField { Layout.fillWidth: true }   // max
    }
    \endcode
*/
RowLayout {
    id: root

    /*! Text shown in the left label column. */
    property string label: ""
    /*! Width of the label column; keep identical across one form for alignment. */
    property int labelWidth: Theme.labelColumnWidth
    /*! Optional unit / hint rendered after the editor (e.g. "rpm", "bits"). */
    property string suffix: ""
    /*! Marks the row as mandatory (drawn with an accent dot). */
    property bool required_: false
    /*! Page gutter; rows align with the section captions by default. */
    property int horizontalMargin: Theme.spacingLg

    Layout.fillWidth: true
    Layout.leftMargin: horizontalMargin
    Layout.rightMargin: horizontalMargin
    spacing: Theme.spacingMd

    RowLayout {
        Layout.preferredWidth: root.labelWidth
        Layout.alignment: Qt.AlignTop
        Layout.topMargin: (Theme.inputHeight - labelItem.implicitHeight) / 2
        spacing: Theme.spacingXs

        Label {
            id: labelItem
            Layout.fillWidth: true
            text: root.label
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeMd
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignRight
        }
        Rectangle {
            visible: root.required_
            Layout.alignment: Qt.AlignVCenter
            width: 4; height: 4; radius: 2
            color: Theme.accent
        }
    }

    // --- editor slot ---------------------------------------------------------
    RowLayout {
        id: editorRow
        Layout.fillWidth: true
        spacing: Theme.spacingSm
    }

    Label {
        visible: root.suffix.length > 0
        Layout.preferredWidth: 36
        Layout.alignment: Qt.AlignVCenter
        text: root.suffix
        color: Theme.textDisabled
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeSm
    }

    /*! Children declared inside a PropertyField land in the editor slot. */
    default property alias editor: editorRow.data
}
