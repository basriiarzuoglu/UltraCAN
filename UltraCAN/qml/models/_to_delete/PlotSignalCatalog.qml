pragma Singleton

import QtQuick

/*!
    \qmltype PlotSignalCatalog
    \brief Dummy catalogue of plottable signals plus the waveform generator
           that fakes live bus traffic.

    Phase 3 stand-in. The real implementation will expose the decoded signals
    of the loaded DBC and receive samples from the C++ acquisition thread; keep
    the role names (\c name, \c unit, \c minimum, \c maximum, \c active) and
    the \l sample() signature and GraphPlotView will not need changes.
*/
QtObject {
    id: root

    /*! Signals the user can put on the chart. \c active mirrors the check box. */
    readonly property ListModel items: ListModel {
        ListElement {
            name: "EngineSpeed";  source: "EngineData";     unit: "rpm"
            minimum: 0.0;    maximum: 8000.0; waveform: "sine";       frequency: 0.12; active: true
        }
        ListElement {
            name: "CoolantTemp";  source: "EngineData";     unit: "degC"
            minimum: -40.0;  maximum: 215.0;  waveform: "randomWalk"; frequency: 0.05; active: false
        }
        ListElement {
            name: "ThrottlePos";  source: "EngineData";     unit: "%"
            minimum: 0.0;    maximum: 100.0;  waveform: "cosine";     frequency: 0.25; active: false
        }
        ListElement {
            name: "PackVoltage";  source: "BatteryStatus";  unit: "V"
            minimum: 0.0;    maximum: 650.0;  waveform: "ramp";       frequency: 0.08; active: false
        }
        ListElement {
            name: "PackCurrent";  source: "BatteryStatus";  unit: "A"
            minimum: -3200.0; maximum: 3200.0; waveform: "sine";      frequency: 0.33; active: false
        }
        ListElement {
            name: "TorqueRequest"; source: "VehicleCommand"; unit: "Nm"
            minimum: -1024.0; maximum: 1023.5; waveform: "square";    frequency: 0.20; active: false
        }
        ListElement {
            name: "AmbientTemp";  source: "VehicleCommand"; unit: "degC"
            minimum: -40.0;  maximum: 85.0;   waveform: "noise";      frequency: 1.00; active: false
        }
    }

    /*! Random-walk state, keyed by signal index. Reset by \l resetState(). */
    property var walkState: ({})

    function resetState() {
        walkState = ({})
    }

    /*!
        Returns a fabricated engineering value for signal \a index at time
        \a t (seconds). Deterministic for every waveform except the random
        ones, so the traces look like real, repeatable bus data.
    */
    function sample(index, t) {
        const sig = items.get(index)
        if (!sig)
            return 0

        const min = sig.minimum
        const max = sig.maximum
        const mid = (min + max) / 2
        const amp = (max - min) / 2
        const w = 2 * Math.PI * sig.frequency * t
        const phase = index * 0.7          // de-phase the traces so they stay readable

        switch (sig.waveform) {
        case "sine":
            return mid + amp * 0.85 * Math.sin(w + phase)
        case "cosine":
            return mid + amp * 0.75 * Math.cos(w + phase) + amp * 0.1 * Math.sin(6 * w)
        case "ramp": {
            const p = (sig.frequency * t + index * 0.15) % 1.0
            return min + (max - min) * p
        }
        case "square":
            return Math.sin(w + phase) >= 0 ? mid + amp * 0.7 : mid - amp * 0.7
        case "noise":
            return mid + amp * 0.4 * Math.sin(w * 0.3) + amp * 0.25 * (Math.random() - 0.5)
        case "randomWalk":
        default: {
            let v = walkState[index]
            if (v === undefined)
                v = mid
            v += (max - min) * 0.01 * (Math.random() - 0.5)
            v += (mid - v) * 0.002                  // gentle pull back to the middle
            v = Math.max(min, Math.min(max, v))
            walkState[index] = v
            return v
        }
        }
    }

    function setActive(index, value) {
        if (index >= 0 && index < items.count)
            items.setProperty(index, "active", value)
    }

    function activeCount() {
        let n = 0
        for (let i = 0; i < items.count; ++i)
            if (items.get(i).active)
                ++n
        return n
    }
}
