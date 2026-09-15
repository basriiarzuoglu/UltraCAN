import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import UltraCAN.Theme
import UltraCAN.Components
import UltraCAN.Core

/*!
    \qmltype TriggerSetupDialog
    \brief Configures WHAT the background log may see (Filters) and WHEN it
           writes (Triggers).

    Two tabs rather than one long column, because the two answer different
    questions and a user setting one rarely wants to read the other. They stay
    in ONE dialog, and Apply commits both, because they interact: a filter that
    excludes the trigger\'s id means the capture can never start, and that
    warning has to be visible from wherever the user is standing.

    \section1 Why the pre-trigger control is the first thing in the dialog

    A trigger without history answers "what happened after the fault", which is
    rarely the question. The interesting frames are the ones BEFORE the event,
    and a user who does not notice the pre-trigger control gets a file that
    starts at the symptom. So it leads, with its own sentence of explanation.

    \section1 Nothing is applied until Apply

    The condition is pushed into CanLogModel only on Apply, in one call per
    side. Live-applying each keystroke would re-resolve the signal key on every
    character and, worse, arm a half-typed condition on a logger that may
    already be waiting.
*/
Popup {
    id: root

    /*! The CanLogModel being configured - CanBusManager.logModel. */
    required property var trace

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0

    implicitWidth: 540

    /*!
        Every band of the dialog, measured rather than assumed.

        The bug this replaces added the header and the footer to the page
        height and forgot the TAB BAR, so the dialog was always one tab bar too
        short - and since the shortfall came off the bottom, it ate the footer.
        The Filters tab made it obvious because its page is the shorter of the
        two: 130 px of dialog for 40 px of header, 40 of tabs, 90 of content and
        50 of buttons.

        So nothing here is a literal. Each band reports its own implicit height
        and this adds them up, which also means the sum cannot drift when a band
        gains a row.
    */
    readonly property int chromeHeight: headerBar.implicitHeight + tabs.implicitHeight
                                        + footer.implicitHeight + Theme.spacingMd * 2
    /*! The page the user is looking at, so the dialog is as tall as it needs to
        be for THIS tab rather than for the tallest one. */
    readonly property int pageHeight: pageStack.currentIndex === 0
                                      ? filterPage.implicitHeight
                                      : triggerPage.implicitHeight
    /*! Beyond this the page scrolls instead of the dialog growing off-screen. */
    readonly property int maximumHeight: 620

    implicitHeight: Math.min(root.maximumHeight, root.chromeHeight + root.pageHeight)

    background: Rectangle {
        color: Theme.panel
        radius: Theme.radiusMd
        border.width: 1
        border.color: Theme.border
    }

    // Index 0 is "None" in both dropdowns, matching TriggerCondition::Kind.
    readonly property var kindLabels: [qsTr("None"), qsTr("CAN ID matches"), qsTr("Signal value")]
    readonly property var comparisonLabels: [">", ">=", "<", "<=", "==", "!="]
    readonly property var filterModeLabels: [qsTr("Log all IDs"), qsTr("Single ID"),
                                             qsTr("ID range"), qsTr("Payload mask")]

    property var signalNames: []

    onOpened: {
        root.signalNames = root.trace ? root.trace.decoderSignalNames() : []
        const config = root.trace ? root.trace.triggerConfig() : ({})
        preTrigger.value = Math.round((config.preTriggerSeconds || 0) * 10)
        root.loadSide(startSide, config.start)
        root.loadSide(stopSide, config.stop)

        const filter = root.trace ? root.trace.logFilter() : ({})
        filterSide.mode = filter.mode || 0
        filterSide.startIdText = "0x" + Number(filter.startId || 0).toString(16).toUpperCase()
        filterSide.endIdText = "0x" + Number(filter.endId || 0).toString(16).toUpperCase()
        filterSide.byteIndex = filter.byteIndex || 0
        filterSide.maskText = Number(filter.payloadMask !== undefined ? filter.payloadMask : 255)
                              .toString(16).toUpperCase()
        filterSide.matchText = Number(filter.payloadMatch || 0).toString(16).toUpperCase()
        root.problem = ""
    }

    /*! Pushes the filter into C++. Returns "" or a sentence naming the typo. */
    function applyFilter() {
        if (filterSide.mode === 0) {
            root.trace.setLogFilter(0, 0, 0, 0, 0xFF, 0)
            return ""
        }

        const startId = filterSide.parseHex(filterSide.startIdText)
        if (startId < 0)
            return qsTr("“%1” is not a hex CAN ID.").arg(filterSide.startIdText)

        if (filterSide.mode === 1) {
            root.trace.setLogFilter(1, startId, startId, 0, 0xFF, 0)
            return ""
        }

        if (filterSide.mode === 2) {
            const endId = filterSide.parseHex(filterSide.endIdText)
            if (endId < 0)
                return qsTr("“%1” is not a hex CAN ID.").arg(filterSide.endIdText)
            root.trace.setLogFilter(2, startId, endId, 0, 0xFF, 0)
            return ""
        }

        const mask = filterSide.parseHex(filterSide.maskText)
        const match = filterSide.parseHex(filterSide.matchText)
        if (mask < 0 || match < 0)
            return qsTr("Mask and match must be hex bytes, e.g. FF and 00.")
        if ((match & ~mask & 0xFF) !== 0) {
            // Refused, not stored: the rule cannot match any frame, so saving
            // it would produce a capture that stays empty for a reason nothing
            // on screen explains afterwards.
            return qsTr("The match value has bits the mask clears - no frame can "
                      + "satisfy that rule.")
        }
        root.trace.setLogFilter(3, startId, startId, filterSide.byteIndex, mask, match)
        return ""
    }

    function loadSide(side, spec) {
        if (!spec)
            return
        side.kind = spec.kind || 0
        side.idText = "0x" + Number(spec.frameId || 0).toString(16).toUpperCase()
        side.extended = !!spec.extended
        const full = (spec.messageName ? spec.messageName + "." : "") + (spec.signalName || "")
        side.signalIndex = root.signalNames.indexOf(full)
        side.comparison = spec.comparison || 0
        side.thresholdText = String(spec.threshold !== undefined ? spec.threshold : 0)
    }

    /*! Pushes one side into C++. Returns "" or a sentence naming what is wrong. */
    function applySide(side, isStart) {
        if (side.kind === 0) {
            root.trace.setTrigger(isStart, 0, 0, false, "", "", 0, 0)
            return ""
        }

        if (side.kind === 1) {
            // parseInt handles both "0x123" and "291"; NaN is the typo case and
            // must not silently become id 0, which is a real and common frame.
            const id = parseInt(side.idText, side.idText.trim().toLowerCase().startsWith("0x") ? 16 : 10)
            if (isNaN(id) || id < 0) {
                return qsTr("“%1” is not a CAN ID.").arg(side.idText)
            }
            root.trace.setTrigger(isStart, 1, id, side.extended || id > 0x7FF, "", "", 0, 0)
            return ""
        }

        if (side.signalIndex < 0 || side.signalIndex >= root.signalNames.length)
            return qsTr("Pick a signal for the %1 condition.")
                   .arg(isStart ? qsTr("start") : qsTr("stop"))

        const value = Number(side.thresholdText)
        if (isNaN(value))
            return qsTr("“%1” is not a number.").arg(side.thresholdText)

        const full = root.signalNames[side.signalIndex]
        const dot = full.lastIndexOf(".")
        const messageName = dot > 0 ? full.substring(0, dot) : ""
        const signalName = dot > 0 ? full.substring(dot + 1) : full

        if (!root.trace.setTrigger(isStart, 2, 0, false, messageName, signalName,
                                   side.comparison, value)) {
            // Stored anyway, so this is a warning about the CURRENT DBC, not a
            // refusal - see CanLogModel::setTrigger().
            return qsTr("“%1” is not in the loaded database; the condition is saved but will not fire until it is.").arg(full)
        }
        return ""
    }

    property string problem: ""

    function apply() {
        if (!root.trace)
            return

        // The filter FIRST: filterTriggerConflict() below compares the two, and
        // it can only do that once both are in the model.
        const f = root.applyFilter()
        if (f.length > 0) {
            root.problem = f
            tabs.currentIndex = 0
            return
        }

        root.trace.preTriggerSeconds = preTrigger.value / 10.0
        const a = root.applySide(startSide, true)
        const b = root.applySide(stopSide, false)
        root.problem = a.length > 0 ? a : b
        // A warning about an unloaded DBC still closes the dialog; a typo does
        // not, because there is nothing useful stored yet.
        if (a.length > 0 || b.length > 0)
            return

        // Both halves are stored and legal on their own; the last question is
        // whether they contradict each other. A warning, not a refusal - the
        // user may be about to change the trigger next.
        const clash = root.trace.filterTriggerConflict()
        if (clash.length > 0) {
            root.problem = clash
            return
        }
        root.close()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ------------------------------------------------------------ header
        Rectangle {
            id: headerBar

            Layout.fillWidth: true
            Layout.preferredHeight: Theme.toolbarHeight
            implicitHeight: Theme.toolbarHeight
            color: Theme.panelElevated
            radius: Theme.radiusMd

            // Squares off the bottom corners so the header meets the tabs flush.
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: parent.radius
                color: parent.color
            }

            Label {
                anchors.verticalCenter: parent.verticalCenter
                x: Theme.spacingMd
                text: qsTr("Filters and triggers")
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMd
                font.weight: Font.DemiBold
            }
        }

        // -------------------------------------------------------------- tabs
        TabBar {
            id: tabs

            Layout.fillWidth: true
            background: Rectangle { color: Theme.surface }

            Repeater {
                model: [qsTr("Filters"), qsTr("Triggers")]

                delegate: TabButton {
                    id: tabButton

                    required property int index
                    required property string modelData

                    text: modelData
                    contentItem: Label {
                        text: tabButton.text
                        color: tabs.currentIndex === tabButton.index ? Theme.accent
                                                                     : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeMd
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: Theme.surface
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 2
                            color: tabs.currentIndex === tabButton.index ? Theme.accent
                                                                         : Theme.border
                        }
                    }
                }
            }
        }

        // ------------------------------------------------------------- pages
        //
        // fillHeight, so this band absorbs every pixel the fixed bands above
        // and below do not want. That is what keeps the footer on screen when
        // the content is taller than maximumHeight: the Flickable shrinks and
        // scrolls, rather than the column pushing its last child out of the
        // dialog.
        Flickable {
            id: pageArea

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: Theme.spacingMd
            clip: true
            contentWidth: width
            contentHeight: pageStack.height
            boundsBehavior: Flickable.StopAtBounds
            // Only interactive when there is something to scroll to; otherwise
            // a short page rubber-bands under the cursor for no reason.
            interactive: contentHeight > height

            ScrollBar.vertical: ScrollBar {
                policy: pageArea.contentHeight > pageArea.height ? ScrollBar.AlwaysOn
                                                                 : ScrollBar.AlwaysOff
            }

            StackLayout {
                id: pageStack

                x: Theme.spacingMd
                width: pageArea.width - Theme.spacingMd * 2
                // Explicit, not implicit: a StackLayout sizes itself from ALL
                // of its children, so the short tab would inherit the tall
                // tab's height and leave a lake of empty space under it.
                height: root.pageHeight

                // One direction only: the TabBar owns the selection and the
                // stack follows it. Binding both ways looks symmetrical and
                // means a click has to settle a loop before the page changes.
                currentIndex: tabs.currentIndex

                // ------------------------------------------------ FILTERS
                ColumnLayout {
                    id: filterPage

                    Layout.alignment: Qt.AlignTop
                    spacing: Theme.spacingSm

                    LogFilterRow {
                        id: filterSide
                        Layout.fillWidth: true
                        modeLabels: root.filterModeLabels
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: root.problem.length > 0 && tabs.currentIndex === 0
                        text: root.problem
                        color: Theme.error
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        wrapMode: Text.WordWrap
                    }

                    // Absorbs the slack when this page is shorter than the one
                    // beside it, so its rows stay at the top instead of being
                    // spread down the panel by the layout.
                    Item { Layout.fillHeight: true }
                }

                // ----------------------------------------------- TRIGGERS
                ColumnLayout {
                    id: triggerPage

                    Layout.alignment: Qt.AlignTop
                    spacing: Theme.spacingSm

                    Label {
                        text: qsTr("PRE-TRIGGER WINDOW")
                        color: Theme.textDisabled
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        font.letterSpacing: 1.1
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacingSm

                        ThemedSlider {
                            id: preTrigger
                            Layout.fillWidth: true
                            from: 0
                            to: 100                     // tenths of a second, 0 - 10 s
                            stepSize: 1
                            value: 20
                        }
                        Label {
                            Layout.preferredWidth: 56
                            horizontalAlignment: Text.AlignRight
                            text: qsTr("%1 s").arg((preTrigger.value / 10).toFixed(1))
                            color: Theme.accent
                            font.family: Theme.monoFontFamily
                            font.pixelSize: Theme.fontSizeMd
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: preTrigger.value === 0
                              ? qsTr("The file starts at the trigger. Nothing before it is kept.")
                              : qsTr("The %1 s of traffic before the trigger is held in memory "
                                     + "and written to the file ahead of it.")
                                .arg((preTrigger.value / 10).toFixed(1))
                        color: Theme.textDisabled
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        wrapMode: Text.WordWrap
                    }

                    TriggerConditionRow {
                        id: startSide
                        Layout.fillWidth: true
                        title: qsTr("START CONDITION")
                        hint: qsTr("Until this fires the logger waits, holding history only.")
                        kindLabels: root.kindLabels
                        comparisonLabels: root.comparisonLabels
                        signalNames: root.signalNames
                    }

                    TriggerConditionRow {
                        id: stopSide
                        Layout.fillWidth: true
                        title: qsTr("STOP CONDITION")
                        hint: qsTr("Optional. With none set, recording runs until you press Stop.")
                        kindLabels: root.kindLabels
                        comparisonLabels: root.comparisonLabels
                        signalNames: root.signalNames
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: root.signalNames.length === 0
                        text: qsTr("No DBC is loaded, so only ID conditions are available.")
                        color: Theme.warning
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: root.problem.length > 0 && tabs.currentIndex === 1
                        text: root.problem
                        color: Theme.error
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        wrapMode: Text.WordWrap
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }

        // ------------------------------------------------------------ footer
        //
        // Last child of the column and fixed-height, so it is the one band that
        // can never be pushed anywhere: everything variable above it is inside
        // the fillHeight Flickable.
        Rectangle {
            id: footer

            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingMd
            Layout.preferredHeight: footerRow.implicitHeight + Theme.spacingSm * 2
            implicitHeight: footerRow.implicitHeight + Theme.spacingSm * 2
            color: Theme.panel

            // The separator earns its keep here: without it a ComboBox scrolled
            // to the bottom of the page sits directly against the buttons and
            // reads as part of them.
            Rectangle { width: parent.width; height: 1; color: Theme.border }

            RowLayout {
                id: footerRow

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.spacingMd
                anchors.rightMargin: Theme.spacingMd
                spacing: Theme.spacingSm

                ToolbarButton {
                    text: qsTr("Clear")
                    enabled: startSide.kind !== 0 || stopSide.kind !== 0 || filterSide.mode !== 0
                    onClicked: {
                        root.trace.clearTriggers()
                        root.trace.setLogFilter(0, 0, 0, 0, 0xFF, 0)
                        startSide.kind = 0
                        stopSide.kind = 0
                        filterSide.mode = 0
                        root.problem = ""
                    }
                }

                Item { Layout.fillWidth: true }

                ToolbarButton {
                    text: qsTr("Cancel")
                    onClicked: root.close()
                }
                ToolbarButton {
                    text: qsTr("Apply")
                    glyph: "✓"
                    primary: true
                    onClicked: root.apply()
                }
            }
        }
    }
}
