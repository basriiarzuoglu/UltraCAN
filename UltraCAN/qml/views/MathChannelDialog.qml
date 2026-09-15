import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import UltraCAN.Theme
import UltraCAN.Components
import UltraCAN.Core

/*!
    \qmltype MathChannelDialog
    \brief Authoring form for a virtual signal: name, unit, formula, and one
           dropdown per variable the formula mentions.

    \section1 Why the mapping rows are generated, not typed

    The formula is written in terms of short names - \c {(A - B) / 2} - because
    a real DBC signal name is thirty characters long and an expression built
    from those is unreadable. That leaves the question of what \c A is, and the
    only honest answer is a picker over the signals actually loaded: a typed
    name is a typo waiting to become a channel that compiles and never fires.

    So the variables come from MathChannelManager::variablesIn() on every
    keystroke, and each one grows a row here. The manager validates the formula
    on the same keystroke, which is why the error line can be live rather than
    a surprise on Create.

    \section1 What this does NOT do

    It never guesses. An unmapped variable blocks Create rather than defaulting
    to zero - a channel quietly reading 0 for one of its inputs plots a
    plausible, wrong line, and nothing on screen would say so.
*/
Popup {
    id: root

    /*! Emitted after a channel is created, with the new id. */
    signal channelCreated(int channelId)

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0

    implicitWidth: 520
    implicitHeight: Math.min(600, form.implicitHeight + Theme.toolbarHeight
                                  + Theme.statusBarHeight + Theme.spacingLg * 2)

    background: Rectangle {
        color: Theme.panel
        radius: Theme.radiusMd
        border.width: 1
        border.color: Theme.border
    }

    // -- state ---------------------------------------------------------------
    /*! Variable names in first-appearance order, straight from C++. */
    property var variables: []
    /*! variable -> index into \l signalChoices. -1 while unmapped. */
    property var mapping: ({})
    /*! Flat pick list, built once per opening from the loaded databases. */
    property var signalChoices: []
    property string formulaError: ""
    property string submitError: ""

    readonly property bool ready: nameField.text.trim().length > 0
                                  && formulaField.text.trim().length > 0
                                  && root.formulaError.length === 0
                                  && root.everyVariableMapped

    /*! Recomputed rather than cached: `mapping` is a var, and mutating a var in
        place notifies nothing, so anything bound to it must be re-read through
        a counter that IS notified. */
    property int mappingVersion: 0
    readonly property bool everyVariableMapped: {
        root.mappingVersion            // dependency, deliberately
        for (let i = 0; i < root.variables.length; ++i)
            if (root.mapping[root.variables[i]] === undefined
                    || root.mapping[root.variables[i]] < 0)
                return false
        return true
    }

    onOpened: {
        nameField.text = ""
        unitField.text = ""
        formulaField.text = ""
        root.variables = []
        root.mapping = ({})
        root.mappingVersion++
        root.formulaError = ""
        root.submitError = ""
        root.signalChoices = GraphDataModel.searchSignals("", 5000)
        nameField.forceActiveFocus()
    }

    function refreshVariables() {
        const found = MathChannelManager.variablesIn(formulaField.text)

        // Keep whatever the user already mapped: renaming C to D should not
        // clear A and B.
        const kept = {}
        for (let i = 0; i < found.length; ++i) {
            const variable = found[i]
            kept[variable] = (root.mapping[variable] !== undefined)
                             ? root.mapping[variable] : -1
        }
        root.mapping = kept
        root.variables = found
        root.mappingVersion++

        root.formulaError = formulaField.text.trim().length === 0
                            ? "" : MathChannelManager.validate(formulaField.text, found)
        root.submitError = ""
    }

    function choiceLabel(index) {
        if (index === undefined || index < 0 || index >= root.signalChoices.length)
            return qsTr("Pick a signal...")
        const choice = root.signalChoices[index]
        return choice.messageName + " . " + choice.signalName
    }

    function submit() {
        if (!root.ready)
            return

        const bindings = []
        for (let i = 0; i < root.variables.length; ++i) {
            const variable = root.variables[i]
            const choice = root.signalChoices[root.mapping[variable]]
            bindings.push({
                variable: variable,
                dbc: choice.dbcName,
                message: choice.messageName,
                signal: choice.signalName
            })
        }

        const channelId = MathChannelManager.createChannel(nameField.text, unitField.text,
                                                           formulaField.text, bindings)
        if (channelId < 0) {
            root.submitError = MathChannelManager.lastError
            return
        }

        root.channelCreated(channelId)
        root.close()
    }

    // -- body ----------------------------------------------------------------
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.toolbarHeight
            color: Theme.panelElevated
            radius: Theme.radiusMd

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: parent.radius
                color: parent.color
            }

            Label {
                anchors.verticalCenter: parent.verticalCenter
                x: Theme.spacingMd
                text: qsTr("Create math channel")
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMd
                font.weight: Font.DemiBold
            }
        }

        Flickable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: form.implicitHeight + Theme.spacingMd * 2
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            ColumnLayout {
                id: form

                x: Theme.spacingMd
                y: Theme.spacingMd
                width: parent.width - Theme.spacingMd * 2
                spacing: Theme.spacingSm

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacingSm

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            text: qsTr("Name")
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSm
                        }
                        ThemedTextField {
                            id: nameField
                            Layout.fillWidth: true
                            placeholderText: qsTr("Power")
                            onTextChanged: root.submitError = ""
                        }
                    }
                    ColumnLayout {
                        Layout.preferredWidth: 110
                        spacing: 2
                        Label {
                            text: qsTr("Unit")
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontSizeSm
                        }
                        ThemedTextField {
                            id: unitField
                            Layout.fillWidth: true
                            placeholderText: qsTr("W")
                        }
                    }
                }

                Label {
                    Layout.topMargin: Theme.spacingXs
                    text: qsTr("Formula")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                }
                ThemedTextField {
                    id: formulaField
                    Layout.fillWidth: true
                    placeholderText: qsTr("A * B / 1000")
                    font.family: Theme.monoFontFamily
                    onTextChanged: root.refreshVariables()
                    Keys.onEscapePressed: root.close()
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.formulaError.length > 0
                    text: root.formulaError
                    color: Theme.error
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    wrapMode: Text.WordWrap
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.formulaError.length === 0
                    // Shows the call form, not bare names: the first version
                    // of this line listed "sin" on its own, and the obvious
                    // reading of that is that sin is a thing you can name -
                    // which is exactly the formula the parser then queried.
                    text: qsTr("Allowed: arithmetic, comparisons, ?:, PI, E, and "
                             + "sin(x) cos(x) sqrt(x) abs(x) pow(x,y) - %1 in all")
                          .arg(MathChannelManager.allowedFunctions.length)
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    wrapMode: Text.WordWrap

                    HoverHandler { id: allowedHover }
                    ToolTip.visible: allowedHover.hovered
                    ToolTip.text: MathChannelManager.allowedFunctions
                                  .map(function (f) { return f + "()" }).join("  ")
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.spacingXs
                    Layout.preferredHeight: 1
                    color: Theme.border
                    visible: root.variables.length > 0
                }

                Label {
                    visible: root.variables.length > 0
                    text: qsTr("VARIABLES")
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    font.letterSpacing: 1.1
                }

                Repeater {
                    model: root.variables

                    delegate: RowLayout {
                        id: mapRow

                        required property int index
                        required property string modelData

                        Layout.fillWidth: true
                        spacing: Theme.spacingSm

                        Label {
                            Layout.preferredWidth: 34
                            text: mapRow.modelData
                            color: Theme.accent
                            font.family: Theme.monoFontFamily
                            font.pixelSize: Theme.fontSizeMd
                            font.weight: Font.DemiBold
                        }
                        Label {
                            text: "="
                            color: Theme.textDisabled
                            font.family: Theme.monoFontFamily
                            font.pixelSize: Theme.fontSizeMd
                        }
                        ThemedComboBox {
                            Layout.fillWidth: true

                            // Index 0 is the "pick one" placeholder, so the real
                            // choices are shifted by one - an unmapped variable
                            // has to be distinguishable from "the first signal".
                            model: {
                                const labels = [qsTr("Pick a signal...")]
                                for (let i = 0; i < root.signalChoices.length; ++i)
                                    labels.push(root.signalChoices[i].dbcName + "  ·  "
                                                + root.signalChoices[i].messageName + " . "
                                                + root.signalChoices[i].signalName)
                                return labels
                            }
                            currentIndex: {
                                root.mappingVersion
                                const chosen = root.mapping[mapRow.modelData]
                                return (chosen === undefined || chosen < 0) ? 0 : chosen + 1
                            }
                            onActivated: {
                                root.mapping[mapRow.modelData] = currentIndex - 1
                                root.mappingVersion++
                                root.submitError = ""
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.variables.length === 0 && root.formulaError.length === 0
                    text: GraphDataModel.databaseCount === 0
                          ? qsTr("No databases are loaded, so there is nothing to map a variable to.")
                          : qsTr("Name your inputs with single letters - A, B, C - and a row appears for each.")
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    wrapMode: Text.WordWrap
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.submitError.length > 0
                    text: root.submitError
                    color: Theme.error
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    wrapMode: Text.WordWrap
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.statusBarHeight + Theme.spacingSm
            color: Theme.panel

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spacingMd
                anchors.rightMargin: Theme.spacingMd
                spacing: Theme.spacingSm

                Item { Layout.fillWidth: true }

                ToolbarButton {
                    text: qsTr("Cancel")
                    onClicked: root.close()
                }
                ToolbarButton {
                    text: qsTr("Create")
                    glyph: "✓"
                    primary: true
                    enabled: root.ready
                    onClicked: root.submit()
                }
            }
        }
    }
}
