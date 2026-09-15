pragma Singleton

import QtQuick
import UltraCAN.Core

/*!
    \qmltype CanSenderDummyModel
    \brief Transmit-job list plus the DBC signal packer.

    Phase 5 stand-in for the C++ transmit path. A "job" is one frame the user
    wants on the bus, either once (\c cycleMs == 0) or periodically.

    The cyclic countdowns are deliberately NOT stored in the ListModel: writing
    a role at 100 Hz would notify every delegate on every tick. They live in a
    plain JS map keyed by \c jobId, which the view never binds to.

    Role names (\c name, \c idText, \c canId, \c extended, \c channel, \c dlc,
    \c payload, \c cycleMs, \c enabled, \c sentCount) are the contract for the
    future C++ transmit model. Never call a role \c data - it shadows Item's
    default property inside delegates.
*/
QtObject {
    id: root

    readonly property ListModel jobs: ListModel { }

    /*! Frames handed to the (simulated) driver since startup. */
    property int totalSent: 0
    /*! Monotonic id source; countdowns are keyed by it so removals cannot desync. */
    property int nextJobId: 1
    /*! jobId -> milliseconds until the next cyclic transmission. */
    property var countdowns: ({})

    readonly property var channelNames: [ "CAN1", "CAN2" ]

    // ------------------------------------------------------------ formatting
    function formatId(id, extended) {
        return "0x" + Number(id).toString(16).toUpperCase().padStart(extended ? 8 : 3, "0")
    }

    function formatCycle(cycleMs) {
        return cycleMs > 0 ? qsTr("%1 ms").arg(cycleMs) : qsTr("Manual")
    }

    /*! [0x12, 0xAB] -> "12 AB" */
    function bytesToHex(bytes) {
        let out = []
        for (let i = 0; i < bytes.length; ++i)
            out.push((bytes[i] & 0xFF).toString(16).toUpperCase().padStart(2, "0"))
        return out.join(" ")
    }

    // --------------------------------------------------------- signal packing
    /*!
        Bit position of the \a i-th bit (0 = MSB of the signal) for a Motorola
        signal starting at \a startBit, in the "sawtooth" numbering .dbc uses:
        walk down inside the byte, then jump to bit 7 of the next byte.
    */
    function motorolaBitIndex(startBit, i) {
        let byteIndex = Math.floor(startBit / 8)
        let bitIndex = startBit % 8
        for (let k = 0; k < i; ++k) {
            if (bitIndex === 0) {
                bitIndex = 7
                byteIndex += 1
            } else {
                bitIndex -= 1
            }
        }
        return byteIndex * 8 + bitIndex
    }

    /*! Physical value -> raw integer, clamped to the signal's width and sign. */
    function rawValueOf(sig, physical) {
        let raw = Math.round((physical - sig.offset) / sig.factor)
        const width = sig.bitLength
        if (sig.valueType === 1) {                       // signed
            const lo = -Math.pow(2, width - 1)
            const hi = Math.pow(2, width - 1) - 1
            raw = Math.max(lo, Math.min(hi, raw))
            if (raw < 0)
                raw += Math.pow(2, width)                // two's complement
        } else {
            raw = Math.max(0, Math.min(Math.pow(2, width) - 1, raw))
        }
        return raw
    }

    /*!
        Assembles the payload of message \a messageIndex from \a values
        (a map of signalIndex -> physical value). Returns an array of \a dlc
        bytes. Signals wider than 32 bits are not supported by JS bit ops -
        the C++ encoder will handle those.
    */
    function packPayload(messageIndex, values, dlc) {
        let bytes = new Array(dlc).fill(0)
        const signalList = DbcModel.signalsOf(messageIndex)
        if (!signalList)
            return bytes

        for (let s = 0; s < signalList.count; ++s) {
            const sig = signalList.get(s)
            const physical = values[s] !== undefined ? values[s] : sig.minimum
            const raw = rawValueOf(sig, physical)

            for (let i = 0; i < sig.bitLength; ++i) {          // i = bit weight, 0 = LSB
                const bit = (raw >>> i) & 1
                if (bit === 0)
                    continue
                const position = sig.byteOrder === DbcModel.byteOrderIntel
                               ? sig.startBit + i
                               : motorolaBitIndex(sig.startBit, sig.bitLength - 1 - i)
                const byteIndex = Math.floor(position / 8)
                if (byteIndex < dlc)
                    bytes[byteIndex] |= (1 << (position % 8))
            }
        }
        return bytes
    }

    // ---------------------------------------------------------------- jobs
    /*!
        Appends a transmit job. \a job must carry every role - a ListModel
        fixes its roles on the first append.
    */
    function addJob(job) {
        const id = nextJobId++
        jobs.append({
            jobId: id,
            name: job.name,
            canId: job.canId,
            idText: formatId(job.canId, job.extended === true),
            extended: job.extended === true,
            channel: job.channel,
            dlc: job.dlc,
            payload: job.payload,
            cycleMs: job.cycleMs,
            enabled: job.cycleMs > 0,          // cyclic jobs arm themselves
            sentCount: 0,
            origin: job.origin                 // "raw" | "dbc"
        })
        countdowns[id] = job.cycleMs
        return jobs.count - 1
    }

    function removeJob(index) {
        const job = jobAt(index)
        if (!job)
            return
        delete countdowns[job.jobId]
        jobs.remove(index)
    }

    function clearJobs() {
        countdowns = ({})
        jobs.clear()
    }

    function jobAt(index) {
        return (index >= 0 && index < jobs.count) ? jobs.get(index) : null
    }

    function setEnabled(index, value) {
        const job = jobAt(index)
        if (!job || job.cycleMs <= 0)
            return
        jobs.setProperty(index, "enabled", value)
        countdowns[job.jobId] = value ? job.cycleMs : 0
    }

    function markSent(index) {
        const job = jobAt(index)
        if (!job)
            return
        jobs.setProperty(index, "sentCount", job.sentCount + 1)
        ++totalSent
    }

    function enabledCount() {
        let n = 0
        for (let i = 0; i < jobs.count; ++i)
            if (jobs.get(i).enabled && jobs.get(i).cycleMs > 0)
                ++n
        return n
    }

    /*!
        Advances every armed cyclic job by \a deltaMs and returns the indexes
        that are due for transmission this tick.
    */
    function tick(deltaMs) {
        let due = []
        for (let i = 0; i < jobs.count; ++i) {
            const job = jobs.get(i)
            if (!job.enabled || job.cycleMs <= 0)
                continue

            let remaining = countdowns[job.jobId]
            if (remaining === undefined)
                remaining = job.cycleMs
            remaining -= deltaMs
            if (remaining <= 0) {
                due.push(i)
                remaining += job.cycleMs        // keep the phase, do not reset to full
            }
            countdowns[job.jobId] = remaining
        }
        return due
    }
}
