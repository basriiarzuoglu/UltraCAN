import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs

import UltraCAN.Theme
import UltraCAN.Core
import UltraCAN.Components

/*!
    \qmltype RawAnalyzerView
    \brief Bit/byte activity heatmap for reverse-engineering an undocumented bus.

    The workflow this view exists for:
      1. leave the vehicle idle for a few seconds,
      2. press \e {Set Reference} - every row drops to zero,
      3. do the one thing you want to find (turn the wheel, press the brake),
      4. the bits that carry it are the only ones that light up,
      5. right-click the first and last bit of that run and create a channel,
         which lands in the DBC editor as a real signal and is immediately
         plottable in the Graph Plot.

    All of the arithmetic is in CanAnalyzerModel and all of the pixels are in
    BitStripItem; this file is the table, the toolbar and the wiring between
    them. In particular there is deliberately NO Repeater of bit rectangles -
    see the note at the top of BitStripItem.h for why that matters.
*/
BasePage {
    id: root

    title: qsTr("Raw Data Analyzer")
    subtitle: CanAnalyzerModel.count > 0
              ? qsTr("%1 arbitration IDs - %2 - %3")
                .arg(CanAnalyzerModel.count)
                .arg(CanAnalyzerModel.referenceSet ? qsTr("showing NEW activity since the reference")
                                                   : qsTr("showing absolute activity"))
                .arg(CanAnalyzerModel.databaseCount === 0
                     ? qsTr("no database")
                     : qsTr("%1 database(s)").arg(CanAnalyzerModel.databaseCount))
              : qsTr("Listening for traffic")
    contentMargins: 0

    // ---------------------------------------------------------------- layout
    // Widths are shared by the header bar and every row, so the two can never
    // drift apart. The last entry is 0 = "take what is left", which is the bit
    // strip - it should get every pixel the text columns do not need.
    /*! Live column widths. NOT readonly any more - the header's splitters
        write here, and the rows read the same array, so header and body
        cannot drift apart however far a column is dragged. */
    property var analyzerColumns: [ 18, 190, 62, 80, 92, 20, 34, 0 ]
    /*! What double-clicking a splitter restores. Kept separate rather than
        reconstructed, so "reset" always means the shipped layout. */
    readonly property var defaultAnalyzerColumns: [ 18, 190, 62, 80, 92, 20, 34, 0 ]

    /*! Replaces the whole array on purpose. \c analyzerColumns is a `var`, and
        QML compares those by REFERENCE: writing analyzerColumns[i] = w mutates
        the object every binding already holds, so nothing re-evaluates and the
        drag appears to do nothing. slice() + assign is what notifies. */
    function setAnalyzerColumn(index, width) {
        if (index < 0 || index >= root.analyzerColumns.length - 1)
            return                          // the last column flexes
        const clamped = Math.max(16, Math.round(width))
        if (root.analyzerColumns[index] === clamped)
            return
        let next = root.analyzerColumns.slice()
        next[index] = clamped
        root.analyzerColumns = next
    }
    readonly property var analyzerTitles: [ "", qsTr("ARB."), qsTr("Bits"), qsTr("Frequency"),
                                            qsTr("Change"), qsTr("C"), qsTr("R/T"),
                                            qsTr("Value") ]

    /*! Height of one decoded-signal line inside an expanded row. */
    readonly property int signalRowHeight: 20

    property int contextRow: -1
    property int contextCell: -1

    // -------------------------------------------------------------- toolbar
    toolbar: Component {
        RowLayout {
            spacing: Theme.spacingSm

            ToolbarButton {
                text: qsTr("Set Reference")
                glyph: "◎"
                primary: !CanAnalyzerModel.referenceSet
                enabled: CanAnalyzerModel.count > 0
                onClicked: CanAnalyzerModel.setReference()
            }
            ToolbarButton {
                text: qsTr("Clear Reference")
                glyph: "○"
                enabled: CanAnalyzerModel.referenceSet
                onClicked: CanAnalyzerModel.clearReference()
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: Theme.inputHeight - 8
                Layout.alignment: Qt.AlignVCenter
                color: Theme.border
            }

            Label {
                text: qsTr("Analysis type")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMd
            }
            ThemedComboBox {
                Layout.preferredWidth: 168
                model: CanAnalyzerModel.analysisTypeNames
                currentIndex: CanAnalyzerModel.analysisType
                onActivated: CanAnalyzerModel.analysisType = currentIndex
            }

            Label {
                text: qsTr("Data direction")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMd
            }
            ThemedComboBox {
                Layout.preferredWidth: 104
                model: CanAnalyzerModel.byteOrderNames
                currentIndex: CanAnalyzerModel.byteOrder
                onActivated: CanAnalyzerModel.byteOrder = currentIndex
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: Theme.inputHeight - 8
                Layout.alignment: Qt.AlignVCenter
                color: Theme.border
            }

            ThemedCheckBox {
                text: qsTr("Automatic Sort")
                checked: CanAnalyzerModel.autoSort
                onToggled: {
                    const wanted = checked
                    checked = Qt.binding(function() { return CanAnalyzerModel.autoSort })
                    CanAnalyzerModel.autoSort = wanted
                }
            }
            ToolbarButton {
                text: qsTr("Sort by Change")
                glyph: "↓"
                onClicked: CanAnalyzerModel.sortBy(2, true)   // SortByChange
            }
            ToolbarButton {
                text: qsTr("Unsort")
                onClicked: CanAnalyzerModel.unsort()
            }
            ToolbarButton {
                text: qsTr("Collapse all")
                glyph: "⌃"
                enabled: CanAnalyzerModel.expandedCount > 0
                onClicked: CanAnalyzerModel.collapseAll()
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: Theme.inputHeight - 8
                Layout.alignment: Qt.AlignVCenter
                color: Theme.border
            }

            ToolbarButton {
                text: CanAnalyzerModel.running ? qsTr("Pause") : qsTr("Resume")
                // The third transport in the app; all three now share the
                // same two images rather than three different glyph guesses.
                iconSource: "qrc:/qt/qml/UltraCAN/assets/icons/"
                          + (CanAnalyzerModel.running ? "pause.png" : "play.png")
                onClicked: CanAnalyzerModel.running = !CanAnalyzerModel.running
            }
            ToolbarButton {
                text: qsTr("Clear")
                glyph: "✕"
                destructive: true
                onClicked: CanAnalyzerModel.reset()
            }
        }
    }

    FileDialog {
        id: dbcDialog
        title: qsTr("Load a database for the analyzer")
        fileMode: FileDialog.OpenFile
        nameFilters: [ qsTr("CAN databases (*.dbc)"), qsTr("All files (*)") ]
        onAccepted: CanAnalyzerModel.loadAnalyzerDbc(selectedFile)
    }

    // ------------------------------------------------------- cell context menu
    // Mirrors the Dewesoft flow exactly: pick a first bit, pick a second bit,
    // then turn the span between them into a channel.
    Menu {
        id: cellMenu

        background: Rectangle {
            implicitWidth: 232
            color: Theme.panel
            radius: Theme.radiusSm
            border.width: 1
            border.color: Theme.border
        }

        AnalyzerMenuItem {
            text: qsTr("Select first bit")
            onTriggered: CanAnalyzerModel.selectFirstBit(root.contextRow, root.contextCell)
        }
        AnalyzerMenuItem {
            text: qsTr("Select second bit")
            onTriggered: CanAnalyzerModel.selectSecondBit(root.contextRow, root.contextCell)
        }
        AnalyzerMenuItem {
            text: qsTr("Create channel between selected bits")
            enabled: CanAnalyzerModel.canCreateChannel
            onTriggered: CanAnalyzerModel.createChannelBetweenSelectedBits()
        }
    }

    // ================================================================== body
    //
    // SplitView rather than a fixed RowLayout: the heatmap is sixty-four cells
    // wide and the panel is a list of file names, so where the boundary
    // belongs depends on the monitor. The user drags it once.
    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        handle: Rectangle {
            implicitWidth: 4
            color: SplitHandle.pressed ? Theme.accent
                 : (SplitHandle.hovered ? Theme.panelElevated : Theme.border)
        }

        ColumnLayout {
            // Anchors are forbidden on a SplitView child - the SplitView owns
            // the geometry.
            SplitView.fillWidth: true
            SplitView.minimumWidth: 420
            spacing: 0

            TableHeaderBar {
                Layout.fillWidth: true
                columns: root.analyzerColumns
                titles: root.analyzerTitles
                rightAligned: [ 2, 3, 4 ]
                // The splitters live in the header component, so the CAN
                // Logger gets them by setting the same one property.
                resizable: true
                onColumnResized: (index, width) => root.setAnalyzerColumn(index, width)
                onColumnResetRequested: (index) =>
                    root.setAnalyzerColumn(index, root.defaultAnalyzerColumns[index])
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.background

                ListView {
                    id: table

                    anchors.fill: parent
                    clip: true
                    model: CanAnalyzerModel.rows
                    boundsBehavior: Flickable.StopAtBounds
                    reuseItems: true
                    cacheBuffer: 400

                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    delegate: Rectangle {
                        id: analyzerRow

                        required property int index
                        required property var model

                        width: ListView.view.width
                        // The strip half is always exactly one row tall; only the
                        // panel below it grows. This is what keeps the heatmap
                        // geometry identical whether the row is open or shut.
                        height: Theme.rowHeight
                                + (analyzerRow.model.expanded ? signalPanel.panelHeight : 0)
                        color: CanAnalyzerModel.selectionRow === analyzerRow.index
                               ? Theme.selection
                               : (analyzerRow.index % 2 === 1 ? Theme.plotBackground : Theme.background)

                        Behavior on height {
                            NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
                        }

                        readonly property int gutter: Theme.spacingSm

                        // ---------------------------------------------- raw row
                        /*!
                            Geometry must match TableHeaderBar EXACTLY, because
                            the header is the same widths laid out by a different
                            object - nothing enforces agreement but this comment
                            and the numbers below.

                            The house convention, shared with LogRow: an outer
                            gutter on each side, \c gutter of SPACING between
                            cells, and cell content flush inside its cell. This
                            row previously used spacing 0 with the gutter applied
                            INSIDE each cell, which looks identical for column 0
                            and then drifts by one gutter per column - 8, 16, 24
                            ... 64 px by the time it reaches the bit strip. That
                            is the squashed-header / sliding-data effect: nothing
                            was squashed, the two were simply counting different
                            gaps.
                        */
                        Row {
                            id: rawRow

                            x: analyzerRow.gutter
                            width: parent.width - analyzerRow.gutter * 2
                            spacing: analyzerRow.gutter
                            // Pinned. Without this the Row stretches with the
                            // delegate and the BitStripItem grows into the panel.
                            height: Theme.rowHeight

                            // -- expand chevron
                            Item {
                                width: root.analyzerColumns[0]
                                height: parent.height

                                Label {
                                    anchors.centerIn: parent
                                    text: analyzerRow.model.expanded ? "▾" : "▸"
                                    color: analyzerRow.model.signalCount > 0 || analyzerRow.model.known
                                           ? Theme.textSecondary : Theme.textDisabled
                                    font.pixelSize: Theme.fontSizeSm
                                }
                                TapHandler {
                                    onSingleTapped: CanAnalyzerModel.toggleExpanded(analyzerRow.index)
                                }
                            }

                            // -- ARB.  "MessageName (0x1A0)" when the DBC knows it
                            Item {
                                width: root.analyzerColumns[1]
                                height: parent.height
                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    text: analyzerRow.model.arbText
                                    // A named id is a solved id; colour is the
                                    // fastest way to see how much of the bus is
                                    // still unknown.
                                    color: analyzerRow.model.known ? Theme.accentAlt : Theme.textPrimary
                                    font.family: Theme.monoFontFamily
                                    font.pixelSize: Theme.fontSizeMd
                                    elide: Text.ElideRight
                                }
                            }

                            // -- Bits (DLC)
                            Item {
                                width: root.analyzerColumns[2]
                                height: parent.height
                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.right: parent.right
                                    text: qsTr("DLC %1").arg(analyzerRow.model.dlc)
                                    color: Theme.textSecondary
                                    font.family: Theme.monoFontFamily
                                    font.pixelSize: Theme.fontSizeSm
                                }
                            }

                            // -- Frequency
                            Item {
                                width: root.analyzerColumns[3]
                                height: parent.height
                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.right: parent.right
                                    text: qsTr("%1 Hz").arg(analyzerRow.model.frequency.toFixed(3))
                                    color: analyzerRow.model.frequency > 0 ? Theme.textPrimary
                                                                           : Theme.textDisabled
                                    font.family: Theme.monoFontFamily
                                    font.pixelSize: Theme.fontSizeSm
                                }
                            }

                            // -- Change, with the number sitting on its own bar. A
                            //    column of digits is hard to scan; a column of bars
                            //    is not, and this is the column you scan.
                            Item {
                                width: root.analyzerColumns[4]
                                height: parent.height

                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    x: 0
                                    height: parent.height - 8
                                    // Spans the cell, so "full bar" means exactly
                                    // the column the Change header sits over.
                                    width: Math.max(0, parent.width
                                                       * analyzerRow.model.change / 100)
                                    radius: Theme.radiusSm
                                    color: Theme.accent
                                    opacity: 0.28
                                }
                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.right: parent.right
                                    text: analyzerRow.model.change.toFixed(3)
                                    color: analyzerRow.model.change > 0 ? Theme.accent : Theme.textDisabled
                                    font.family: Theme.monoFontFamily
                                    font.pixelSize: Theme.fontSizeSm
                                    font.weight: analyzerRow.model.change > 0 ? Font.DemiBold : Font.Normal
                                }
                            }

                            // -- colour swatch
                            Item {
                                width: root.analyzerColumns[5]
                                height: parent.height
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 10; height: 10; radius: 2
                                    color: Theme.seriesColor(analyzerRow.model.colorIndex)
                                }
                            }

                            // -- Rx/Tx
                            Item {
                                width: root.analyzerColumns[6]
                                height: parent.height
                                Label {
                                    anchors.centerIn: parent
                                    text: analyzerRow.model.direction
                                    color: analyzerRow.model.direction === "Tx" ? Theme.accent
                                                                                : Theme.success
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontSizeSm
                                }
                            }

                            // -- the heatmap itself
                            Item {
                                // Same arithmetic as TableHeaderBar's last column:
                                // the row's inner width, less the fixed columns,
                                // less the seven inter-column gaps. Forgetting the
                                // gaps here is what let the heatmap start 64 px
                                // left of its own header.
                                width: Math.max(64, rawRow.width
                                                    - root.analyzerColumns
                                                          .slice(0, 7)
                                                          .reduce(function(a, b) { return a + b }, 0)
                                                    - analyzerRow.gutter
                                                      * (root.analyzerColumns.length - 1))
                                height: parent.height

                                BitStripItem {
                                    id: strip

                                    anchors.fill: parent
                                    anchors.topMargin: 1
                                    anchors.bottomMargin: 1

                                    source: CanAnalyzerModel
                                    row: analyzerRow.index

                                    ramp: Theme.heatRamp
                                    inactiveColor: Theme.heatInactive
                                    glyphColor: Theme.heatGlyph
                                    selectionColor: Theme.textPrimary
                                }

                                TapHandler {
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onSingleTapped: function(point, button) {
                                        const cell = strip.cellAt(point.position.x)
                                        if (cell < 0)
                                            return
                                        root.contextRow = analyzerRow.index
                                        root.contextCell = cell
                                        if (button === Qt.RightButton) {
                                            cellMenu.popup()
                                        } else {
                                            // Left click alternates: first pick
                                            // anchors, second completes the span.
                                            if (CanAnalyzerModel.firstBit < 0
                                                    || CanAnalyzerModel.selectionRow !== analyzerRow.index)
                                                CanAnalyzerModel.selectFirstBit(analyzerRow.index, cell)
                                            else
                                                CanAnalyzerModel.selectSecondBit(analyzerRow.index, cell)
                                        }
                                    }
                                }
                            }
                        }

                        // -------------------------------------- decoded signals
                        // A Loader, so a collapsed row costs nothing: the panel and
                        // its delegates do not exist until the row is opened, and
                        // are destroyed the moment it closes. With a hundred ids on
                        // screen that is the difference between zero extra items
                        // and several hundred.
                        Loader {
                            id: signalPanel

                            anchors.top: rawRow.bottom
                            anchors.left: parent.left
                            anchors.right: parent.right

                            readonly property int panelHeight: item ? item.implicitHeight : 0
                            // Explicit rather than relying on the Loader adopting
                            // its item's implicit size: the delegate's own height
                            // is computed from panelHeight, and leaving both to be
                            // inferred is how a layout loop starts.
                            height: panelHeight

                            active: analyzerRow.model.expanded
                            visible: active

                            // Declared inline, NOT as a root-level Component: a
                            // Component resolves ids in the scope where it is
                            // WRITTEN, so one defined at the top of the file could
                            // not see analyzerRow and would need the row index
                            // smuggled in through parent chains. Declaring it here
                            // costs nothing - a Component is a declaration, and the
                            // item inside it is still only built when active turns
                            // true.
                            sourceComponent: Component {
                                Rectangle {
                                    implicitHeight: signalColumn.implicitHeight
                                                    + Theme.spacingSm * 2
                                    color: Theme.surface

                                    // Left rule tying the panel to its row.
                                    Rectangle {
                                        // Centred under the chevron, which now
                                        // starts one gutter in like every cell.
                                        x: analyzerRow.gutter + root.analyzerColumns[0] / 2
                                        width: 2
                                        height: parent.height
                                        color: Theme.accentAlt
                                        opacity: 0.5
                                    }

                                    Column {
                                        id: signalColumn

                                        x: analyzerRow.gutter + root.analyzerColumns[0]
                                           + Theme.spacingMd
                                        y: Theme.spacingSm
                                        width: parent.width - x - Theme.spacingMd
                                        spacing: 1

                                        // Built ONCE per open, and again only when
                                        // the database itself changes. Binding this
                                        // to anything that ticks at the refresh rate
                                        // would rebuild every delegate ten times a
                                        // second to show the same signal names.
                                        property var descriptors:
                                            CanAnalyzerModel.signalDescriptors(analyzerRow.index)

                                        Connections {
                                            target: CanAnalyzerModel
                                            function onNamesChanged() {
                                                signalColumn.descriptors =
                                                    CanAnalyzerModel.signalDescriptors(analyzerRow.index)
                                            }
                                        }

                                        Label {
                                            visible: signalColumn.descriptors.length === 0
                                            text: analyzerRow.model.known
                                                  ? qsTr("No signals defined for this message.")
                                                  : qsTr("This ID is not in the loaded database - select two bits above to define one.")
                                            color: Theme.textDisabled
                                            font.family: Theme.fontFamily
                                            font.pixelSize: Theme.fontSizeSm
                                        }

                                        Repeater {
                                            model: signalColumn.descriptors

                                            delegate: Item {
                                                id: signalLine

                                                required property int index
                                                required property var modelData

                                                width: signalColumn.width
                                                height: root.signalRowHeight

                                                readonly property real span:
                                                    signalLine.modelData.maximum - signalLine.modelData.minimum
                                                // The tick again - same reason as
                                                // the value label below.
                                                readonly property real fraction:
                                                    (span > 0 && CanAnalyzerModel.refreshTick >= 0)
                                                    ? Math.max(0, Math.min(1,
                                                        (CanAnalyzerModel.signalValue(analyzerRow.index,
                                                                                      signalLine.index)
                                                         - signalLine.modelData.minimum) / span))
                                                    : 0

                                                // -- name
                                                Label {
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    width: 220
                                                    text: signalLine.modelData.name
                                                    color: Theme.textSecondary
                                                    font.family: Theme.fontFamily
                                                    font.pixelSize: Theme.fontSizeSm
                                                    elide: Text.ElideRight
                                                }

                                                /*!
                                                    Live value, now possibly
                                                    "32 (P)" rather than a bare
                                                    number.

                                                    The width GROWS with the
                                                    text instead of being a flat
                                                    150: a mapped value can be
                                                    "32 (Zwischenstellung)", and
                                                    eliding that to "32 (Zwis..."
                                                    throws away the only part
                                                    that carries information.
                                                    Capped so it can never reach
                                                    the name column, and the
                                                    range bar starts after it -
                                                    the bar is decorative, so it
                                                    is what gives up the space.
                                                */
                                                Label {
                                                    id: valueLabel

                                                    readonly property real wanted:
                                                        Math.min(implicitWidth + Theme.spacingSm, 300)

                                                    anchors.verticalCenter: parent.verticalCenter
                                                    x: 224
                                                    width: Math.max(150, wanted)
                                                    horizontalAlignment: Text.AlignRight
                                                    // refreshTick is named in this
                                                    // binding on purpose. signalText()
                                                    // is an invokable, not a bindable
                                                    // property, so without a
                                                    // dependency that actually changes
                                                    // the binding would evaluate once
                                                    // and freeze at whatever the value
                                                    // was when the row opened.
                                                    text: CanAnalyzerModel.refreshTick >= 0
                                                          ? CanAnalyzerModel.signalText(analyzerRow.index,
                                                                                        signalLine.index)
                                                          : ""
                                                    color: Theme.textPrimary
                                                    font.family: Theme.monoFontFamily
                                                    font.pixelSize: Theme.fontSizeSm
                                                    font.weight: Font.DemiBold
                                                    elide: Text.ElideRight
                                                }

                                                // -- where the value sits in its
                                                //    declared range. Only drawn when
                                                //    the database actually declares
                                                //    one; a 0..0 bar would be a lie.
                                                Rectangle {
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    // Starts after the value,
                                                    // wherever the value ended.
                                                    x: valueLabel.x + valueLabel.width + Theme.spacingMd
                                                    width: Math.max(0, parent.width - x)
                                                    height: 4
                                                    radius: 2
                                                    visible: signalLine.span > 0
                                                    color: Theme.panelElevated

                                                    Rectangle {
                                                        width: parent.width * signalLine.fraction
                                                        height: parent.height
                                                        radius: parent.radius
                                                        color: Theme.accentAlt
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 1
                            color: Theme.border
                            opacity: 0.4
                        }
                    }
                }

                // -- empty state
                ColumnLayout {
                    anchors.centerIn: parent
                    visible: CanAnalyzerModel.count === 0
                    spacing: Theme.spacingSm

                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: "▦"
                        color: Theme.textDisabled
                        font.pixelSize: 40
                    }
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("No traffic yet.")
                        color: Theme.textDisabled
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeMd
                    }
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("Connect a bus - the analyzer needs no database.")
                        color: Theme.textDisabled
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                    }
                }
            }

            // ------------------------------------------------------ status strip
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.statusBarHeight
                color: Theme.panel

                Rectangle { width: parent.width; height: 1; color: Theme.border }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingMd
                    anchors.rightMargin: Theme.spacingMd
                    spacing: Theme.spacingLg

                    Label {
                        text: qsTr("%1 IDs").arg(CanAnalyzerModel.count)
                        color: Theme.textSecondary
                        font.family: Theme.monoFontFamily
                        font.pixelSize: Theme.fontSizeSm
                    }
                    Label {
                        visible: CanAnalyzerModel.expandedCount > 0
                        text: qsTr("%1 expanded").arg(CanAnalyzerModel.expandedCount)
                        color: Theme.accentAlt
                        font.family: Theme.monoFontFamily
                        font.pixelSize: Theme.fontSizeSm
                    }
                    Label {
                        visible: CanAnalyzerModel.referenceSet
                        text: qsTr("REFERENCE SET")
                        color: Theme.accent
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        font.weight: Font.DemiBold
                    }
                    Label {
                        visible: !CanAnalyzerModel.running
                        text: qsTr("PAUSED")
                        color: Theme.warning
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        font.weight: Font.DemiBold
                    }
                    Label {
                        visible: CanAnalyzerModel.selectionSummary.length > 0
                        text: CanAnalyzerModel.selectionSummary
                        color: Theme.accentAlt
                        font.family: Theme.monoFontFamily
                        font.pixelSize: Theme.fontSizeSm
                    }
                    ToolbarButton {
                        visible: CanAnalyzerModel.canCreateChannel
                        text: qsTr("Create channel")
                        primary: true
                        onClicked: CanAnalyzerModel.createChannelBetweenSelectedBits()
                    }

                    Item { Layout.fillWidth: true }

                    Label {
                        text: CanAnalyzerModel.lastMessage
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                        elide: Text.ElideRight
                        Layout.maximumWidth: 420
                    }

                    // -- heat legend
                    Label {
                        text: qsTr("idle")
                        color: Theme.textDisabled
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                    }
                    Row {
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 0
                        Repeater {
                            model: Theme.heatRamp
                            delegate: Rectangle {
                                required property var modelData
                                width: 12
                                height: 10
                                color: modelData
                            }
                        }
                    }
                    Label {
                        text: qsTr("every frame")
                        color: Theme.textDisabled
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeSm
                    }
                }
            }
        }

        // ------------------------------------------------ DBC manager panel
        //
        // These databases belong to THIS VIEW. Loading one here does not touch
        // the DBC Editor's document and the Editor's document does not appear
        // here - which is the whole point: on an unknown bus you try three
        // candidate files in a row, and none of that churn should land in the
        // .dbc you are actually authoring.
        ColumnLayout {
            SplitView.preferredWidth: 250
            SplitView.minimumWidth: 200
            SplitView.maximumWidth: 420
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.toolbarHeight
                color: Theme.panel

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: Theme.border
                }

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    x: Theme.spacingMd
                    text: qsTr("ANALYZER DATABASES")
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    font.letterSpacing: 1.1
                }
            }

            // -- actions
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: dbcActions.implicitHeight + Theme.spacingSm * 2
                color: Theme.surface

                ColumnLayout {
                    id: dbcActions

                    x: Theme.spacingSm
                    y: Theme.spacingSm
                    width: parent.width - Theme.spacingSm * 2
                    spacing: Theme.spacingXs

                    ToolbarButton {
                        Layout.fillWidth: true
                        text: qsTr("Load DBC")
                        glyph: "+"
                        primary: true
                        onClicked: dbcDialog.open()
                    }

                    // Convenience, not coupling: it reads the Editor's current
                    // file path once, on click, and loads a private copy. After
                    // that the two are unrelated.
                    ToolbarButton {
                        Layout.fillWidth: true
                        text: qsTr("Add editor's DBC")
                        glyph: "↧"
                        enabled: DbcModel.sourceUrl.toString().length > 0
                        onClicked: CanAnalyzerModel.loadAnalyzerDbc(DbcModel.sourceUrl)
                    }

                    ToolbarButton {
                        Layout.fillWidth: true
                        text: qsTr("Remove all")
                        destructive: true
                        enabled: CanAnalyzerModel.databaseCount > 0
                        onClicked: CanAnalyzerModel.clearAnalyzerDbcs()
                    }
                }
            }

            // -- loaded files
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.background

                ListView {
                    id: dbcList

                    anchors.fill: parent
                    anchors.margins: Theme.spacingSm
                    clip: true
                    spacing: Theme.spacingXs
                    // A plain QVariantList, rebuilt only on load/remove. It is
                    // deliberately NOT bound to anything that ticks with the
                    // display refresh, or these delegates would be destroyed
                    // and recreated ten times a second.
                    model: CanAnalyzerModel.loadedDbcs

                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    delegate: Rectangle {
                        id: dbcRow

                        required property int index
                        required property var modelData

                        width: ListView.view.width
                        height: dbcInfo.implicitHeight + Theme.spacingSm * 2
                        radius: Theme.radiusSm
                        color: dbcHover.hovered ? Theme.panelElevated : Theme.panel
                        border.width: 1
                        border.color: Theme.border

                        HoverHandler { id: dbcHover }

                        // Priority marker. First match wins, so the number is
                        // not decoration - it says which file claims an id when
                        // two of them define it.
                        Rectangle {
                            x: Theme.spacingSm
                            y: Theme.spacingSm
                            width: 16
                            height: 16
                            radius: 3
                            color: Theme.accentAlt
                            opacity: 0.85

                            Label {
                                anchors.centerIn: parent
                                text: String(dbcRow.index + 1)
                                color: Theme.background
                                font.family: Theme.monoFontFamily
                                font.pixelSize: Theme.fontSizeSm
                                font.weight: Font.DemiBold
                            }
                        }

                        ColumnLayout {
                            id: dbcInfo

                            x: Theme.spacingSm + 22
                            y: Theme.spacingSm
                            width: parent.width - x - 26
                            spacing: 1

                            Label {
                                Layout.fillWidth: true
                                text: dbcRow.modelData.name
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeMd
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("%1 messages - %2 signals")
                                      .arg(dbcRow.modelData.messages)
                                      .arg(dbcRow.modelData["signals"])
                                color: Theme.textDisabled
                                font.family: Theme.monoFontFamily
                                font.pixelSize: Theme.fontSizeSm
                            }
                            Label {
                                Layout.fillWidth: true
                                // refreshTick, same reason as everywhere else:
                                // matchedIdCount is an invokable, so without a
                                // changing dependency this would read once and
                                // freeze at zero.
                                readonly property int matched:
                                    CanAnalyzerModel.refreshTick >= 0
                                    ? CanAnalyzerModel.matchedIdCount(dbcRow.index)
                                    : 0
                                text: matched > 0 ? qsTr("resolving %1 live ID(s)").arg(matched)
                                                  : qsTr("no live ID matches yet")
                                color: matched > 0 ? Theme.success : Theme.textDisabled
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontSizeSm
                                elide: Text.ElideRight
                            }
                        }

                        // -- remove
                        Rectangle {
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: Theme.spacingXs
                            width: 18
                            height: 18
                            radius: 3
                            color: removeHover.hovered ? Theme.error : "transparent"

                            Label {
                                anchors.centerIn: parent
                                text: "✕"
                                color: removeHover.hovered ? Theme.background : Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSm
                            }

                            HoverHandler { id: removeHover }
                            TapHandler {
                                onSingleTapped: CanAnalyzerModel.removeAnalyzerDbc(dbcRow.index)
                            }

                            ToolTip.visible: removeHover.hovered
                            ToolTip.delay: 400
                            ToolTip.text: qsTr("Remove - its IDs revert to raw")
                        }
                    }
                }

                // -- empty state
                ColumnLayout {
                    anchors.centerIn: parent
                    width: parent.width - Theme.spacingLg * 2
                    visible: CanAnalyzerModel.databaseCount === 0
                    spacing: Theme.spacingXs

                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: "⛁"
                        color: Theme.textDisabled
                        font.pixelSize: 28
                    }
                    Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("No databases loaded.\nEvery ID is shown raw.")
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
                    x: Theme.spacingSm
                    width: parent.width - Theme.spacingSm * 2
                    text: CanAnalyzerModel.databaseCount === 0
                          ? qsTr("Independent of the DBC Editor")
                          : qsTr("%1 loaded - first match wins")
                            .arg(CanAnalyzerModel.databaseCount)
                    color: Theme.textDisabled
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                    elide: Text.ElideRight
                }
            }
        }
    }

    // Inline components must be declared at the top level of the document, not
    // nested inside the object that uses them - the same placement every other
    // view in this project uses.
    component AnalyzerMenuItem: MenuItem {
        id: entry

        implicitHeight: Theme.inputHeight
        implicitWidth: 240

        contentItem: Label {
            text: entry.text
            color: !entry.enabled ? Theme.textDisabled
                                  : (entry.highlighted ? Theme.selectionText : Theme.textPrimary)
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeMd
            verticalAlignment: Text.AlignVCenter
            leftPadding: Theme.spacingSm
            elide: Text.ElideRight
        }
        background: Rectangle {
            color: entry.highlighted ? Theme.selection : "transparent"
        }
    }
}
