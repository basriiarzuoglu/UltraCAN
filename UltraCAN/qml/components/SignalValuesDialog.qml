import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

import UltraCAN.Theme

/*!
    \qmltype SignalValuesDialog
    \brief Editor for one signal's VAL_ table: raw value -> display text.

    \section2 Why it edits a copy

    The dialog has a Cancel button, so it cannot write straight through to the
    signal: half-finished edits would already be in the document by the time
    the user changed their mind, and undoing them would mean remembering the
    original - which is just this copy, kept in the wrong place.

    So \l open() snapshots \c{signalObject.valueDescriptions} into a local
    ListModel, every edit touches only that, and OK writes the whole list back
    in one call. The C++ side takes a whole-list replacement for exactly this
    reason, and drops entries whose value is not an integer, so a row the user
    started and abandoned costs nothing.

    \section2 Duplicate values

    Two rows with the same value are a table with one meaning and two labels.
    Rather than silently dropping one at commit time (the map would keep
    whichever came last), duplicates are flagged live and OK is disabled while
    any exist.
*/
Dialog {
    id: root

    /*! The DbcSignalObject being edited. Set by open(). */
    property var signalObject: null
    /*! Shown in the header so the user knows which signal they are editing. */
    property string signalName: ""

    title: qsTr("Signal Values")
    modal: true
    closePolicy: Popup.CloseOnEscape
    padding: Theme.spacingMd

    implicitWidth: 420
    implicitHeight: 460

    /*!
        Loads \a target's table into the scratch model and shows the dialog.
        Always call this rather than open(): a plain open() would re-display
        whatever the previous signal left behind.
    */
    function openFor(target, name) {
        root.signalObject = target
        root.signalName = name || ""
        entries.clear()
        if (target) {
            const existing = target.valueDescriptions
            for (let i = 0; i < existing.length; ++i) {
                entries.append({ value: existing[i].value,
                                 description: existing[i].description })
            }
        }
        table.currentIndex = entries.count > 0 ? 0 : -1
        root.open()
    }

    /*! How many rows share their value with an earlier row. Recomputed through
        a tick rather than a binding on the ListModel, because ListModel is not
        a property source - editing a row does not invalidate a binding that
        read it. */
    property int revision: 0
    readonly property int duplicateCount: {
        root.revision                       // dependency, deliberately unused
        let seen = ({})
        let dupes = 0
        for (let i = 0; i < entries.count; ++i) {
            const v = entries.get(i).value
            if (seen[v] === true)
                ++dupes
            seen[v] = true
        }
        return dupes
    }

    ListModel { id: entries }

    background: Rectangle {
        color: Theme.panel
        radius: Theme.radiusMd
        border.width: 1
        border.color: Theme.border
    }

    header: Item {
        implicitHeight: Theme.headerHeight
        Label {
            anchors.left: parent.left
            anchors.leftMargin: Theme.spacingMd
            anchors.verticalCenter: parent.verticalCenter
            text: root.signalName.length > 0
                  ? qsTr("Signal Values - %1").arg(root.signalName)
                  : qsTr("Signal Values")
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeLg
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.border
        }
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingSm

        // ------------------------------------------------------- add / remove
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSm

            Label {
                Layout.fillWidth: true
                text: qsTr("Raw value to display text")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSm
            }

            ToolbarButton {
                text: qsTr("+")
                primary: true
                implicitWidth: Theme.inputHeight
                onClicked: {
                    // Start at the first unused value so the common case -
                    // adding 0, 1, 2, ... in order - never lands on a duplicate.
                    let candidate = 0
                    let used = ({})
                    for (let i = 0; i < entries.count; ++i)
                        used[entries.get(i).value] = true
                    while (used[candidate] === true)
                        ++candidate
                    entries.append({ value: candidate, description: "" })
                    table.currentIndex = entries.count - 1
                    ++root.revision
                }
            }

            ToolbarButton {
                text: qsTr("−")          // minus sign, not a hyphen
                destructive: true
                implicitWidth: Theme.inputHeight
                enabled: table.currentIndex >= 0 && table.currentIndex < entries.count
                onClicked: {
                    const row = table.currentIndex
                    entries.remove(row)
                    table.currentIndex = Math.min(row, entries.count - 1)
                    ++root.revision
                }
            }
        }

        // -------------------------------------------------------- column head
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.rowHeight
            color: Theme.panelElevated

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingSm
                anchors.rightMargin: Theme.spacingSm
                spacing: Theme.spacingSm

                Label {
                    Layout.preferredWidth: 90
                    text: qsTr("Value")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    font.letterSpacing: 1.1
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Description")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    font.letterSpacing: 1.1
                }
            }
        }

        // -------------------------------------------------------------- table
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.background
            border.width: 1
            border.color: Theme.border
            clip: true

            ListView {
                id: table
                anchors.fill: parent
                anchors.margins: 1
                model: entries
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                currentIndex: -1

                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Rectangle {
                    id: rowItem

                    required property int index
                    required property int value
                    required property string description

                    width: ListView.view.width
                    height: Theme.inputHeight + Theme.spacingXs

                    readonly property bool current: rowItem.index === table.currentIndex
                    /*! An earlier row already claimed this value. */
                    readonly property bool duplicate: {
                        root.revision
                        for (let i = 0; i < rowItem.index && i < entries.count; ++i) {
                            if (entries.get(i).value === rowItem.value)
                                return true
                        }
                        return false
                    }

                    color: current ? Theme.selection : "transparent"

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        onClicked: table.currentIndex = rowItem.index
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacingSm
                        anchors.rightMargin: Theme.spacingSm
                        spacing: Theme.spacingSm

                        ThemedTextField {
                            Layout.preferredWidth: 90
                            text: rowItem.value
                            horizontalAlignment: Text.AlignRight
                            color: rowItem.duplicate ? Theme.error : Theme.textPrimary
                            // Negative codes are legal in a VAL_ table, so the
                            // validator must allow a leading minus.
                            validator: IntValidator {}
                            onActiveFocusChanged: if (!activeFocus) commit()
                            onAccepted: commit()

                            function commit() {
                                const parsed = parseInt(text, 10)
                                if (isNaN(parsed)) {
                                    text = Qt.binding(function() { return rowItem.value })
                                    return
                                }
                                entries.setProperty(rowItem.index, "value", parsed)
                                ++root.revision
                            }

                            ToolTip.visible: rowItem.duplicate && hovered
                            ToolTip.text: qsTr("Another row already maps this value")
                        }

                        ThemedTextField {
                            Layout.fillWidth: true
                            text: rowItem.description
                            placeholderText: qsTr("Description")
                            onTextEdited: entries.setProperty(rowItem.index,
                                                              "description", text)
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: Theme.border
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: entries.count === 0
                text: qsTr("No value descriptions.\nUse + to add one.")
                horizontalAlignment: Text.AlignHCenter
                color: Theme.textDisabled
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMd
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.duplicateCount > 0
            text: qsTr("%1 duplicate value(s) - each value may appear once.")
                  .arg(root.duplicateCount)
            color: Theme.error
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSm
            wrapMode: Text.WordWrap
        }
    }

    footer: Item {
        implicitHeight: Theme.headerHeight

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.border
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spacingMd
            anchors.rightMargin: Theme.spacingMd
            spacing: Theme.spacingSm

            Item { Layout.fillWidth: true }

            ToolbarButton {
                text: qsTr("Cancel")
                onClicked: root.reject()
            }
            ToolbarButton {
                text: qsTr("OK")
                primary: true
                enabled: root.duplicateCount === 0
                onClicked: root.accept()
            }
        }
    }

    onAccepted: {
        if (!root.signalObject)
            return
        // One write for the whole table. Committing row by row would leave the
        // document in a half-edited state if anything below threw.
        let out = []
        for (let i = 0; i < entries.count; ++i) {
            out.push({ value: entries.get(i).value,
                       description: entries.get(i).description })
        }
        root.signalObject.valueDescriptions = out
    }
}
