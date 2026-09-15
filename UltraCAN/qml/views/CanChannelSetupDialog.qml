import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtQuick.Controls.Basic

import UltraCAN.Theme
import UltraCAN.Components
import UltraCAN.Core

/*!
    \qmltype CanChannelSetupDialog
    \brief The whole of one message on one screen: its header fields, its
           signals, the bit layout they compete for, and the properties of
           whichever signal is selected.

    \section2 Why a Window and not a Popup

    A Popup is clipped to its parent window, and this dialog is deliberately
    large - the bit matrix alone wants ~380 px. Making it a real Window also
    lets the user drag it onto a second monitor and keep the grid visible
    behind it, which is exactly how the tools this is modelled on are used.

    \section2 Why it writes through instead of buffering

    The opposite choice from SignalValuesDialog, and for the opposite reason:
    there is no Cancel here. Every field commits to the live DbcSignalObject as
    it is edited, so the bit matrix, the signal list colours and the main grid
    all move together while the user is still typing. A buffered form could not
    show a start-bit collision until it was too late to matter.

    That makes an accidental edit real, so the top-level document keeps its own
    modified flag and Save is still an explicit act.

    \section2 What is deliberately absent

    The reference layout has PDU type, Delay and Sensitivity fields. A .dbc
    cannot express any of them, so rather than show controls that write
    nowhere, they are left out. Colour is shown but not editable for the same
    reason - see the swatch's tooltip.
*/
Window {
    id: root

    /*! Index into DbcModel. -1 closes the dialog. */
    property int messageIndex: -1
    /*! Index into the message's signal list, or -1 for "no signal selected". */
    property int signalIndex: -1
    /*! Colour index of the message row, so the swatches here match the grid. */
    property int baseColorIndex: 0

    readonly property var messageObject: messageIndex >= 0
                                       ? DbcModel.message(messageIndex) : null
    readonly property var signalObject: (messageIndex >= 0 && signalIndex >= 0)
                                      ? DbcModel.signalAt(messageIndex, signalIndex) : null
    readonly property var signalModel: messageObject ? messageObject.signalList : null

    /*! Bumped whenever something that a Q_INVOKABLE reads has changed. Naming
        it inside a binding is what makes that binding re-run - an invokable
        call is not a dependency the engine can track on its own. */
    property int refreshTick: 0

    title: messageObject ? qsTr("CAN channel setup - %1").arg(messageObject.name)
                         : qsTr("CAN channel setup")
    /*!
        Frameless, matching the main window.

        \c{Qt.Dialog}, not the bare \c{Qt.Window} a frameless conversion
        usually reaches for: Qt.Dialog already implies Qt.Window and adds the
        dialog hints, which are what keep this off the taskbar and above its
        parent. Swapping it for Qt.Window would give a modal child window its
        own taskbar button.

        The frame carried edge resizing away with it, so WindowResizeBorders
        below hands that back - see its documentation.
    */
    flags: Qt.Dialog | Qt.FramelessWindowHint
    modality: Qt.ApplicationModal
    color: Theme.surface

    width: 980
    height: 700
    minimumWidth: 820
    minimumHeight: 560

    /*! Opens the dialog on \a message, selecting \a signal (or the first one). */
    function openFor(message, signal, colorIndex) {
        root.messageIndex = message
        root.baseColorIndex = colorIndex || 0
        const list = DbcModel.signalsOf(message)
        const count = list ? list.count : 0
        root.signalIndex = (signal !== undefined && signal >= 0 && signal < count)
                         ? signal
                         : (count > 0 ? 0 : -1)
        ++root.refreshTick
        root.show()
        root.raise()
        root.requestActivate()
    }

    function colorFor(index) {
        return Theme.seriesColor(root.baseColorIndex + 1 + index)
    }

    /*! Spans of the OTHER signals, so the matrix can show what a move would
        collide with. Rebuilt on refreshTick because it reads through the model. */
    readonly property var otherSpans: {
        root.refreshTick
        let out = []
        if (!root.signalModel)
            return out
        for (let i = 0; i < root.signalModel.count; ++i) {
            if (i === root.signalIndex)
                continue
            const s = root.signalModel.get(i)
            if (!s)
                continue
            out.push({ name: s.name, startBit: s.startBit,
                       bitLength: s.bitLength, byteOrder: s.byteOrder })
        }
        return out
    }

    /*!
        The label half of one form row.

        Declared at document top level because an inline component may not be
        nested, and it reads ONLY Theme - never an id from the enclosing
        document, which an inline component cannot rely on resolving. That is
        also why the widths live in Theme rather than on this Window.

        preferredWidth AND maximumWidth: without the cap, a long caption widens
        its whole grid column and drags every input in that column right.
    */
    component FormLabel: Label {
        Layout.preferredWidth: Theme.formLabelWidth
        Layout.maximumWidth: Theme.formLabelWidth
        Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
        horizontalAlignment: Text.AlignRight
        elide: Text.ElideRight
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeMd
    }

    SignalValuesDialog {
        id: valuesDialog
        // anchors.centerIn: parent, not Overlay.overlay - nothing else in this
        // project uses the Overlay attached type and an unavailable attached
        // property is a load-time failure, not a warning.
        anchors.centerIn: parent
        onAccepted: ++root.refreshTick
    }

    // ==================================================== frameless chrome
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TitleBar {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.titleBarHeight
            title: root.title
            // A modal dialog has nothing useful to do with either: minimising
            // it would strand the parent behind an invisible modal grab, and
            // maximising a fixed-purpose form just stretches whitespace.
            minimizeVisible: false
            maximizeVisible: false
            maximizeEnabled: false
        }

        // ======================================================== message setup
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.spacingMd
            spacing: Theme.spacingMd

            Rectangle {
                Layout.fillWidth: true
                // Height follows the CONTENT's implicit height, and the content is
                // anchored on three sides only. Anchoring it to the bottom as well
                // would make the layout's height depend on the rectangle's, which
                // depends right back on the layout - a binding loop that resolves
                // to whatever the first pass happened to compute.
                Layout.preferredHeight: messageBody.implicitHeight + 2 * Theme.spacingMd
                color: Theme.panel
                radius: Theme.radiusMd
                border.width: 1
                border.color: Theme.border

                ColumnLayout {
                    id: messageBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.spacingMd
                    spacing: Theme.spacingSm

                    Label {
                        text: qsTr("MESSAGE SETUP")
                        color: Theme.textDisabled
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        font.letterSpacing: 1.1
                    }

                    RowLayout {
                        id: messageRow
                        Layout.fillWidth: true
                        spacing: Theme.spacingLg

                        // ------------------------------------------- header fields
                        ColumnLayout {
                            Layout.preferredWidth: 280
                            Layout.alignment: Qt.AlignTop
                            spacing: Theme.spacingXs

                            PropertyField {
                                label: qsTr("Name")
                                labelWidth: 74
                                horizontalMargin: 0
                                ThemedTextField {
                                    Layout.fillWidth: true
                                    text: root.messageObject ? root.messageObject.name : ""
                                    enabled: root.messageObject !== null
                                    onTextEdited: if (root.messageObject)
                                                      root.messageObject.name = text
                                }
                            }

                            PropertyField {
                                label: qsTr("Arb. ID")
                                labelWidth: 74
                                horizontalMargin: 0
                                ThemedTextField {
                                    id: arbField
                                    Layout.preferredWidth: 90
                                    // Hex without the 0x, the way the grid shows it.
                                    text: root.messageObject
                                          ? Number(root.messageObject.canId).toString(16).toUpperCase()
                                          : ""
                                    enabled: root.messageObject !== null
                                    validator: RegularExpressionValidator {
                                        regularExpression: /[0-9A-Fa-f]{0,8}/
                                    }
                                    onEditingFinished: {
                                        if (!root.messageObject)
                                            return
                                        const parsed = parseInt(text, 16)
                                        if (isNaN(parsed)) {
                                            // Put the old value back rather than
                                            // writing 0 - an empty field is a
                                            // half-typed edit, not "id zero".
                                            text = Qt.binding(function() {
                                                return root.messageObject
                                                     ? Number(root.messageObject.canId)
                                                       .toString(16).toUpperCase() : ""
                                            })
                                            return
                                        }
                                        root.messageObject.canId = parsed
                                        // An id that cannot fit 11 bits is an
                                        // extended frame by arithmetic, not by
                                        // preference - the driver silently drops
                                        // the frame otherwise.
                                        if (parsed > 0x7FF)
                                            root.messageObject.extended = true
                                        ++root.refreshTick
                                    }
                                }
                                Label {
                                    text: qsTr("h")
                                    color: Theme.textDisabled
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeMd
                                }
                            }

                            PropertyField {
                                label: qsTr("Format")
                                labelWidth: 74
                                horizontalMargin: 0
                                ThemedComboBox {
                                    Layout.fillWidth: true
                                    model: [qsTr("Standard"), qsTr("Extended")]
                                    enabled: root.messageObject !== null
                                             && (root.messageObject.canId <= 0x7FF)
                                    currentIndex: root.messageObject && root.messageObject.extended
                                                  ? 1 : 0
                                    onActivated: if (root.messageObject) {
                                                     root.messageObject.extended = (currentIndex === 1)
                                                     ++root.refreshTick
                                                 }
                                    ToolTip.visible: hovered && !enabled
                                    ToolTip.text: qsTr("An id above 7FFh must be extended")
                                }
                            }

                            PropertyField {
                                label: qsTr("DLC")
                                labelWidth: 74
                                horizontalMargin: 0
                                suffix: qsTr("bytes")
                                ThemedSpinBox {
                                    Layout.preferredWidth: 90
                                    from: 0
                                    to: 64
                                    value: root.messageObject ? root.messageObject.dlc : 8
                                    enabled: root.messageObject !== null
                                    onValueModified: if (root.messageObject)
                                                         root.messageObject.dlc = value
                                }
                            }

                            PropertyField {
                                label: qsTr("Cycle")
                                labelWidth: 74
                                horizontalMargin: 0
                                suffix: qsTr("ms")
                                ThemedSpinBox {
                                    Layout.preferredWidth: 90
                                    from: 0
                                    to: 65535
                                    value: root.messageObject ? root.messageObject.cycleMs : 0
                                    enabled: root.messageObject !== null
                                    onValueModified: if (root.messageObject)
                                                         root.messageObject.cycleMs = value
                                }
                            }
                        }

                        // -------------------------------------------- signal list
                        ColumnLayout {
                            Layout.preferredWidth: 230
                            Layout.alignment: Qt.AlignTop
                            spacing: Theme.spacingXs

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 152
                                color: Theme.background
                                border.width: 1
                                border.color: Theme.border
                                clip: true

                                ListView {
                                    id: signalList
                                    anchors.fill: parent
                                    anchors.margins: 1
                                    model: root.signalModel
                                    clip: true
                                    boundsBehavior: Flickable.StopAtBounds
                                    currentIndex: root.signalIndex

                                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                                    delegate: Rectangle {
                                        id: sigRow

                                        required property int index
                                        required property string name

                                        width: ListView.view.width
                                        height: Theme.rowHeight
                                        color: sigRow.index === root.signalIndex
                                               ? Theme.selection : "transparent"

                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: Theme.spacingXs
                                            anchors.rightMargin: Theme.spacingXs
                                            spacing: Theme.spacingSm

                                            Rectangle {
                                                Layout.preferredWidth: 12
                                                Layout.preferredHeight: 12
                                                Layout.alignment: Qt.AlignVCenter
                                                radius: 2
                                                color: root.colorFor(sigRow.index)
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                text: sigRow.name
                                                elide: Text.ElideRight
                                                color: sigRow.index === root.signalIndex
                                                       ? Theme.selectionText : Theme.textPrimary
                                                font.family: Theme.fontFamily
                                                font.pixelSize: Theme.fontSizeMd
                                            }
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            onClicked: {
                                                root.signalIndex = sigRow.index
                                                ++root.refreshTick
                                            }
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacingSm

                                ToolbarButton {
                                    Layout.fillWidth: true
                                    text: qsTr("Add")
                                    enabled: root.messageIndex >= 0
                                    onClicked: {
                                        const added = DbcModel.addSignal(root.messageIndex)
                                        if (added >= 0)
                                            root.signalIndex = added
                                        ++root.refreshTick
                                    }
                                }
                                ToolbarButton {
                                    Layout.fillWidth: true
                                    text: qsTr("Delete")
                                    destructive: true
                                    enabled: root.signalIndex >= 0
                                    onClicked: {
                                        const removed = root.signalIndex
                                        DbcModel.removeSignal(root.messageIndex, removed)
                                        const count = root.signalModel ? root.signalModel.count : 0
                                        root.signalIndex = count > 0
                                                         ? Math.min(removed, count - 1) : -1
                                        ++root.refreshTick
                                    }
                                }
                            }
                        }

                        // --------------------------------------------- bit layout
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            spacing: Theme.spacingXs

                            Label {
                                text: qsTr("Bit layout")
                                color: Theme.textSecondary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeSm
                            }

                            BitMatrix {
                                id: matrix
                                Layout.fillWidth: true
                                Layout.preferredHeight: implicitHeight

                                startBit: root.signalObject ? root.signalObject.startBit : 0
                                bitLength: root.signalObject ? root.signalObject.bitLength : 1
                                byteOrder: root.signalObject ? root.signalObject.byteOrder : 1
                                dlc: root.messageObject ? root.messageObject.dlc : 8
                                otherSpans: root.otherSpans
                                interactive: root.signalObject !== null

                                // Clicking a cell moves the signal's START bit; the
                                // length is left alone, which is what makes the grid
                                // a positioning tool rather than a drawing surface.
                                onBitClicked: (bit) => {
                                    if (root.signalObject) {
                                        root.signalObject.startBit = bit
                                        ++root.refreshTick
                                    }
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: matrix.occupancyText
                                color: matrix.collisionCount > 0 || matrix.overrunsDlc
                                       ? Theme.error : Theme.textDisabled
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeSm
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }
            }

            // ===================================================== signal setup
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.panel
                radius: Theme.radiusMd
                border.width: 1
                border.color: Theme.border

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingMd
                    spacing: Theme.spacingSm

                    Label {
                        text: qsTr("SIGNAL SETUP")
                        color: Theme.textDisabled
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        font.letterSpacing: 1.1
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: root.signalObject === null
                        text: qsTr("This message has no signals yet. Use Add above.")
                        color: Theme.textDisabled
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeMd
                    }

                    /*!
                        One GridLayout, six columns: label / input, three times.

                        The previous layout was three independent ColumnLayouts of
                        PropertyFields. Each ROW inside those negotiated its own
                        width against its own children's minimums, so when the
                        block was narrower than the sum of its preferred widths -
                        which it always was - every row reached a different
                        compromise and the inputs stepped in and out. Measured on
                        the reported screenshot: the five left-hand inputs began at
                        x = 161, 291, 227, 227 and 163.

                        A GridLayout cannot do that. A column has ONE width shared
                        by every row in it, so the inputs line up by construction
                        rather than by three labelWidth values agreeing.

                        Label cells pin preferred AND maximum width, so a long
                        caption elides instead of widening its column; input cells
                        fill with a small minimum, so the shortfall is shared
                        evenly instead of being taken out of one row.
                    */
                    GridLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        visible: root.signalObject !== null
                        columns: 6
                        columnSpacing: Theme.spacingSm
                        rowSpacing: Theme.spacingXs

                        // -- row 1 ------------------------------------------------
                        FormLabel { text: qsTr("Name") }
                        ThemedTextField {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            text: root.signalObject ? root.signalObject.name : ""
                            onTextEdited: if (root.signalObject) root.signalObject.name = text
                        }

                        FormLabel { text: qsTr("Data format") }
                        ThemedComboBox {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            // Straight from C++ so the two numeric conventions can
                            // never drift apart.
                            model: DbcModel.byteOrderNames
                            currentIndex: root.signalObject ? root.signalObject.byteOrder
                                                            : DbcModel.byteOrderIntel
                            onActivated: if (root.signalObject) {
                                             root.signalObject.byteOrder = currentIndex
                                             ++root.refreshTick
                                         }
                        }

                        FormLabel { text: qsTr("Scale (k factor)") }
                        ThemedTextField {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            text: root.signalObject ? Number(root.signalObject.factor).toString() : "1"
                            validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                            onEditingFinished: {
                                if (!root.signalObject)
                                    return
                                const parsed = parseFloat(text)
                                // A zero factor collapses every raw value onto the
                                // offset and makes the signal undecodable both ways.
                                if (isNaN(parsed) || parsed === 0) {
                                    text = Qt.binding(function() {
                                        return root.signalObject
                                             ? Number(root.signalObject.factor).toString() : "1"
                                    })
                                    return
                                }
                                root.signalObject.factor = parsed
                                ++root.refreshTick
                            }
                        }

                        // -- row 2 ------------------------------------------------
                        FormLabel { text: qsTr("Description") }
                        ThemedTextField {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            text: root.signalObject ? root.signalObject.comment : ""
                            onTextEdited: if (root.signalObject) root.signalObject.comment = text
                        }

                        FormLabel { text: qsTr("Data type") }
                        ThemedComboBox {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            model: DbcModel.valueTypeNames
                            currentIndex: root.signalObject ? root.signalObject.valueType : 0
                            onActivated: if (root.signalObject) root.signalObject.valueType = currentIndex
                        }

                        FormLabel { text: qsTr("Offset") }
                        ThemedTextField {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            text: root.signalObject ? Number(root.signalObject.offset).toString() : "0"
                            validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                            onEditingFinished: {
                                if (!root.signalObject)
                                    return
                                const parsed = parseFloat(text)
                                if (isNaN(parsed)) {
                                    text = Qt.binding(function() {
                                        return root.signalObject
                                             ? Number(root.signalObject.offset).toString() : "0"
                                    })
                                    return
                                }
                                root.signalObject.offset = parsed
                                ++root.refreshTick
                            }
                        }

                        // -- row 3 ------------------------------------------------
                        FormLabel { text: qsTr("Unit") }
                        ThemedTextField {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            text: root.signalObject ? root.signalObject.unit : ""
                            onTextEdited: if (root.signalObject) root.signalObject.unit = text
                        }

                        FormLabel { text: qsTr("Start bit") }
                        ThemedSpinBox {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            from: 0
                            to: 63
                            value: root.signalObject ? root.signalObject.startBit : 0
                            onValueModified: if (root.signalObject) {
                                                 root.signalObject.startBit = value
                                                 ++root.refreshTick
                                             }
                        }

                        FormLabel { text: qsTr("Maximum") }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            spacing: Theme.spacingXs

                            ThemedTextField {
                                Layout.fillWidth: true
                                Layout.minimumWidth: Theme.formInputMinWidth
                                text: root.signalObject
                                      ? Number(root.signalObject.maximum).toFixed(3) : "0.000"
                                validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                                onEditingFinished: {
                                    if (!root.signalObject)
                                        return
                                    const parsed = parseFloat(text)
                                    if (!isNaN(parsed))
                                        root.signalObject.maximum = parsed
                                    ++root.refreshTick
                                }
                            }
                            ToolbarButton {
                                text: qsTr("Auto")
                                onClicked: if (root.signalObject) {
                                               root.signalObject.maximum = root.signalObject.autoMaximum()
                                               ++root.refreshTick
                                           }
                            }
                        }

                        // -- row 4 ------------------------------------------------
                        FormLabel { text: qsTr("Color") }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            Layout.preferredHeight: Theme.inputHeight
                            radius: Theme.radiusSm
                            color: root.signalIndex >= 0 ? root.colorFor(root.signalIndex)
                                                         : Theme.panelElevated
                            border.width: 1
                            border.color: Theme.border

                            HoverHandler { id: colorHover }
                            ToolTip.visible: colorHover.hovered
                            // Said plainly rather than shown as a disabled picker: a
                            // control that looks editable and silently discards the
                            // choice is worse than no control.
                            ToolTip.text: qsTr("Assigned from the theme palette. "
                                             + "A .dbc file cannot store a colour.")
                        }

                        FormLabel { text: qsTr("Length") }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            spacing: Theme.spacingXs

                            ThemedSpinBox {
                                Layout.fillWidth: true
                                Layout.minimumWidth: Theme.formInputMinWidth
                                from: 1
                                to: 64
                                value: root.signalObject ? root.signalObject.bitLength : 1
                                onValueModified: if (root.signalObject) {
                                                     root.signalObject.bitLength = value
                                                     ++root.refreshTick
                                                 }
                            }
                            Label {
                                text: qsTr("bits")
                                color: Theme.textDisabled
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeSm
                            }
                        }

                        FormLabel { text: qsTr("Minimum") }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            spacing: Theme.spacingXs

                            ThemedTextField {
                                Layout.fillWidth: true
                                Layout.minimumWidth: Theme.formInputMinWidth
                                text: root.signalObject
                                      ? Number(root.signalObject.minimum).toFixed(3) : "0.000"
                                validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                                onEditingFinished: {
                                    if (!root.signalObject)
                                        return
                                    const parsed = parseFloat(text)
                                    if (!isNaN(parsed))
                                        root.signalObject.minimum = parsed
                                    ++root.refreshTick
                                }
                            }
                            ToolbarButton {
                                text: qsTr("Auto")
                                onClicked: if (root.signalObject) {
                                               root.signalObject.minimum = root.signalObject.autoMinimum()
                                               ++root.refreshTick
                                           }
                            }
                        }

                        // -- row 5 ------------------------------------------------
                        FormLabel { text: qsTr("Receiver") }
                        ThemedTextField {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            text: root.signalObject ? root.signalObject.receiver : ""
                            placeholderText: qsTr("comma separated")
                            onEditingFinished: if (root.signalObject) root.signalObject.receiver = text
                        }

                        FormLabel { text: qsTr("Values") }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            spacing: Theme.spacingSm

                            ToolbarButton {
                                text: qsTr("Values...")
                                enabled: root.signalObject !== null
                                onClicked: valuesDialog.openFor(
                                               root.signalObject,
                                               root.signalObject ? root.signalObject.name : "")
                            }
                            Label {
                                Layout.fillWidth: true
                                text: {
                                    root.refreshTick
                                    if (!root.signalObject)
                                        return ""
                                    const n = root.signalObject.valueDescriptions.length
                                    return n === 0 ? qsTr("none") : qsTr("%1 defined").arg(n)
                                }
                                elide: Text.ElideRight
                                color: Theme.textDisabled
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeSm
                            }
                        }

                        FormLabel { text: qsTr("Current raw") }
                        Label {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            text: {
                                root.refreshTick
                                const v = liveProbe.physical
                                if (v === undefined || !root.signalObject)
                                    return "-"
                                const factor = root.signalObject.factor || 1
                                return ((v - root.signalObject.offset) / factor).toFixed(3)
                            }
                            color: Theme.textSecondary
                            font.family: Theme.monoFontFamily
                            font.pixelSize: Theme.fontSizeMd
                        }

                        // -- row 6 ------------------------------------------------
                        Item { Layout.columnSpan: 4; Layout.preferredHeight: 1 }

                        FormLabel { text: qsTr("Current value") }
                        Label {
                            Layout.fillWidth: true
                            Layout.preferredWidth: Theme.formInputWidth
                            Layout.minimumWidth: Theme.formInputMinWidth
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            text: {
                                root.refreshTick
                                const v = liveProbe.physical
                                if (v === undefined || !root.signalObject)
                                    return "-"
                                // The same C++ rule the analyzer and the grid
                                // use, so one signal never reads three ways.
                                return root.signalObject.formattedValue(v)
                            }
                            color: Theme.textPrimary
                            font.family: Theme.monoFontFamily
                            font.pixelSize: Theme.fontSizeMd
                        }
                    }

                    // A poll rather than a binding: the live value lives behind a
                    // Q_INVOKABLE, and the model's own dataChanged is throttled for
                    // the grid, not for two labels. 5 Hz matches that throttle.
                    Timer {
                        id: liveProbe
                        property var physical: undefined
                        interval: 200
                        repeat: true
                        running: root.visible && root.signalObject !== null
                        onTriggered: physical = DbcFlatModel.liveValueAt(root.messageIndex,
                                                                         root.signalIndex)
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacingSm

                        Item { Layout.fillWidth: true }

                        ToolbarButton {
                            text: qsTr("OK")
                            primary: true
                            onClicked: root.close()
                        }
                    }
                }
            }
        }
    }

    /*!
        The 1 px edge, and why it is not a drop shadow.

        A frameless window has no system border, so without this the dialog
        dissolves into whatever dark surface is behind it. A QML DropShadow
        cannot help: an effect is painted INSIDE the window, so a shadow would
        eat layout space and still stop dead at the window edge. A real shadow
        on Win32 comes from DWM (DwmExtendFrameIntoClientArea), which is a
        native call, not a QML one. So: one hairline, drawn last so it sits
        over the content it frames.
    */
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.width: 1
        border.color: Theme.border
        // Purely decorative - never let it intercept a press meant for the
        // resize strips underneath it.
        z: 50
    }

    // Hands edge dragging back to the OS, exactly as the main window does.
    WindowResizeBorders {}
}
