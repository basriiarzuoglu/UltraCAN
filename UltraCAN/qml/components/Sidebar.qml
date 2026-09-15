import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import UltraCAN.Theme

/*!
    \qmltype Sidebar
    \brief Left-hand navigation rail.

    Deliberately model-agnostic: it renders one NavButton per entry of \l model
    and reports selection through \l routeSelected. It does not import the
    Navigation singleton, so Components never depends on Models - the shell
    wires the two together. It never loads or knows about the pages themselves.
*/
Rectangle {
    id: root

    /*! List of route objects (title / icon / description). */
    property var model: []

    /*! Index of the highlighted entry. */
    property int currentIndex: 0

    /*! Emitted when the user picks a destination. */
    signal routeSelected(int index)

    /*! Session file actions. Signals rather than direct calls, because this
        type is deliberately model-agnostic - it does not import UltraCAN.Core
        and must not learn that a ConfigManager exists. The shell owns the file
        dialogs and does the wiring, exactly as it does for routeSelected. */
    signal exportRequested()
    signal importRequested()

    /*! Collapsed (icon-only) mode - useful on narrow windows. */
    property bool collapsed: false

    implicitWidth: collapsed ? Theme.sidebarCollapsedWidth : Theme.sidebarWidth
    Layout.fillHeight: true
    color: Theme.panel
    clip: true

    Behavior on implicitWidth {
        NumberAnimation { duration: Theme.durationNormal; easing.type: Easing.OutCubic }
    }

    // Right hairline separating the rail from the content area.
    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ------------------------------------------------------------- brand
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.headerHeight

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingLg
                anchors.rightMargin: Theme.spacingSm
                spacing: Theme.spacingSm

                Label {
                    text: "UltraCAN"
                    visible: !root.collapsed
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeLg
                    font.weight: Font.Bold
                }

                Item { Layout.fillWidth: true }

                // Collapse / expand toggle.
                AbstractButton {
                    implicitWidth: 28
                    implicitHeight: 28
                    hoverEnabled: true
                    onClicked: root.collapsed = !root.collapsed
                    background: Rectangle {
                        radius: Theme.radiusSm
                        color: parent.hovered ? Theme.panelElevated : "transparent"
                    }
                    contentItem: Label {
                        text: root.collapsed ? "»" : "«"
                        color: Theme.textSecondary
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: Theme.fontSizeLg
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        Item { Layout.preferredHeight: Theme.spacingSm }

        Label {
            Layout.leftMargin: Theme.spacingLg
            Layout.bottomMargin: Theme.spacingXs
            visible: !root.collapsed
            text: qsTr("MODULES")
            color: Theme.textDisabled
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSm
            font.letterSpacing: 1.2
        }

        // -------------------------------------------------------- nav entries
        Repeater {
            model: root.model

            delegate: NavButton {
                required property int index
                required property var modelData

                text: modelData.title
                glyph: modelData.icon
                collapsed: root.collapsed
                active: root.currentIndex === index
                ToolTip.text: root.collapsed ? modelData.title : modelData.description
                onClicked: root.routeSelected(index)
            }
        }

        Item { Layout.fillHeight: true } // pushes the footer down

        // ------------------------------------------------------------ session
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        Item { Layout.preferredHeight: Theme.spacingSm }

        Label {
            Layout.leftMargin: Theme.spacingLg
            Layout.bottomMargin: Theme.spacingXs
            visible: !root.collapsed
            text: qsTr("SESSION")
            color: Theme.textDisabled
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSm
            font.letterSpacing: 1.2
        }

        // Same NavButton as the routes above, with active permanently false:
        // these are actions, not destinations, so nothing here ever stays lit.
        // Reusing the type is what keeps the collapsed rail coherent - both
        // rows shrink to their glyph and grow the same tooltip.
        NavButton {
            text: qsTr("Export Config")
            glyph: "⭳"
            collapsed: root.collapsed
            active: false
            ToolTip.text: qsTr("Save databases, analyzer setup and the graph workspace to a file")
            onClicked: root.exportRequested()
        }

        NavButton {
            text: qsTr("Import Config")
            glyph: "⭱"
            collapsed: root.collapsed
            active: false
            ToolTip.text: qsTr("Replace the current session with one saved earlier")
            onClicked: root.importRequested()
        }

        Item { Layout.preferredHeight: Theme.spacingSm }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        Label {
            Layout.fillWidth: true
            Layout.margins: Theme.spacingMd
            visible: !root.collapsed
            text: qsTr("v%1").arg(Qt.application.version)
            color: Theme.textDisabled
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSm
        }
    }
}
