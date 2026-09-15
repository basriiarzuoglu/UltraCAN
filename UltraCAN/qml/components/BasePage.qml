import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import UltraCAN.Theme

/*!
    \qmltype BasePage
    \brief Generic page template every module view derives from.

    Gives all modules an identical skeleton: a header (title, subtitle, an
    optional toolbar slot) and a content area. Module views only fill in the
    slots, so the shell stays visually consistent as the app grows.

    \code
    BasePage {
        title: qsTr("CAN Logger")
        toolbar: RowLayout { Button { text: "Start" } }
        Rectangle { ... }   // default content
    }
    \endcode
*/
Item {
    id: root

    /*! Page heading. */
    property string title: ""
    /*! Optional one-line explanation under the heading. */
    property string subtitle: ""
    /*! Placeholder text shown while the module has no implementation yet. */
    property string placeholderText: qsTr("This module is not implemented yet.")

    /*! Slot for page-specific action buttons, rendered on the header's right side. */
    property Component toolbar: null

    /*! Padding around the content area. Set to 0 for edge-to-edge editors. */
    property int contentMargins: Theme.spacingLg

    /*! Default property: anything declared inside a BasePage lands in the content area. */
    default property alias content: contentArea.data

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ------------------------------------------------------------ header
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.headerHeight + Theme.spacingMd
            color: Theme.surface

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingLg
                anchors.rightMargin: Theme.spacingLg
                spacing: Theme.spacingLg

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Label {
                        text: root.title
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeXl
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        visible: root.subtitle.length > 0
                        text: root.subtitle
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }

                // Page-specific actions.
                Loader {
                    sourceComponent: root.toolbar
                    Layout.alignment: Qt.AlignVCenter
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        // ----------------------------------------------------------- content
        Item {
            id: contentArea
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: root.contentMargins

            // Shown only while the module has no real content yet.
            ColumnLayout {
                anchors.centerIn: parent
                spacing: Theme.spacingSm
                visible: contentArea.children.length === 1 // just this placeholder

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: "⚙"
                    color: Theme.textDisabled
                    font.pixelSize: 48
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: root.placeholderText
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeMd
                }
            }
        }
    }
}
