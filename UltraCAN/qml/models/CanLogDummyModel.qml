pragma Singleton

import QtQuick

/*!
    \qmltype CanLogDummyModel
    \brief In-memory trace buffer plus a fake bus that fills it.

    Phase 4 stand-in for the C++ acquisition path. The real implementation will
    be a QAbstractTableModel fed from the driver thread; keep these role names
    (\c time, \c direction, \c channel, \c canId, \c extended, \c dlc, \c payload)
    and GraphPlot-style view code will not need to change.

    A role must never be called "data": in a delegate that role name shadows
    Item's default property. Hence \c payload.

    Capacity: the buffer is a ring - once \l capacity frames are stored the
    oldest are dropped. A trace window must never be allowed to grow without
    bound; at 500 frames/s an unbounded ListModel eats a gigabyte in minutes.
*/
QtObject {
    id: root

    /*! Maximum frames kept in memory. Older frames are discarded. */
    property int capacity: 20000
    /*! Frames dropped by the ring buffer since the last clear. */
    property int droppedCount: 0
    /*! Total frames produced since the last clear (including dropped ones). */
    property int totalCount: 0
    /*! Bus timestamp in seconds, monotonic since the last clear. */
    property real busTime: 0

    readonly property var channelNames: [ "CAN1", "CAN2" ]

    /*! The trace itself. */
    readonly property ListModel frames: ListModel { }

    // --------------------------------------------------------- frame recipes
    // A small, realistic-looking traffic mix: each entry is emitted at its own
    // rate so the trace has the uneven rhythm of a real bus.
    readonly property var templates: [
        { name: "EngineData",     id: 0x100, ext: false, ch: 0, dir: "Rx", dlc: 8, period: 1  },
        { name: "BatteryStatus",  id: 0x200, ext: false, ch: 0, dir: "Rx", dlc: 8, period: 5  },
        { name: "VehicleCommand", id: 0x300, ext: false, ch: 1, dir: "Tx", dlc: 4, period: 2  },
        { name: "ChargerStatus",  id: 0x18FF50E5, ext: true, ch: 1, dir: "Rx", dlc: 8, period: 10 },
        { name: "DiagResponse",   id: 0x7E8, ext: false, ch: 0, dir: "Rx", dlc: 8, period: 25 },
        { name: "DiagRequest",    id: 0x7E0, ext: false, ch: 0, dir: "Tx", dlc: 8, period: 25 }
    ]

    property int tickIndex: 0

    // ------------------------------------------------------------- formatting
    function formatId(id, extended) {
        const hex = Number(id).toString(16).toUpperCase()
        return "0x" + hex.padStart(extended ? 8 : 3, "0")
    }

    function randomPayload(dlc) {
        let bytes = []
        for (let i = 0; i < dlc; ++i)
            bytes.push(Math.floor(Math.random() * 256).toString(16).toUpperCase().padStart(2, "0"))
        return bytes.join(" ")
    }

    // ---------------------------------------------------------------- writing
    function appendFrame(frame) {
        frames.append(frame)
        ++totalCount
        if (frames.count > capacity) {
            const excess = frames.count - capacity
            frames.remove(0, excess)        // ring buffer: drop the oldest
            droppedCount += excess
        }
    }

    /*!
        Advances the simulated bus by \a deltaSeconds and appends every frame
        that "arrived" during that slice. Returns how many were appended.
    */
    function generate(deltaSeconds) {
        busTime += deltaSeconds
        ++tickIndex

        let produced = 0
        for (let i = 0; i < templates.length; ++i) {
            const t = templates[i]
            if (tickIndex % t.period !== 0)
                continue

            appendFrame({
                time: busTime,
                direction: t.dir,
                channel: channelNames[t.ch],
                canId: t.id,
                idText: formatId(t.id, t.ext),
                extended: t.ext,
                dlc: t.dlc,
                payload: randomPayload(t.dlc),
                name: t.name
            })
            ++produced
        }

        // Occasional unsolicited frame so the trace is not perfectly periodic.
        if (Math.random() < 0.05) {
            const id = 0x400 + Math.floor(Math.random() * 0x50)
            const dlc = 1 + Math.floor(Math.random() * 8)
            appendFrame({
                time: busTime,
                direction: Math.random() < 0.75 ? "Rx" : "Tx",
                channel: channelNames[Math.floor(Math.random() * 2)],
                canId: id,
                idText: formatId(id, false),
                extended: false,
                dlc: dlc,
                payload: randomPayload(dlc),
                name: "Unmapped"
            })
            ++produced
        }
        return produced
    }

    function clear() {
        frames.clear()
        busTime = 0
        tickIndex = 0
        totalCount = 0
        droppedCount = 0
    }

    /*! Plain-text export used by the (simulated) Save Log action. */
    function toPlainText(maxRows) {
        const rows = Math.min(frames.count, maxRows === undefined ? frames.count : maxRows)
        let out = "Time,Dir,Ch,ID,Type,DLC,Data\n"
        for (let i = 0; i < rows; ++i) {
            const f = frames.get(i)
            out += "%1,%2,%3,%4,%5,%6,%7\n"
                   .arg(f.time.toFixed(4)).arg(f.direction).arg(f.channel)
                   .arg(f.idText).arg(f.extended ? "Ext" : "Std").arg(f.dlc).arg(f.payload)
        }
        return out
    }
}
