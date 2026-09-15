import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import UltraCAN.Theme
import UltraCAN.Components
import UltraCAN.Core

/*!
    \qmltype SignalPickerDialog
    \brief Search-and-pick over every signal in the Graph Plot's loaded databases.

    \section1 Why a dialog and not a cascading menu

    A cascading DBC -> message -> signal menu is the obvious reading of "pick a
    signal", and it collapses on contact with a real database: DMC.dbc alone
    carries a few hundred signals, so the third level is a scrolling column
    taller than the screen that you navigate by holding the mouse steady. A
    search box finds "steer" in two keystrokes and still shows the full tree
    when the field is empty, grouped by message.

    The grouping is done here rather than in C++ because the C++ side already
    returns a flat, filtered, capped list - the cheapest thing to hand across -
    and turning a sorted flat list into headed sections is four lines of QML.
*/
Popup {
    id: root

    /*! Which pane the chosen signal is plotted on. */
    property string paneId: ""
    /*! Emitted once the user commits a choice. */
    signal signalChosen(int dbcIndex, int messageIndex, int signalIndex)
    /*! Emitted when the choice is a math channel rather than a decoded signal. */
    signal mathChannelChosen(int channelId)

    /*! Cap on how many results are fetched; the search box is the real filter. */
    readonly property int resultLimit: 300

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0

    implicitWidth: 460
    implicitHeight: 460

    background: Rectangle {
        color: Theme.panel
        radius: Theme.radiusMd
        border.width: 1
        border.color: Theme.border
    }

    onOpened: {
        searchField.text = ""
        root.reload()
        searchField.forceActiveFocus()
    }

    // -- results ------------------------------------------------------------
    /*! Flat rows plus injected header rows, so one ListView renders both. */
    property var rows: []

    function reload() {
        const flat = GraphDataModel.searchSignals(searchField.text, root.resultLimit)
        const built = []
        let currentGroup = ""

        // Virtual signals lead: there are a handful of them against hundreds of
        // decoded ones, and a user who has just built a formula is looking for
        // it, not scrolling past it.
        const needle = searchField.text.trim().toLowerCase()
        const math = GraphDataModel.mathChannels()
        let mathShown = 0
        for (let m = 0; m < math.length; ++m) {
            const channel = math[m]
            if (!channel.valid)
                continue
            if (needle.length > 0
                    && channel.name.toLowerCase().indexOf(needle) < 0
                    && channel.expression.toLowerCase().indexOf(needle) < 0)
                continue
            if (mathShown === 0)
                built.push({ header: true, title: qsTr("Virtual  ·  Math channels") })
            ++mathShown
            built.push({
                header: false,
                math: true,
                entry: {
                    channelId: channel.id,
                    signalName: channel.name,
                    unit: channel.unit
                }
            })
        }
        for (let i = 0; i < flat.length; ++i) {
            const entry = flat[i]
            const group = entry.dbcName + "  ·  " + entry.messageName
            if (group !== currentGroup) {
                currentGroup = group
                built.push({ header: true, title: group })
            }
            built.push({ header: false, entry: entry })
        }
        root.rows = built
    }

    function commit(row) {
        // Takes the ROW, not the entry: only the row knows which of the two
        // kinds this is, and the two carry different keys.
        if (row.math)
            root.mathChannelChosen(row.entry.channelId)
        else
            root.signalChosen(row.entry.dbcIndex, row.entry.messageIndex, row.entry.signalIndex)
        root.close()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // -- title
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.toolbarHeight
            color: Theme.panelElevated
            radius: Theme.radiusMd

            // Square off the bottom corners so the header meets the body flush.
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: parent.radius
                color: parent.color
            }

            Label {
                anchors.verticalCenter: parent.verticalCenter
                x: Theme.spacingMd
                text: root.paneId.length > 0 ? qsTr("Add signal to %1").arg(root.paneId)
                                             : qsTr("Add signal")
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMd
                font.weight: Font.DemiBold
            }
        }

        // -- search
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.inputHeight + Theme.spacingSm * 2

            ThemedTextField {
                id: searchField

                anchors.fill: parent
                anchors.margins: Theme.spacingSm
                placeholderText: qsTr("Filter by signal or message name...")
                onTextChanged: root.reload()

                Keys.onReturnPressed: {
                    // Enter takes the first real result, which is what a search
                    // box is for - no reaching for the mouse.
                    for (let i = 0; i < root.rows.length; ++i) {
                        if (!root.rows[i].header) {
                            root.commit(root.rows[i])
                            return
                        }
                    }
                }
                Keys.onEscapePressed: root.close()
            }
        }

        // -- list
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.background

            ListView {
                id: resultList

                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: root.rows

                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Item {
                    id: resultRow

                    required property int index
                    required property var modelData

                    width: ListView.view.width
                    height: modelData.header ? 22 : Theme.rowHeight

                    // -- message group heading
                    Label {
                        visible: resultRow.modelData.header
                        anchors.verticalCenter: parent.verticalCenter
                        x: Theme.spacingSm
                        width: parent.width - Theme.spacingSm * 2
                        text: resultRow.modelData.header ? resultRow.modelData.title : ""
                        color: Theme.textDisabled
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        font.letterSpacing: 0.6
                        elide: Text.ElideRight
                    }

                    // -- a pickable signal
                    Rectangle {
                        visible: !resultRow.modelData.header
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacingMd
                        color: entryHover.hovered ? Theme.selection : "transparent"

                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            x: Theme.spacingSm
                            width: parent.width * 0.62
                            text: resultRow.modelData.header ? "" : resultRow.modelData.entry.signalName
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeMd
                            elide: Text.ElideRight
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.spacingMd
                            text: resultRow.modelData.header ? "" : resultRow.modelData.entry.unit
                            color: Theme.textDisabled
                            font.family: Theme.monoFontFamily
                            font.pixelSize: Theme.fontSizeSm
                        }

                        HoverHandler { id: entryHover }
                        TapHandler {
                            onSingleTapped: root.commit(resultRow.modelData)
                        }
                    }
                }
            }

            // -- nothing to show
            ColumnLayout {
                anchors.centerIn: parent
                width: parent.width - Theme.spacingXl
                visible: root.rows.length === 0
                spacing: Theme.spacingXs

                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: GraphDataModel.databaseCount === 0
                          ? qsTr("No databases loaded.")
                          : qsTr("Nothing matches that.")
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeMd
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    visible: GraphDataModel.databaseCount === 0
                    text: qsTr("Load a .dbc in the panel on the right first.")
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    wrapMode: Text.WordWrap
                }
            }
        }

        // -- footer
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.statusBarHeight
            color: Theme.panel

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            Label {
                anchors.verticalCenter: parent.verticalCenter
                x: Theme.spacingMd
                text: qsTr("%1 signal(s) available - Enter picks the first match")
                      .arg(GraphDataModel.availableSignalCount)
                color: Theme.textDisabled
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSm
            }
        }
    }
}
