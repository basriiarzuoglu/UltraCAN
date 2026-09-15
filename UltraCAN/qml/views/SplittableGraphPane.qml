import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import QtCharts

import UltraCAN.Theme
import UltraCAN.Components
import UltraCAN.Core

/*!
    \qmltype SplittableGraphPane
    \brief One scope pane: its own chart, its own traces, its own axes.

    A pane is the unit of everything in the Graph Plot. It owns the series it
    creates, tells \c GraphDataModel which \l paneId they belong to, and scales
    its Y axis over its own traces only - so a pane showing 0..8000 rpm does not
    flatten the 0..1 boolean next door.

    The pane does NOT decide the layout. It reports \l splitRequested and
    \l closeRequested upward and lets the host rearrange, which is what keeps
    the same component usable inside the main window and inside a detached one.

    \section1 Interaction is all hand-rolled

    The QML \c ChartView has no mouse interaction of its own - no zoom, no pan,
    no rubber band. Those belong to \c QChartView, which is a QWidget and does
    not exist here. So every gesture below is implemented against the axes
    directly, and \l valueAt() is the one function that converts a pixel into
    an axis value.

    \section1 Removing a trace - read this before changing it

    \c ChartView.removeSeries() takes the series off the chart and RELEASES
    OWNERSHIP of it. It does not delete it, so the series' \c destroyed() signal
    never fires, so a design that leans on that signal to drop the C++ track
    leaves the track behind: the line disappears from the plot while the legend
    entry, the sample fan-out and the trace count all survive. Detach and
    re-attach then re-created the traces on top of the survivors and the legend
    doubled every round trip.

    So removal is explicit here. \c GraphDataModel.untrack() is the
    authoritative call and runs first; taking the series off the chart is a
    separate, purely visual step. The \c destroyed() connection in C++ stays as
    a safety net for the case where a whole ChartView really is destroyed (page
    navigation, window teardown), where it does fire.
*/
Rectangle {
    id: root

    /*! Stable identity for this pane, used as the track owner in C++. */
    required property string paneId
    property string title: ""
    property bool canSplit: true
    property bool canClose: false
    property bool active: false

    /*! "left" | "right" | "up" | "down" - where the NEW pane goes relative to
        this one. A direction, not an orientation: "split vertically" means the
        opposite thing in half the tools in this space, and the host needs to
        know which side the new pane lands on anyway. */
    signal splitRequested(string direction)
    signal closeRequested()
    signal activated()
    /*!
        Asks the host to open the signal picker for THIS pane.

        The picker lives on GraphPaneArea rather than here for two reasons: one
        dialog instead of one per pane, and it can centre itself on the whole
        scope area - a 460 px dialog centred on a quarter-pane in the 2x2
        layout would hang off every edge.
    */
    signal addSignalRequested()

    color: Theme.plotBackground
    border.width: 1
    border.color: root.active ? Theme.accent : Theme.border

    /*! Bumped whenever this pane's track set changes, to re-evaluate the
        invokables below - they are functions, not bindable properties. */
    property int tracksVersion: 0

    /*!
        Y-axis scaling, three states rather than a bool.

        \list
        \li \b Auto - Y follows the data, X follows the time window.
        \li \b Manual - Y is pinned to \l manualMin / \l manualMax, X still
            follows the window. Comparing two panes only means anything if
            their axes agree, and that cannot be done by eye.
        \li \b Free - neither box ticked: both axes belong to the mouse
            (wheel, box zoom, middle-drag pan).
        \endlist

        The two checkboxes are mutually exclusive, as asked - but unticking the
        active one leaves Free rather than forcing the other on. Free is how
        the existing wheel and box zoom work, and turning two states into two
        radio buttons would have deleted it.
    */
    property bool autoScale: true
    property bool manualScale: false
    property real manualMin: 0
    property real manualMax: 100

    /*!
        The mouse currently owns the axes, overriding Auto or Manual.

        \b {This flag is the feature, not the gating.} Letting the input
        handlers fire while paused is only half of it: \c refresh() emits
        \c refreshed() on its 60 Hz timer whether or not the model is paused -
        it stops INGESTING but it does not stop repainting - and this pane
        re-applies its scaling on every one of those. Without a suppression
        flag the user would drag a perfect zoom box and watch it be erased
        16 ms later, which reads as "zoom is broken" rather than "zoom is
        disallowed".

        Set by any zoom or pan gesture taken while paused, and cleared the
        moment the model resumes - live data with a frozen window would scroll
        straight out of view.
    */
    property bool zoomOverride: false

    /*! Zoom and pan are allowed in Free mode, and - regardless of mode -
        whenever the graph is paused. A still picture is exactly when someone
        wants to look closely at it. */
    readonly property bool interactiveAxes: (!root.autoScale && !root.manualScale)
                                            || GraphDataModel.paused

    /*! Auto/Manual reassert themselves as soon as the mouse stops owning the
        axes: on resume, on a mode change, or on the double-click reset. */
    function releaseZoomOverride() {
        if (!root.zoomOverride)
            return
        root.zoomOverride = false
        root.applyAutoScale()
    }

    Connections {
        target: GraphDataModel
        function onTracksChanged() { root.tracksVersion++ }
        function onRefreshed() { root.applyAutoScale() }
        function onPausedChanged() {
            // Resuming ends the override. Leaving it set would pin the window
            // where the user left it while live data scrolled past outside it.
            if (!GraphDataModel.paused)
                root.releaseZoomOverride()
        }
    }

    Component.onCompleted: root.applyAutoScale()

    Component.onDestruction: {
        // Safety net for a teardown that skips removeTrack() entirely.
        GraphDataModel.untrackPane(root.paneId)
    }

    // ------------------------------------------------------------ plotting
    /*! trackId -> series, so a legend entry can find its own line. */
    property var seriesByTrack: ({})

    function addSignal(dbcIndex, messageIndex, signalIndex) {
        const series = chart.createSeries(ChartView.SeriesTypeLine, "", axisX, axisY)
        if (!series) {
            root.notify(qsTr("The chart refused to create a series."))
            return
        }

        const trackId = GraphDataModel.trackSignal(dbcIndex, messageIndex, signalIndex,
                                                   series, root.paneId)
        root.finishTrack(series, trackId)
    }

    /*! A math channel is plotted exactly like a decoded signal - it just comes
        from MathChannelManager instead of a decode table. */
    function addMathChannel(channelId) {
        const series = chart.createSeries(ChartView.SeriesTypeLine, "", axisX, axisY)
        if (!series) {
            root.notify(qsTr("The chart refused to create a series."))
            return
        }

        const trackId = GraphDataModel.trackMathChannel(channelId, series, root.paneId)
        root.finishTrack(series, trackId)
    }

    /*! Shared tail of addSignal() / addMathChannel(): dress the series, wire
        its tooltip and register it. \a trackId < 0 means C++ refused, and the
        orphan series has to go with it. */
    function finishTrack(series, trackId) {
        if (trackId < 0) {
            chart.removeSeries(series)
            root.notify(GraphDataModel.lastError)
            return
        }

        // Name and colour come back from C++ so the legend, the series and the
        // readout cannot disagree about what is being drawn.
        const tracks = GraphDataModel.tracksForPane(root.paneId)
        for (let i = 0; i < tracks.length; ++i) {
            if (tracks[i].trackId !== trackId)
                continue
            series.name = tracks[i].name
            series.color = Theme.seriesColor(tracks[i].colorIndex)
            series.width = 2
            break
        }

        // Individual samples drawn on top of the line. On a CAN trace the
        // sample instants are information in their own right - a gap between
        // dots is a message that did not arrive, which a smooth line hides.
        series.pointsVisible = true

        // hovered(QPointF point, bool state) - verified against
        // QtCharts/plugins.qmltypes, as is mapToPosition(value, series).
        // Connected in script rather than declared, because the series is
        // created by createSeries() at runtime and has no QML body to put an
        // onHovered handler in.
        series.hovered.connect(function (point, state) {
            root.showPointTooltip(series, point, state)
        })

        root.seriesByTrack[trackId] = series
        root.tracksVersion++
        root.applyAutoScale()
    }

    function removeTrack(trackId) {
        pointTooltip.visible = false
        // Untrack FIRST and explicitly - see the class note. removeSeries()
        // alone would leave the track, the legend row and the fan-out alive.
        GraphDataModel.untrack(trackId)

        const series = root.seriesByTrack[trackId]
        if (series)
            chart.removeSeries(series)
        delete root.seriesByTrack[trackId]
        root.tracksVersion++
    }

    function clearPane() {
        pointTooltip.visible = false
        const tracks = GraphDataModel.tracksForPane(root.paneId)
        for (let i = 0; i < tracks.length; ++i)
            GraphDataModel.untrack(tracks[i].trackId)

        // removeAllSeries() DELETES the series it removes, where removeSeries()
        // only releases ownership - so this is both the correct and the only
        // leak-free way to empty a pane.
        chart.removeAllSeries()
        root.seriesByTrack = ({})
        root.tracksVersion++
    }

    property string notice: ""
    function notify(message) {
        root.notice = message
        noticeTimer.restart()
    }
    Timer {
        id: noticeTimer
        interval: 5000
        onTriggered: root.notice = ""
    }

    // ------------------------------------------------------- image export
    /*!
        Saves this pane exactly as it appears on screen - chart, axes, legend,
        title - as a PNG or JPEG.

        \c grabToImage is ASYNCHRONOUS. It schedules a render of this item
        into an offscreen texture and calls back a frame later, so there is
        nothing meaningful to return from here; the outcome is reported
        through \l notify() from inside the callback instead.
    */
    function exportImage(target) {
        // Any toast still on screen would otherwise be baked into the
        // picture. Clearing it now lands in the same frame the grab renders.
        root.notice = ""

        const started = root.grabToImage(function (result) {
            if (result.saveToFile(target))
                root.notify(qsTr("Saved %1").arg(root.fileNameOf(target)))
            else
                root.notify(qsTr("Could not write %1").arg(root.fileNameOf(target)))
        })

        // false means the grab could not even be SCHEDULED - no window, or a
        // zero-sized item. Worth saying out loud: the callback above simply
        // never runs, so without this the click looks like it did nothing.
        if (!started)
            root.notify(qsTr("This pane cannot be captured right now."))
    }

    /*! Last path segment of a file url - a five-second toast should not be
        carrying the whole "file:///C:/Users/..." across the pane. */
    function fileNameOf(fileUrl) {
        const text = fileUrl.toString()
        return text.slice(text.lastIndexOf("/") + 1)
    }

    FileDialog {
        id: exportDialog

        title: qsTr("Export pane as image")
        fileMode: FileDialog.SaveFile
        nameFilters: [ qsTr("PNG image (*.png)"), qsTr("JPEG image (*.jpg *.jpeg)") ]
        /*!
            Not cosmetic. saveToFile() picks the encoder from the file
            extension, and a name with NO extension makes it return false and
            write nothing at all - so a user who types "plot" and presses
            Save would get silence. This makes Qt append ".png" first.
        */
        defaultSuffix: "png"

        /*!
            selectedFile is handed to saveToFile() UNCHANGED - no stripping of
            the "file:///" prefix.

            The Qt 5 recipe was to convert the url to a raw OS path first,
            because saveToFile() took a QString. Qt 6.2 replaced that overload
            with saveToFile(QUrl), which rejects anything QUrl does not regard
            as a local file - and "C:/Users/basri/plot.png", which is exactly
            what stripping the prefix leaves on Windows, parses as a url whose
            SCHEME is "c". isLocalFile() is then false and the save fails with
            no file and no error. Passing the url through untouched is both
            simpler and the only form that is correct on this platform.
        */
        onAccepted: root.exportImage(exportDialog.selectedFile)
    }

    // ---------------------------------------------------------------- axes
    //
    // Axis ranges are ASSIGNED, never bound. A binding would fight both the
    // wheel handler and ChartView's own rubber-band zoom, which set the axis
    // ranges directly - the first drag would break the binding and the axis
    // would then be stuck wherever the drag left it.

    function applyAutoScale() {
        // The mouse wins while it holds the axes. This is what makes a zoom
        // taken during a pause survive the next repaint - see zoomOverride.
        if (root.zoomOverride)
            return

        // X follows the window in BOTH driven modes. Only Free hands it over.
        if (root.autoScale || root.manualScale) {
            // windowSeconds is clamped to [0.5, 600] in C++ and only ever
            // subtracted here, so a sub-second window needs no special case and
            // there is no denominator to guard.
            axisX.min = Math.max(0, GraphDataModel.latestTime - GraphDataModel.windowSeconds)
            axisX.max = Math.max(GraphDataModel.windowSeconds, GraphDataModel.latestTime)
        }

        if (root.manualScale) {
            root.applyManualScale()
            return
        }
        if (!root.autoScale)
            return

        let low = GraphDataModel.paneDataMinimum(root.paneId)
        let high = GraphDataModel.paneDataMaximum(root.paneId)
        if (!(high > low)) {
            high = low + 1
        } else {
            // A little air top and bottom, so a trace at its extreme is not
            // drawn along the frame.
            const pad = (high - low) * 0.08
            low -= pad
            high += pad
        }
        axisY.min = low
        axisY.max = high
    }

    /*! Pins Y to the typed range. Assigned, never bound - the same rule the
        rest of this file follows, because a binding would be broken by the
        first mouse zoom and then silently stop tracking the fields. */
    function applyManualScale() {
        let low = root.manualMin
        let high = root.manualMax
        // An inverted or empty range makes the chart draw nothing at all, with
        // no hint as to why. Widen it instead of rejecting the input, so a
        // half-typed number never blanks the pane.
        if (!(high > low))
            high = low + 1
        axisY.min = low
        axisY.max = high
    }

    // ------------------------------------------------------------- tooltip
    /*!
        Shows the value under the cursor for one series.

        \a point arrives in DATA coordinates, so it has to be projected back
        through \c {chart.mapToPosition(value, series)} to find the pixel. The
        cursor position would be the easy alternative and the wrong one: the
        chart snaps the hover to the nearest sample, and a readout that floats
        with the mouse rather than sitting on the point it describes tells you
        nothing about which sample you are reading.
    */
    function showPointTooltip(series, point, state) {
        if (!state) {
            pointTooltip.visible = false
            return
        }

        const pos = chart.mapToPosition(point, series)
        if (!pos || isNaN(pos.x) || isNaN(pos.y)) {
            pointTooltip.visible = false
            return
        }

        pointTooltip.seriesName = series.name
        pointTooltip.swatch = series.color
        // Three decimals, like every other value readout in the application.
        pointTooltip.body = qsTr("X: %1 s\nY: %2")
                            .arg(Number(point.x).toFixed(3))
                            .arg(Number(point.y).toFixed(3))

        // Prefer up and to the right of the sample, then fold back inside the
        // pane rather than letting the box hang off an edge - in a 2x2 split
        // there is not much pane to hang off.
        const gap = Theme.spacingSm
        let px = pos.x + gap
        let py = pos.y - pointTooltip.height - gap
        if (px + pointTooltip.width > root.width - gap)
            px = pos.x - pointTooltip.width - gap
        if (py < gap)
            py = pos.y + gap
        pointTooltip.x = Math.max(gap, Math.min(px, root.width - pointTooltip.width - gap))
        pointTooltip.y = Math.max(gap, Math.min(py, root.height - pointTooltip.height - gap))
        pointTooltip.visible = true
    }

    /*! Value on \a axis at pixel \a position along the plot area. Exact for a
        linear axis, and it needs no series - mapToValue() does. */
    function valueAt(axis, position, vertical) {
        const area = chart.plotArea
        if (vertical) {
            if (area.height <= 0)
                return (axis.min + axis.max) / 2
            const fraction = (position - area.y) / area.height
            return axis.max - fraction * (axis.max - axis.min)
        }
        if (area.width <= 0)
            return (axis.min + axis.max) / 2
        return axis.min + ((position - area.x) / area.width) * (axis.max - axis.min)
    }

    /*! Scales \a axis by \a factor about \a anchor, so the value under the
        cursor stays under the cursor. */
    function zoomAxis(axis, factor, anchor) {
        const low = anchor - (anchor - axis.min) * factor
        const high = anchor + (axis.max - anchor) * factor
        if (high > low) {
            axis.min = low
            axis.max = high
        }
    }

    function panAxis(axis, delta) {
        axis.min += delta
        axis.max += delta
    }

    /*! Applies a dragged rectangle (in pane pixels) to both axes. */
    function zoomToBox(x1, y1, x2, y2) {
        // A box zoom taken while paused holds the axes until resume, the same
        // as the wheel and the pan.
        if (GraphDataModel.paused)
            root.zoomOverride = true
        const area = chart.plotArea
        if (area.width <= 0 || area.height <= 0)
            return

        // Clamp to the plot area: a drag that ran off the edge should zoom to
        // the edge, not to an extrapolated value outside the data.
        const left = Math.max(area.x, Math.min(x1, x2))
        const right = Math.min(area.x + area.width, Math.max(x1, x2))
        const top = Math.max(area.y, Math.min(y1, y2))
        const bottom = Math.min(area.y + area.height, Math.max(y1, y2))
        if (right <= left || bottom <= top)
            return

        const xLow = root.valueAt(axisX, left, false)
        const xHigh = root.valueAt(axisX, right, false)
        // Y is inverted: the TOP of the box is the HIGHER value.
        const yHigh = root.valueAt(axisY, top, true)
        const yLow = root.valueAt(axisY, bottom, true)

        if (xHigh > xLow) {
            axisX.min = xLow
            axisX.max = xHigh
        }
        if (yHigh > yLow) {
            axisY.min = yLow
            axisY.max = yHigh
        }
    }

    // ---------------------------------------------------------------- chart
    ChartView {
        id: chart

        // NOT anchors.fill. The footer is an overlay drawn at z:2, and while
        // the chart filled the pane its X-axis tick labels were rendered
        // underneath it - "0.000" and "2.500" landed directly on top of the
        // Auto Scale row. The two were never fighting each other; the chart
        // was painting into their space. Stopping the chart at the footer's
        // top edge is what removes the collision, and it costs nothing: the
        // plot area simply gets the height it was already visually confined
        // to.
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: footerBar.top
        anchors.margins: 1
        anchors.bottomMargin: 0
        z: 0                     // explicit: the input layering below matters
        antialiasing: true
        theme: ChartView.ChartThemeDark
        backgroundColor: "transparent"
        plotAreaColor: "transparent"
        backgroundRoundness: 0
        dropShadowEnabled: false
        legend.visible: false
        localizeNumbers: false
        margins.top: Theme.spacingSm
        margins.bottom: Theme.spacingXs
        margins.left: Theme.spacingXs
        margins.right: Theme.spacingSm

        // NOTE: no `rubberBand` here. That property belongs to QChartView, the
        // QWidget class - the QML ChartView type has no such property and no
        // built-in drag interaction at all. Box zoom is drawn and applied by
        // paneInput below.

        ValueAxis {
            id: axisX
            min: 0
            max: 10
            tickCount: 5
            labelFormat: "%.3f"
            labelsColor: Theme.textSecondary
            labelsFont.family: Theme.fontFamily
            labelsFont.pixelSize: Theme.fontSizeSm
            color: Theme.border
            gridLineColor: Theme.gridLine
            minorGridVisible: false
            titleVisible: false
        }

        ValueAxis {
            id: axisY
            min: 0
            max: 1
            tickCount: 5
            labelFormat: "%.3f"
            labelsColor: Theme.textSecondary
            labelsFont.family: Theme.fontFamily
            labelsFont.pixelSize: Theme.fontSizeSm
            color: Theme.border
            gridLineColor: Theme.gridLine
            minorGridVisible: false
            titleVisible: false
        }
    }

    // ---------------------------------------------------------------- input
    //
    // A MouseArea ON TOP of the ChartView, not a TapHandler on the pane root.
    // ChartView calls setAcceptedMouseButtons(AllButtons) for its own zoom and
    // pan, so it takes an exclusive grab on press and nothing behind it sees
    // the event.
    //
    // z sits between the chart (0) and the furniture (2), so the legend's
    // remove buttons and the Auto Scale box still get their clicks first.
    MouseArea {
        id: paneInput

        anchors.fill: parent
        z: 1
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton

        property real lastPanX: 0
        property real lastPanY: 0

        /*! Box-zoom drag state. */
        property bool selecting: false
        property real selectStartX: 0
        property real selectStartY: 0

        onPressed: function(mouse) {
            root.activated()

            if (mouse.button === Qt.MiddleButton) {
                paneInput.lastPanX = mouse.x
                paneInput.lastPanY = mouse.y
                return
            }
            if (mouse.button === Qt.LeftButton && root.interactiveAxes) {
                paneInput.selecting = true
                paneInput.selectStartX = mouse.x
                paneInput.selectStartY = mouse.y
            }
            // Every button is accepted. The left one used to be declined so it
            // would reach the ChartView's rubber band - which never existed -
            // and declining it also meant onDoubleClicked never fired.
        }

        onPositionChanged: function(mouse) {
            if (paneInput.selecting) {
                selectionBox.updateFrom(paneInput.selectStartX, paneInput.selectStartY,
                                        mouse.x, mouse.y)
                return
            }
            if (!root.interactiveAxes || !(mouse.buttons & Qt.MiddleButton))
                return
            // A pan while paused takes the axes off Auto/Manual until resume.
            if (GraphDataModel.paused)
                root.zoomOverride = true

            const area = chart.plotArea
            if (area.width <= 0 || area.height <= 0)
                return

            root.panAxis(axisX, -(mouse.x - paneInput.lastPanX)
                                / area.width * (axisX.max - axisX.min))
            root.panAxis(axisY, (mouse.y - paneInput.lastPanY)
                                / area.height * (axisY.max - axisY.min))
            paneInput.lastPanX = mouse.x
            paneInput.lastPanY = mouse.y
        }

        onReleased: function(mouse) {
            if (!paneInput.selecting)
                return
            paneInput.selecting = false
            selectionBox.visible = false

            // Below this it was a click, not a drag - zooming to a two-pixel
            // box would throw the view somewhere unrecoverable.
            const minimumDrag = 8
            if (Math.abs(mouse.x - paneInput.selectStartX) < minimumDrag
                    || Math.abs(mouse.y - paneInput.selectStartY) < minimumDrag) {
                return
            }
            root.zoomToBox(paneInput.selectStartX, paneInput.selectStartY, mouse.x, mouse.y)
        }

        onClicked: function(mouse) {
            if (mouse.button === Qt.RightButton)
                paneMenu.popup()
        }

        // Escape hatch: whatever the mouse did to the axes, this puts them back.
        onDoubleClicked: function(mouse) {
            root.zoomOverride = false
            autoScaleBox.checked = true
            root.applyAutoScale()
        }

        onWheel: function(wheel) {
            if (!root.interactiveAxes) {
                // Nothing to zoom - the next repaint would overwrite it.
                wheel.accepted = false
                return
            }
            if (GraphDataModel.paused)
                root.zoomOverride = true
            const factor = wheel.angleDelta.y > 0 ? 0.85 : (1.0 / 0.85)
            if (wheel.modifiers & Qt.ShiftModifier)
                root.zoomAxis(axisX, factor, root.valueAt(axisX, wheel.x, false))
            else
                root.zoomAxis(axisY, factor, root.valueAt(axisY, wheel.y, true))
            wheel.accepted = true
        }
    }

    // -- box-zoom marquee, drawn by us because ChartView draws nothing
    Rectangle {
        id: selectionBox

        z: 2
        visible: false
        color: Theme.accent
        opacity: 0.18
        border.width: 1
        border.color: Theme.accent

        function updateFrom(x1, y1, x2, y2) {
            selectionBox.x = Math.min(x1, x2)
            selectionBox.y = Math.min(y1, y2)
            selectionBox.width = Math.abs(x2 - x1)
            selectionBox.height = Math.abs(y2 - y1)
            selectionBox.visible = true
        }
    }

    // ------------------------------------------------------- pane furniture
    Label {
        z: 2
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: Theme.spacingXs
        text: root.title
        color: Theme.textDisabled
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeSm
    }

    /*!
        Floating readout for the hovered sample.

        A plain Rectangle, not a Controls ToolTip: a ToolTip is positioned
        relative to the item it is attached to and shown through a timer and a
        shared popup instance, none of which suits a thing that has to sit on
        an arbitrary pixel inside the plot and follow a hover that changes
        several times a second. z is above every other piece of furniture -
        including the close button - because it is transient and must never be
        the thing that is covered.

        No MouseArea and no hover handling of its own: anything that accepted
        input here would sit between the cursor and the chart and cancel the
        very hover that summoned it.
    */
    Rectangle {
        id: pointTooltip

        property string seriesName: ""
        property string body: ""
        property color swatch: Theme.accent

        z: 4
        visible: false
        width: tooltipContent.implicitWidth + 2 * Theme.spacingSm
        height: tooltipContent.implicitHeight + 2 * Theme.spacingXs
        radius: Theme.radiusSm
        color: Theme.panel
        opacity: 0.96
        border.width: 1
        border.color: Theme.border

        Column {
            id: tooltipContent
            x: Theme.spacingSm
            y: Theme.spacingXs
            spacing: 1

            Row {
                spacing: Theme.spacingXs
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 8
                    height: 8
                    radius: 2
                    color: pointTooltip.swatch
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: pointTooltip.seriesName
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeSm
                }
            }

            Label {
                text: pointTooltip.body
                color: Theme.textPrimary
                // Monospace so the two lines' digits align and a changing value
                // does not make the box jitter as its width follows the text.
                font.family: Theme.monoFontFamily
                font.pixelSize: Theme.fontSizeSm
            }
        }
    }

    // -- close, top right corner
    Rectangle {
        id: closeButton

        z: 3
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Theme.spacingXs
        width: 18
        height: 18
        radius: Theme.radiusSm
        // Hidden, not disabled, on the last pane: a permanently greyed X in
        // every corner is noise. The menu entry still explains itself.
        visible: root.canClose
        color: closeArea.containsPress ? Theme.closeHoverPressed
             : closeArea.containsMouse ? Theme.closeHover
                                       : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

        Label {
            anchors.centerIn: parent
            text: "\u2715"
            color: closeArea.containsMouse ? Theme.textPrimary : Theme.textDisabled
            font.pixelSize: Theme.fontSizeSm
        }

        MouseArea {
            id: closeArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            onClicked: root.closeRequested()
            ToolTip.visible: containsMouse
            ToolTip.text: qsTr("Close this pane")
            ToolTip.delay: 600
        }
    }

    // -- legend / live readout for this pane
    Column {
        id: legend

        z: 2
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Theme.spacingSm
        // Clear of the close button, which sits in the same corner.
        anchors.topMargin: Theme.spacingSm + (root.canClose ? 18 : 0)
        spacing: 1

        Repeater {
            // tracksVersion in the binding: tracksForPane() is an invokable, so
            // without a dependency that changes it would evaluate once.
            model: root.tracksVersion >= 0 ? GraphDataModel.tracksForPane(root.paneId) : []

            delegate: Row {
                id: legendRow

                required property int index
                required property var modelData

                spacing: Theme.spacingXs

                Rectangle {
                    width: 10
                    height: 10
                    radius: 2
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.seriesColor(legendRow.modelData.colorIndex)
                }
                Label {
                    text: legendRow.modelData.name
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeMd
                }
                Label {
                    // refreshTick, not tracksVersion: this one has to move at
                    // the repaint rate. C++ formats it to three decimals.
                    text: GraphDataModel.refreshTick >= 0
                          ? GraphDataModel.lastValueText(legendRow.modelData.trackId)
                          : ""
                    color: Theme.textPrimary
                    font.family: Theme.monoFontFamily
                    // The one number in the pane that gets read from across a
                    // desk rather than studied, so it outsizes its own label.
                    font.pixelSize: Theme.fontSizeReadout
                    font.weight: Font.Bold
                }
                Label {
                    text: "✕"
                    color: removeHover.hovered ? Theme.error : Theme.textDisabled
                    // Grown with the row it sits in, or it becomes a hit
                    // target too small to aim at next to 15 px text.
                    font.pixelSize: Theme.fontSizeMd
                    HoverHandler { id: removeHover }
                    TapHandler {
                        onSingleTapped: root.removeTrack(legendRow.modelData.trackId)
                    }
                }
            }
        }
    }

    // -- footer: scaling controls on the left, mouse hints on the right
    //
    // A RowLayout anchored to BOTH sides, not a Row anchored only to the left.
    // A Row is as wide as its contents, so the hint text ran off the right
    // edge of a narrow pane with nothing to stop it; and because the Row had
    // no reserved height, the chart above was free to draw its axis labels
    // straight through it. Now the strip owns a band of the pane, the chart
    // ends where it begins, and the hint gets Layout.fillWidth so it is the
    // element that gives way when a pane is split narrow.
    RowLayout {
        id: footerBar

        z: 2
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.spacingXs
        spacing: Theme.spacingXs

        /*! Guards the two checkboxes against each other. Each writes the
            other's `checked`, so without this the pair ping-pongs once per
            click and can settle in the wrong state. */
        property bool syncing: false

        ThemedCheckBox {
            id: autoScaleBox

            Layout.alignment: Qt.AlignVCenter
            text: qsTr("Auto Scale")
            checked: true
            // onCheckedChanged, not onToggled: the context-menu entry and the
            // double-click reset both set `checked` programmatically, and
            // onToggled is only guaranteed for interactive changes.
            onCheckedChanged: {
                root.autoScale = checked
                if (checked && !footerBar.syncing) {
                    footerBar.syncing = true
                    manualScaleBox.checked = false
                    footerBar.syncing = false
                }
                // Turning it back ON snaps to the data immediately rather than
                // waiting for the next repaint.
                root.applyAutoScale()
            }
        }

        ThemedCheckBox {
            id: manualScaleBox

            Layout.alignment: Qt.AlignVCenter
            text: qsTr("Manual Scale")
            checked: false
            onCheckedChanged: {
                root.manualScale = checked
                if (checked && !footerBar.syncing) {
                    footerBar.syncing = true
                    autoScaleBox.checked = false
                    footerBar.syncing = false
                    // Seed the fields from what is on screen, so switching to
                    // manual holds the current view instead of jumping to an
                    // unrelated default the user then has to undo.
                    root.manualMin = axisY.min
                    root.manualMax = axisY.max
                }
                root.applyAutoScale()
            }
        }

        // -- the two range fields, manual mode only
        ThemedTextField {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 78
            visible: root.manualScale
            text: Number(root.manualMin).toFixed(3)
            horizontalAlignment: Text.AlignRight
            validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
            onEditingFinished: {
                const parsed = parseFloat(text)
                if (isNaN(parsed)) {
                    // Put the old number back rather than writing NaN, which
                    // would collapse the axis and blank the pane.
                    text = Qt.binding(function () { return Number(root.manualMin).toFixed(3) })
                    return
                }
                root.manualMin = parsed
                root.applyAutoScale()
            }
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Y axis minimum")
        }

        Label {
            Layout.alignment: Qt.AlignVCenter
            visible: root.manualScale
            text: "\u2026"
            color: Theme.textDisabled
            font.pixelSize: Theme.fontSizeSm
        }

        ThemedTextField {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 78
            visible: root.manualScale
            text: Number(root.manualMax).toFixed(3)
            horizontalAlignment: Text.AlignRight
            validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
            onEditingFinished: {
                const parsed = parseFloat(text)
                if (isNaN(parsed)) {
                    text = Qt.binding(function () { return Number(root.manualMax).toFixed(3) })
                    return
                }
                root.manualMax = parsed
                root.applyAutoScale()
            }
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Y axis maximum")
        }

        // Absorbs the slack, so the checkboxes keep their natural width and
        // this is what shrinks first. Right-aligned inside its own box so the
        // hints sit against the pane edge rather than drifting with the
        // width of whatever is to their left.
        Label {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            horizontalAlignment: Text.AlignRight
            // Only in Free mode - in Auto or Manual the mouse hints would be
            // describing gestures that the next repaint overwrites.
            visible: !root.autoScale && !root.manualScale
            text: qsTr("wheel: Y  ·  shift+wheel: X  ·  drag: box zoom  ·  "
                       + "middle-drag: pan  ·  double-click: reset")
            // Accent, not textDisabled. These are the only affordances in the
            // pane that nothing on screen advertises, and grey-on-dark read as
            // a disabled caption rather than as something to try.
            color: Theme.accent
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSm
            elide: Text.ElideRight
        }
    }

    // -- empty state
    Label {
        z: 2
        anchors.centerIn: parent
        visible: root.tracksVersion >= 0
                 && GraphDataModel.tracksForPane(root.paneId).length === 0
        text: qsTr("Right-click to add a signal")
        color: Theme.textDisabled
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeMd
    }

    // -- transient message
    Label {
        z: 2
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.left: parent.horizontalCenter
        anchors.margins: Theme.spacingXs
        visible: root.notice.length > 0
        text: root.notice
        color: Theme.error
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeSm
        horizontalAlignment: Text.AlignRight
        wrapMode: Text.WrapAnywhere
        maximumLineCount: 2
        elide: Text.ElideRight
    }

    // ------------------------------------------------------- context menu
    // Opened from paneInput above - see the note there about why a handler on
    // this item could never see the press.
    Menu {
        id: paneMenu

        background: Rectangle {
            implicitWidth: 220
            color: Theme.panel
            radius: Theme.radiusSm
            border.width: 1
            border.color: Theme.border
        }

        PaneMenuItem {
            text: qsTr("Add Signal...")
            enabled: GraphDataModel.databaseCount > 0
            onTriggered: root.addSignalRequested()
        }
        PaneMenuItem {
            text: qsTr("Clear this pane")
            enabled: root.tracksVersion >= 0
                     && GraphDataModel.tracksForPane(root.paneId).length > 0
            onTriggered: root.clearPane()
        }
        PaneMenuItem {
            // Always available: an empty pane with its axes and title is
            // still a legitimate thing to want a picture of.
            text: qsTr("Export as Image...")
            onTriggered: exportDialog.open()
        }

        MenuSeparator {
            contentItem: Rectangle {
                implicitHeight: 1
                color: Theme.border
            }
        }

        PaneMenuItem {
            text: root.autoScale ? qsTr("Auto scale   ✓") : qsTr("Auto scale")
            onTriggered: autoScaleBox.checked = !autoScaleBox.checked
        }
        PaneMenuItem {
            text: root.manualScale ? qsTr("Manual scale   ✓") : qsTr("Manual scale")
            onTriggered: manualScaleBox.checked = !manualScaleBox.checked
        }

        MenuSeparator {
            contentItem: Rectangle {
                implicitHeight: 1
                color: Theme.border
            }
        }

        // Named by WHERE THE NEW PANE GOES, not by the divider's orientation.
        // "Split vertically" is genuinely ambiguous - it reads as both "a
        // vertical divider" and "stack them vertically", and the two are
        // opposites. A direction cannot be misread.
        PaneMenuItem {
            text: qsTr("Split Left   ◧")
            enabled: root.canSplit
            onTriggered: root.splitRequested("left")
        }
        PaneMenuItem {
            text: qsTr("Split Right   ◨")
            enabled: root.canSplit
            onTriggered: root.splitRequested("right")
        }
        PaneMenuItem {
            text: qsTr("Split Up   ⬒")
            enabled: root.canSplit
            onTriggered: root.splitRequested("up")
        }
        PaneMenuItem {
            text: qsTr("Split Down   ⬓")
            enabled: root.canSplit
            onTriggered: root.splitRequested("down")
        }

        MenuSeparator {
            contentItem: Rectangle {
                implicitHeight: 1
                color: Theme.border
            }
        }

        PaneMenuItem {
            text: qsTr("Close pane")
            enabled: root.canClose
            onTriggered: root.closeRequested()
        }
    }

    // Inline components live at the top level of the document - one declared
    // inside the Menu that uses it is a level too deep to parse.
    component PaneMenuItem: MenuItem {
        id: paneMenuEntry

        implicitHeight: Theme.inputHeight
        implicitWidth: 220

        contentItem: Label {
            text: paneMenuEntry.text
            color: !paneMenuEntry.enabled
                   ? Theme.textDisabled
                   : (paneMenuEntry.highlighted ? Theme.selectionText : Theme.textPrimary)
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeMd
            verticalAlignment: Text.AlignVCenter
            leftPadding: Theme.spacingSm
            elide: Text.ElideRight
        }
        background: Rectangle {
            color: paneMenuEntry.highlighted ? Theme.selection : "transparent"
        }
    }
}
