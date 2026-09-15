import QtQuick
import QtQuick.Controls.Basic

import UltraCAN.Theme
import UltraCAN.Core

/*!
    \qmltype GraphPaneArea
    \brief The scope surface: any number of panes, split any way, to any depth.

    \section1 What replaced the four fixed slots

    This used to be four panes in two nested SplitViews driven by a
    \c layoutMode integer, and its own documentation argued that recursion was
    "the general answer and the wrong one here". Two reasons were given. The
    first - that the layout is capped at four panes - no longer holds: the cap
    is gone. The second still holds completely, and is what shapes this file:

    \b {splitting must not destroy the pane you already filled.}

    A pane owns a ChartView, its series, and every point plotted into them. Any
    design where splitting rebuilds the pane throws all three away, and the
    user watches their traces vanish and slowly refill. So the tree here is
    built from two halves that are deliberately kept apart:

    \list
    \li \b {The pool} - \c panePool, paneId -> a live SplittableGraphPane.
        Panes are created when a split adds one and destroyed only when the
        user closes one. Re-laying out the tree never touches them.
    \li \b {The tree} - a plain JS structure of leaf and split nodes. It says
        where each pane goes; it holds no items.
    \endlist

    \l rebuildLayout() throws away the SCAFFOLDING - the GraphSplitNodes - and
    rebuilds it from the tree, reparenting the pooled panes into the new slots.
    Scaffolding is cheap and holds no state; panes are expensive and hold all
    of it. Nothing a pane owns crosses a rebuild.

    \section1 The tree

    \code
    leaf  = { kind: "leaf",  paneId: "P3" }
    split = { kind: "split", vertical: bool, ratio: 0.5, a: node, b: node }
    \endcode

    \c vertical false = side by side, true = stacked. Split directions map onto
    it by choosing the axis and which side the NEW pane lands on: left and up
    put it in slot a, right and down in slot b.

    Closing a leaf replaces its PARENT with its sibling, so the sibling
    inherits exactly the space the pair occupied. The last remaining pane
    cannot be closed - there would be nothing to show.

    \section1 Snapshot / restore

    Still here for the detach button, and now serialising the tree rather than
    one integer. Moving live ChartViews between windows means moving items
    between scene graphs, which works until it does not; rebuilding them in the
    new window is predictable.
*/
Item {
    id: root

    /*! Which pane the last interaction was in - the one the toolbar acts on. */
    property string activePaneId: ""
    /*! Bumped on every structural change.

        Not decorative: \l layoutTree is a `var`, and QML compares those by
        REFERENCE. Mutating a node in place and re-assigning the same object
        notifies nothing, so a binding onto the tree alone would go stale the
        moment a split edited it. Cloning the tree instead would notify, but
        would also orphan the node objects the split handles write their new
        ratio back into. An explicit counter avoids both. */
    property int layoutVersion: 0

    /*! Live pane count, for the toolbar and the close guards. */
    readonly property int paneCount: {
        root.layoutVersion
        return root.leafIds(root.layoutTree).length     // leafIds(null) -> []
    }

    /*! Namespace for the pane ids this area mints.

        \c GraphDataModel keys tracks on the paneId STRING, in one flat table
        shared by every area in the process. Two areas both starting at "P1"
        would therefore not merely look alike - they would be the same owner:
        \c tracksForPane() would hand each the other's traces and closing one
        would untrack the other's series. One workspace tab = one prefix is
        what keeps them apart. Empty for a lone area, where it costs nothing. */
    property string paneIdPrefix: ""

    /*! paneId -> SplittableGraphPane. Never rebuilt by a layout change. */
    property var panePool: ({})
    /*! The layout tree. Replaced wholesale on every change so that anything
        binding to it re-evaluates - mutating it in place notifies nothing.

        Deliberately EMPTY here, not a leaf named "P1": the first leaf has to
        carry the prefixed id, and the prefix is not known until the property
        assignments finish. Seeding it with a bare "P1" left the tree naming a
        pane the pool had filed as "W2:P1", so mountNode() found nothing to
        mount and findLeaf() failed every split. Component.onCompleted plants
        the real first leaf. */
    property var layoutTree: null
    property int nextPaneSerial: 2

    // ------------------------------------------------------------ tree walks
    function leafIds(node) {
        if (!node)
            return []
        if (node.kind === "leaf")
            return [node.paneId]
        return leafIds(node.a).concat(leafIds(node.b))
    }

    /*! Returns {parent, side} for the leaf owning \a paneId, or null. parent is
        null when the leaf IS the root. */
    function findLeaf(node, paneId, parent, side) {
        if (!node)
            return null
        if (node.kind === "leaf")
            return node.paneId === paneId ? { parent: parent, side: side } : null
        return findLeaf(node.a, paneId, node, "a") || findLeaf(node.b, paneId, node, "b")
    }

    /*! Returns {parent, side} for the node \a target itself. */
    function findNode(node, target, parent, side) {
        if (!node)
            return null
        if (node === target)
            return { parent: parent, side: side }
        if (node.kind === "leaf")
            return null
        return findNode(node.a, target, node, "a") || findNode(node.b, target, node, "b")
    }

    // ------------------------------------------------------------ mutations
    /*! \a direction is one of "left", "right", "up", "down". */
    function splitPane(paneId, direction) {
        const hit = root.findLeaf(root.layoutTree, paneId, null, "")
        if (!hit)
            return

        const fresh = root.createPane()
        const vertical = (direction === "up" || direction === "down")
        const newFirst = (direction === "left" || direction === "up")

        // The EXISTING pane keeps its id, so the pool entry - and with it the
        // ChartView and every plotted point - carries straight over.
        const keep = { kind: "leaf", paneId: paneId }
        const added = { kind: "leaf", paneId: fresh }
        const node = {
            kind: "split",
            vertical: vertical,
            ratio: 0.5,
            a: newFirst ? added : keep,
            b: newFirst ? keep : added
        }

        if (!hit.parent)
            root.layoutTree = node
        else
            hit.parent[hit.side] = node

        root.layoutVersion += 1
        root.activePaneId = fresh
        root.rebuildLayout()
    }

    function closePane(paneId) {
        if (!root.layoutTree || root.layoutTree.kind === "leaf")
            return                              // the last pane stays

        const hit = root.findLeaf(root.layoutTree, paneId, null, "")
        if (!hit || !hit.parent)
            return

        const parent = hit.parent
        const sibling = parent[hit.side === "a" ? "b" : "a"]

        const grand = root.findNode(root.layoutTree, parent, null, "")
        if (!grand || !grand.parent)
            root.layoutTree = sibling           // sibling becomes the whole area
        else
            grand.parent[grand.side] = sibling

        root.destroyPane(paneId)

        if (root.activePaneId === paneId) {
            const remaining = root.leafIds(root.layoutTree)
            root.activePaneId = remaining.length > 0 ? remaining[0] : ""
        }
        root.layoutVersion += 1
        root.rebuildLayout()
    }

    // -------------------------------------------------------- the pane pool
    function createPane() {
        const paneId = root.paneIdPrefix + "P" + root.nextPaneSerial
        root.nextPaneSerial += 1
        const pane = paneComponent.createObject(detachedPanes, { paneId: paneId })
        if (!pane) {
            console.warn("GraphPaneArea: could not create a pane")
            return ""
        }
        root.panePool[paneId] = pane
        return paneId
    }

    function destroyPane(paneId) {
        const pane = root.panePool[paneId]
        if (!pane)
            return
        // Explicitly, and BEFORE the item goes: ChartView.removeSeries() hands
        // ownership back without deleting, so destroyed() is not a signal the
        // C++ side can rely on. Leaving this to the safety net in the pane's
        // own Component.onDestruction would work, but the ordering is clearer
        // stated here - one place decides that this pane's tracks are gone.
        GraphDataModel.untrackPane(paneId)
        delete root.panePool[paneId]
        pane.parent = null
        pane.destroy()
    }

    // ------------------------------------------------------------- rendering
    /*! Rebuilds the scaffolding and reparents the pooled panes into it.

        Panes are parked in \l detachedPanes first: destroying a split node
        would otherwise take the panes inside it with it, which is precisely
        the outcome this whole design exists to avoid. */
    function rebuildLayout() {
        // Break the anchor FIRST. A parked pane still anchored to a slot that
        // is about to be destroyed is anchored to a dead object.
        for (const id in root.panePool) {
            const pane = root.panePool[id]
            pane.anchors.fill = undefined
            pane.parent = detachedPanes
        }

        // destroy() is deferred to the event loop, so an old node would still
        // be painted over the new one for a frame. Unparent it now and let the
        // deletion happen whenever.
        const stale = []
        for (let i = 0; i < scaffold.children.length; ++i)
            stale.push(scaffold.children[i])
        for (let j = 0; j < stale.length; ++j) {
            stale[j].visible = false
            stale[j].parent = null
            stale[j].destroy()
        }

        root.mountNode(root.layoutTree, scaffold)
        root.syncPaneFlags()
    }

    /*! Parents the item for \a node into \a slot, recursing for splits. */
    function mountNode(node, slot) {
        if (!node)
            return
        if (node.kind === "leaf") {
            const pane = root.panePool[node.paneId]
            if (!pane)
                return
            pane.parent = slot
            pane.anchors.fill = slot
            return
        }

        const item = splitNodeComponent.createObject(slot, {
            vertical: node.vertical,
            ratio: node.ratio
        })
        if (!item) {
            console.warn("GraphPaneArea: could not create a split node")
            return
        }
        item.anchors.fill = slot
        // The node reports drags back into the tree, so a rebuild for an
        // unrelated reason does not snap every divider back to the middle.
        item.ratioEdited.connect(function (value) { node.ratio = value })

        root.mountNode(node.a, item.slotA)
        root.mountNode(node.b, item.slotB)
    }

    /*! canClose is false for the only pane on screen; the pane greys out both
        its X and its menu entry from it. */
    function syncPaneFlags() {
        const only = root.paneCount <= 1
        for (const id in root.panePool)
            root.panePool[id].canClose = !only
    }

    function clearAll() {
        for (const id in root.panePool)
            root.panePool[id].clearPane()
    }

    // ---------------------------------------------------- snapshot / restore
    function snapshot() {
        let state = { tree: root.cloneTree(root.layoutTree), panes: {} }
        for (const id in root.panePool) {
            const tracks = GraphDataModel.tracksForPane(id)
            let traces = []
            for (let t = 0; t < tracks.length; ++t) {
                // A handover is in-process and instantaneous, so a math
                // trace can be carried by CHANNEL ID here. The session file
                // cannot - see exportNode(), which uses the name.
                traces.push({ isVirtual: !!tracks[t].isVirtual,
                              channelId: tracks[t].channelId,
                              dbcIndex: tracks[t].dbcIndex,
                              messageIndex: tracks[t].messageIndex,
                              signalIndex: tracks[t].signalIndex })
            }
            state.panes[id] = traces
        }
        return state
    }

    function cloneTree(node) {
        if (!node)
            return null
        if (node.kind === "leaf")
            return { kind: "leaf", paneId: node.paneId }
        return { kind: "split", vertical: node.vertical, ratio: node.ratio,
                 a: root.cloneTree(node.a), b: root.cloneTree(node.b) }
    }

    /*! Renames every leaf into THIS area's namespace, appending
        {from, to} pairs to \a moved in traversal order. */
    function renameLeaves(node, moved) {
        if (!node)
            return
        if (node.kind === "leaf") {
            const fresh = root.paneIdPrefix + "P" + root.nextPaneSerial
            root.nextPaneSerial += 1
            moved.push({ from: node.paneId, to: fresh })
            node.paneId = fresh
            return
        }
        root.renameLeaves(node.a, moved)
        root.renameLeaves(node.b, moved)
    }

    function restore(state) {
        if (!state || !state.tree)
            return

        // Keys first: destroyPane() deletes from the pool, and mutating an
        // object mid for-in is a coin toss about what gets visited.
        let doomed = []
        for (const id in root.panePool)
            doomed.push(id)
        for (let d = 0; d < doomed.length; ++d)
            root.destroyPane(doomed[d])
        root.panePool = ({})

        // -- RE-KEY, rather than adopting the ids the snapshot carries.
        //
        // A detach leaves the source area standing: its panes are merely
        // hidden, never destroyed. Reusing the snapshot's ids therefore put a
        // second live pane in the process under a name the source still
        // answered to - and since GraphDataModel keys tracks on that string,
        // the two were one owner. What that cost was visible: restore()
        // destroys its old panes first, each destroy calling
        // untrackPane(<shared id>), which reached across and deleted a trace
        // belonging to the OTHER area. One trace disappeared per handover.
        //
        // Minting fresh ids from this area's own prefix and serial makes the
        // two namespaces disjoint, so neither can reach the other's tracks.
        // The snapshot's ids survive only as lookup keys for the trace lists.
        // nextPaneSerial is NOT reset. destroy() is deferred to the event
        // loop, so the panes doomed above are still alive right now - and the
        // pane this area minted in Component.onCompleted is among them. Reset
        // the counter and the first re-keyed pane is born holding a name a
        // dying one still answers to; when that one is finally collected its
        // Component.onDestruction untracks the id and takes the NEW pane's
        // trace with it. Monotonic serials cannot collide with a corpse.
        root.layoutTree = root.cloneTree(state.tree)
        let moved = []
        root.renameLeaves(root.layoutTree, moved)

        for (let i = 0; i < moved.length; ++i) {
            const pane = paneComponent.createObject(detachedPanes, { paneId: moved[i].to })
            if (pane)
                root.panePool[moved[i].to] = pane
        }
        root.layoutVersion += 1

        root.rebuildLayout()

        for (let j = 0; j < moved.length; ++j) {
            const traces = state.panes[moved[j].from] || []
            const pane = root.panePool[moved[j].to]
            if (!pane)
                continue
            for (let k = 0; k < traces.length; ++k) {
                if (traces[k].isVirtual)
                    pane.addMathChannel(traces[k].channelId)
                else
                    pane.addSignal(traces[k].dbcIndex, traces[k].messageIndex,
                                   traces[k].signalIndex)
            }
        }
        root.activePaneId = moved.length > 0 ? moved[0].to : ""
    }

    // ------------------------------------------------- session export / import
    //
    // A SECOND serialisation, deliberately not snapshot()/restore().
    //
    // Those two exist for the detach handover: same process, same instant,
    // same loaded databases, so naming a signal by its (dbcIndex,
    // messageIndex, signalIndex) triple is both cheapest and exact. A session
    // file is the opposite case - it is read back tomorrow, on another
    // machine, possibly with a fourth .dbc loaded ahead of the one that
    // matters. Those indices are positions in a list; the moment the list
    // differs, index 2 is a different signal and the graph plots the wrong
    // thing while looking entirely healthy. So the file carries NAMES and
    // GraphDataModel.findSignal() resolves them on the way back in.
    //
    // The tree shape is kept, not flattened to a list of panes: the recursive
    // split layout is most of what the user built, and a flat list would
    // restore the right number of panes in the wrong arrangement.

    /*! Serialises this area: the split tree, and per pane its scaling mode and
        its signals by name. Contains no ids - \l applyState() mints its own. */
    function exportState() {
        return { tree: root.exportNode(root.layoutTree) }
    }

    function exportNode(node) {
        if (!node)
            return null
        if (node.kind !== "leaf") {
            return { kind: "split", vertical: node.vertical, ratio: node.ratio,
                     a: root.exportNode(node.a), b: root.exportNode(node.b) }
        }

        const pane = root.panePool[node.paneId]
        let out = { kind: "leaf", signals: [] }
        if (!pane)
            return out

        // All four, not just autoScale: the pane has THREE modes - auto,
        // manual and free - and a lone autoScale flag cannot tell the last two
        // apart, so a free-scrolling pane would come back locked to whatever
        // manual bounds it happened to hold.
        out.autoScale = pane.autoScale
        out.manualScale = pane.manualScale
        out.manualMin = pane.manualMin
        out.manualMax = pane.manualMax

        const tracks = GraphDataModel.tracksForPane(node.paneId)
        for (let i = 0; i < tracks.length; ++i) {
            if (tracks[i].isVirtual) {
                // No dbc/message triple to write: a math channel is not IN a
                // database. Its name is the key, and MathChannelManager has
                // already been restored by the time this is read back.
                out.signals.push({ isVirtual: true,
                                   channelName: tracks[i].name })
                continue
            }
            out.signals.push({ dbc: tracks[i].dbcName,
                               message: tracks[i].messageName,
                               signal: tracks[i].name })
        }
        return out
    }

    /*! Rebuilds this area from \l exportState() output. Unresolvable signals
        are skipped and counted, never guessed at: returns the number that
        could not be found, so the page can say so instead of quietly showing
        a pane the user will assume is simply idle. */
    function applyState(state) {
        if (!state || !state.tree)
            return 0

        let doomed = []
        for (const id in root.panePool)
            doomed.push(id)
        for (let d = 0; d < doomed.length; ++d)
            root.destroyPane(doomed[d])
        root.panePool = ({})

        // Same rule as restore(): serials keep climbing. destroy() above is
        // deferred, so a reset counter would hand a new pane the name a corpse
        // still answers to and its Component.onDestruction would untrack the
        // live one's traces.
        let leaves = []
        root.layoutTree = root.buildNode(state.tree, leaves)
        if (!root.layoutTree) {
            // An empty or malformed tree must not leave the area with nothing
            // at all - there would be no pane to right-click and no way back.
            const fallback = root.paneIdPrefix + "P" + root.nextPaneSerial
            root.nextPaneSerial += 1
            root.layoutTree = { kind: "leaf", paneId: fallback }
            leaves = [ { paneId: fallback, spec: { signals: [] } } ]
        }

        for (let i = 0; i < leaves.length; ++i) {
            const pane = paneComponent.createObject(detachedPanes,
                                                    { paneId: leaves[i].paneId })
            if (pane)
                root.panePool[leaves[i].paneId] = pane
        }
        root.layoutVersion += 1
        root.rebuildLayout()

        let unresolved = 0
        for (let j = 0; j < leaves.length; ++j) {
            const pane = root.panePool[leaves[j].paneId]
            const spec = leaves[j].spec
            if (!pane)
                continue

            const list = spec.signals || []
            for (let k = 0; k < list.length; ++k) {
                if (list[k].isVirtual) {
                    // Resolved by name against the channels ConfigManager
                    // restored BEFORE it emitted graphStateRestored(). A -1
                    // here means the channel did not survive the import (a
                    // formula the whitelist now refuses, say) - counted with
                    // the missing signals rather than silently skipped.
                    const channelId = MathChannelManager.channelIdFor(list[k].channelName || "")
                    if (channelId < 0) {
                        unresolved += 1
                        continue
                    }
                    pane.addMathChannel(channelId)
                    continue
                }

                const hit = GraphDataModel.findSignal(list[k].dbc || "",
                                                      list[k].message || "",
                                                      list[k].signal || "")
                if (!hit || !hit.found) {
                    unresolved += 1
                    continue
                }
                pane.addSignal(hit.dbcIndex, hit.messageIndex, hit.signalIndex)
            }

            // Scales AFTER the signals. addSignal() ends with applyAutoScale(),
            // which overwrites the axis - setting the bounds first would show
            // the restored range for one frame and then lose it.
            if (spec.manualMin !== undefined)
                pane.manualMin = Number(spec.manualMin)
            if (spec.manualMax !== undefined)
                pane.manualMax = Number(spec.manualMax)
            if (spec.autoScale !== undefined)
                pane.autoScale = !!spec.autoScale
            if (spec.manualScale !== undefined)
                pane.manualScale = !!spec.manualScale
        }

        root.activePaneId = leaves.length > 0 ? leaves[0].paneId : ""
        return unresolved
    }

    /*! Converts one serialised node into a live tree node, minting a fresh
        paneId per leaf and appending {paneId, spec} to \a leaves in order. */
    function buildNode(spec, leaves) {
        if (!spec)
            return null

        if (spec.kind === "split") {
            const a = root.buildNode(spec.a, leaves)
            const b = root.buildNode(spec.b, leaves)
            // A split that lost one of its halves collapses into the other,
            // rather than mounting a node with a null slot.
            if (!a) return b
            if (!b) return a
            return { kind: "split",
                     vertical: !!spec.vertical,
                     ratio: (typeof spec.ratio === "number" && spec.ratio > 0 && spec.ratio < 1)
                            ? spec.ratio : 0.5,
                     a: a, b: b }
        }

        const paneId = root.paneIdPrefix + "P" + root.nextPaneSerial
        root.nextPaneSerial += 1
        leaves.push({ paneId: paneId, spec: spec })
        return { kind: "leaf", paneId: paneId }
    }

    // --------------------------------------------------------- signal picker
    property string pickerPaneId: ""

    function openPicker(paneId) {
        root.pickerPaneId = paneId
        root.activePaneId = paneId
        picker.open()
    }

    function paneById(paneId) {
        return root.panePool[paneId] || null
    }

    SignalPickerDialog {
        id: picker
        anchors.centerIn: parent
        paneId: root.pickerPaneId
        onSignalChosen: function (dbcIndex, messageIndex, signalIndex) {
            const pane = root.paneById(root.pickerPaneId)
            if (pane)
                pane.addSignal(dbcIndex, messageIndex, signalIndex)
        }
        onMathChannelChosen: function (channelId) {
            const pane = root.paneById(root.pickerPaneId)
            if (pane)
                pane.addMathChannel(channelId)
        }
    }

    // -------------------------------------------------------------- the tree
    /*! Where panes live while the scaffolding around them is rebuilt. Never
        visible - it has no size and clips. */
    Item {
        id: detachedPanes
        width: 0
        height: 0
        clip: true
        visible: false
    }

    Item {
        id: scaffold
        anchors.fill: parent
    }

    Component {
        id: splitNodeComponent
        GraphSplitNode {}
    }

    Component {
        id: paneComponent

        SplittableGraphPane {
            // paneId is supplied by createObject; it is `required` on the type.
            canSplit: true
            canClose: false
            active: root.activePaneId === paneId
            onActivated: root.activePaneId = paneId
            onSplitRequested: function (direction) { root.splitPane(paneId, direction) }
            onCloseRequested: root.closePane(paneId)
            onAddSignalRequested: root.openPicker(paneId)
        }
    }

    Component.onCompleted: {
        // The first pane is P1 and is created the same way every later one is,
        // so there is no special case for "the original".
        const first = root.paneIdPrefix + "P1"
        root.layoutTree = { kind: "leaf", paneId: first }
        const pane = paneComponent.createObject(detachedPanes, { paneId: first })
        if (pane)
            root.panePool[first] = pane
        root.activePaneId = first
        root.layoutVersion += 1
        root.rebuildLayout()
    }
}
