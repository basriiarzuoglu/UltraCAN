import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import QtQuick.Controls.Basic

import UltraCAN.Theme
import UltraCAN.Components

/*!
    \qmltype GraphWorkspace
    \brief One graph tab: a splittable pane area that can live inline or in a
           window of its own.

    \section1 Why this is a type and not code inside GraphPlotView

    Detaching used to be a property of the PAGE - one \c detached flag, one
    floating \c Window, one snapshot buffer. With several workspaces open that
    model breaks down immediately: "is the chart detached" stops being a single
    answer, and the re-attach path needs to know which tab it is going back to.

    Making the workspace the unit fixes both by construction. Each instance
    owns its own window, its own handover buffer and its own detached flag, and
    "re-attach at the original index" needs no bookkeeping at all: the inline
    half never left the StackLayout, so the tab it returns to is the tab it was
    always in.

    \section1 Two areas, two namespaces

    While detached, BOTH areas exist: the inline one is only hidden - its panes
    are alive behind the placeholder. So the two must not be able to name the
    same pane. \c GraphDataModel keys tracks on the pane-id string, in one flat
    table for the whole process, and two areas answering to "W1:P1" would be
    one owner: the detached area's \c restore() destroys its old panes first,
    and each \c untrackPane("W1:P1") would reach across and delete a trace
    belonging to the inline half. Measured cost of getting this wrong: exactly
    one trace lost per handover.

    Hence the \c "d" - inline mints \c {W1:Pn}, floating mints \c {W1d:Pn}, and
    \l GraphPaneArea::restore() re-keys the snapshot into whichever namespace
    it is landing in. The \l Loader on the floating half is then a plain
    lifetime optimisation rather than a correctness device: a workspace that
    has never been detached carries no second area.

    \section1 Handover, not copy

    \l detach() snapshots the layout + traces, CLEARS the source, then flips
    \l detached. The clear is load-bearing: panes keep their traces while merely
    hidden, so without it the rebuilt panes re-create the same signals and
    \c GraphDataModel feeds two series per trace.
*/
Item {
    id: root

    /*! Stable identity, used as the pane-id namespace. Supplied by the host. */
    required property string workspaceId
    /*! Label shown on the tab. */
    property string name: ""

    /*! True while this workspace's charts live in their own window. */
    property bool detached: false

    /*! The area currently holding this workspace's panes - inline or floating.
        Null for the single event-loop turn in which the Loader is swapping. */
    readonly property var area: root.detached ? detachedLoader.item : inlineArea
    /*! Live pane count, for the page subtitle. */
    readonly property int paneCount: root.area ? root.area.paneCount : 0

    /*! The pane ids currently in this workspace, in layout order. Empty while
        a detach handover is swapping the Loader. */
    function paneIds() {
        return root.area ? root.area.leafIds(root.area.layoutTree) : []
    }

    /*! Plots \a channelId on \a paneId. Returns false when that pane is not
        in this workspace - which happens if the menu was built before a pane
        was closed under it. */
    function addMathChannelTo(paneId, channelId) {
        const pane = root.area ? root.area.paneById(paneId) : null
        if (!pane)
            return false
        pane.addMathChannel(channelId)
        return true
    }

    /*! Emitted with a line for the page's status strip. */
    signal statusPosted(string text)

    /*! Carries layout + traces across the teardown/rebuild of a handover. */
    property var pendingState: null

    /*!
        Set while a handover is driving graphWindow.close() itself, so the
        window's onClosing can tell that apart from the user pressing the title
        bar's X - which must instead be turned into a re-attach.
    */
    property bool handoverInProgress: false

    // ------------------------------------------------------------- handover
    function detach() {
        if (root.detached)
            return

        root.pendingState = inlineArea.snapshot()
        inlineArea.clearAll()

        root.detached = true
        graphWindow.show()
        graphWindow.raise()
        graphWindow.requestActivate()
        root.statusPosted(qsTr("“%1” moved to its own window.").arg(root.name))
    }

    function reattach() {
        if (!root.detached)
            return

        const source = detachedLoader.item
        root.pendingState = source ? source.snapshot() : null
        if (source)
            source.clearAll()

        // ORDER MATTERS. close() delivers onClosing synchronously, and that
        // handler vetoes the close whenever `detached` is still true - so
        // closing first left the window open, re-entered this function, and
        // produced an empty floating window that nothing could shut. Clear the
        // flag, mark the handover, then close.
        root.detached = false
        root.handoverInProgress = true
        graphWindow.close()
        root.handoverInProgress = false

        root.statusPosted(qsTr("“%1” returned to the main window.").arg(root.name))
    }

    // ------------------------------------------------- session export / import
    /*! This tab as plain data: its label plus whichever area currently holds
        the panes. \c detached is recorded but NOT restored - see applyState(). */
    function exportState() {
        const source = root.area
        let out = { name: root.name, detached: root.detached }
        if (source) {
            const areaState = source.exportState()
            out.tree = areaState.tree
        }
        return out
    }

    /*! Rebuilds this tab. Returns the number of signals that could not be
        resolved.

        A tab saved while detached comes back INLINE. Restoring the floating
        window would mean guessing its position and size on a machine whose
        monitors may not be the ones it was saved on, and a window that opens
        off-screen is a window the user cannot find. Detach is one click away.
    */
    function applyState(state) {
        if (!state)
            return 0
        if (state.name !== undefined && String(state.name).length > 0)
            root.name = String(state.name)

        // Always land inline, so the target area is the one that is definitely
        // alive - a reattach here would have to survive its own callLater.
        if (root.detached) {
            root.reattach()
            // reattach() queues a restore of the panes it just took out of the
            // window. Those are about to be replaced wholesale, and letting
            // the queued handler run would put them back one turn AFTER
            // applyState() finished - the imported layout would appear and
            // then be overwritten by the old one.
            root.pendingState = null
        }
        return inlineArea.applyState({ tree: state.tree })
    }

    /*! Everything that must happen before this workspace is destroyed: the
        floating window has no page behind it once we are gone, and the tracks
        must go with the series rather than outliving them in C++.

        Deliberately NOT GraphDataModel.untrackAll() - that is a process-wide
        sweep and the other tabs are still plotting. */
    function shutdown() {
        root.handoverInProgress = true
        graphWindow.close()

        if (detachedLoader.item)
            detachedLoader.item.clearAll()
        inlineArea.clearAll()
    }

    Component.onDestruction: root.shutdown()

    // The rebuild has to happen AFTER the destination exists and is visible,
    // which is one event loop turn later than the button click. Watching
    // `detached` rather than doing it inline is what makes both directions -
    // detach and re-attach - go through the same three lines.
    onDetachedChanged: Qt.callLater(function () {
        const state = root.pendingState
        root.pendingState = null
        if (!state)
            return
        const target = root.area
        if (target)
            target.restore(state)
    })

    // ---------------------------------------------------------- inline half
    Rectangle {
        anchors.fill: parent
        color: Theme.background

        GraphPaneArea {
            id: inlineArea
            anchors.fill: parent
            anchors.margins: Theme.spacingSm
            paneIdPrefix: root.workspaceId + ":"
            visible: !root.detached
        }

        // -- placeholder while this workspace's charts are elsewhere
        ColumnLayout {
            anchors.centerIn: parent
            visible: root.detached
            spacing: Theme.spacingSm

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: "⤢"
                color: Theme.textDisabled
                font.pixelSize: 40
            }
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("“%1” is in its own window.").arg(root.name)
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeMd
            }
            ToolbarButton {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Bring it back")
                primary: true
                onClicked: root.reattach()
            }
        }
    }

    // -------------------------------------------------------- floating half
    Window {
        id: graphWindow

        width: 1100
        height: 700
        minimumWidth: 480
        minimumHeight: 320
        title: qsTr("UltraCAN - %1").arg(root.name)
        color: Theme.background

        // The title bar's X must mean "re-attach", not "make the chart
        // disappear with no way back". A close we asked for ourselves is let
        // through untouched.
        onClosing: function (close) {
            if (root.handoverInProgress)
                return
            if (root.detached) {
                close.accepted = false
                root.reattach()
            }
        }

        Loader {
            id: detachedLoader
            anchors.fill: parent
            anchors.margins: Theme.spacingSm
            // Synchronous on purpose: detach() flips `detached` and the very
            // next event loop turn calls restore() on whatever `area` resolves
            // to. An async Loader would still be null at that point and the
            // snapshot would be dropped on the floor.
            asynchronous: false
            active: root.detached

            // "W1d:", NOT "W1:" - see the class note. The inline area is
            // still alive behind the placeholder and still owns "W1:Pn".
            sourceComponent: GraphPaneArea {
                paneIdPrefix: root.workspaceId + "d:"
            }
        }
    }
}
