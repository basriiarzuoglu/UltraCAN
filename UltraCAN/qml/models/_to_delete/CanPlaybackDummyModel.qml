pragma Singleton

import QtQuick

/*!
    \qmltype CanPlaybackDummyModel
    \brief A recorded trace held in memory, plus the lookups a player needs.

    Phase 6 stand-in for a parsed .asc/.blf/.trc file. Roles are identical to
    CanLogDummyModel (\c time, \c direction, \c channel, \c canId, \c idText,
    \c extended, \c dlc, \c payload, \c name) so the very same LogRow delegate
    renders both the live trace and the playback trace.

    \note Frames are sorted by \c time on construction, which is what lets
          \l indexAtTime() be a binary search. Scrubbing a 100k-frame log must
          never walk the list - that is the difference between a scrubber that
          tracks the mouse and one that stutters.
*/
QtObject {
    id: root

    readonly property ListModel frames: ListModel { }

    /*! Name of the loaded file, empty when nothing is loaded. */
    property string sourceName: ""
    /*! Timestamp of the last frame, in seconds. */
    property real duration: 0
    readonly property bool loaded: frames.count > 0

    readonly property var channelNames: [ "CAN1", "CAN2" ]

    // The recorded traffic mix. Weight controls how often each one appears.
    readonly property var templates: [
        { name: "EngineData",     id: 0x100,      ext: false, ch: 0, dir: "Rx", dlc: 8, weight: 10 },
        { name: "BatteryStatus",  id: 0x200,      ext: false, ch: 0, dir: "Rx", dlc: 8, weight: 5  },
        { name: "VehicleCommand", id: 0x300,      ext: false, ch: 1, dir: "Tx", dlc: 4, weight: 4  },
        { name: "ChargerStatus",  id: 0x18FF50E5, ext: true,  ch: 1, dir: "Rx", dlc: 8, weight: 2  },
        { name: "DiagRequest",    id: 0x7E0,      ext: false, ch: 0, dir: "Tx", dlc: 8, weight: 1  },
        { name: "DiagResponse",   id: 0x7E8,      ext: false, ch: 0, dir: "Rx", dlc: 8, weight: 1  }
    ]

    // ------------------------------------------------------------ formatting
    function formatId(id, extended) {
        return "0x" + Number(id).toString(16).toUpperCase().padStart(extended ? 8 : 3, "0")
    }

    function randomPayload(dlc) {
        let bytes = []
        for (let i = 0; i < dlc; ++i)
            bytes.push(Math.floor(Math.random() * 256).toString(16).toUpperCase().padStart(2, "0"))
        return bytes.join(" ")
    }

    /*! "12.345 s" -> "00:12.345", the form a player should show. */
    function formatTime(seconds) {
        const s = Math.max(0, seconds)
        const minutes = Math.floor(s / 60)
        const rest = s - minutes * 60
        return "%1:%2".arg(String(minutes).padStart(2, "0"))
                      .arg(rest.toFixed(3).padStart(6, "0"))
    }

    // ---------------------------------------------------------------- loading
    /*!
        Fabricates a recorded log of \a frameCount frames with monotonically
        increasing timestamps (roughly 2..22 ms apart, so ~1000 frames make a
        12-second recording).
    */
    function loadDemo(fileName, frameCount) {
        frames.clear()

        // Flatten the weights once so picking a template is O(1) per frame.
        let bag = []
        for (let i = 0; i < templates.length; ++i)
            for (let w = 0; w < templates[i].weight; ++w)
                bag.push(templates[i])

        let t = 0
        for (let f = 0; f < frameCount; ++f) {
            const tpl = bag[Math.floor(Math.random() * bag.length)]
            t += 0.002 + Math.random() * 0.02
            frames.append({
                time: t,
                direction: tpl.dir,
                channel: channelNames[tpl.ch],
                canId: tpl.id,
                idText: formatId(tpl.id, tpl.ext),
                extended: tpl.ext,
                dlc: tpl.dlc,
                payload: randomPayload(tpl.dlc),
                name: tpl.name
            })
        }

        duration = t
        sourceName = fileName
    }

    function clear() {
        frames.clear()
        duration = 0
        sourceName = ""
    }

    // ---------------------------------------------------------------- lookups
    function frameAt(index) {
        return (index >= 0 && index < frames.count) ? frames.get(index) : null
    }

    function timeAt(index) {
        const frame = frameAt(index)
        return frame ? frame.time : 0
    }

    /*!
        Index of the last frame whose timestamp is <= \a seconds, or -1 before
        the first frame. Binary search: O(log n), called on every playback tick
        and on every pixel of a scrub.
    */
    function indexAtTime(seconds) {
        let lo = 0
        let hi = frames.count - 1
        let result = -1
        while (lo <= hi) {
            const mid = (lo + hi) >> 1
            if (frames.get(mid).time <= seconds) {
                result = mid
                lo = mid + 1
            } else {
                hi = mid - 1
            }
        }
        return result
    }

    /*! Average frames per second across the whole recording. */
    function averageRate() {
        return duration > 0 ? frames.count / duration : 0
    }
}
