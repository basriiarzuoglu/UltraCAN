pragma Singleton

import QtQuick

/*!
    \qmltype DbcDummyModel
    \brief Throw-away in-memory stand-in for the future C++ DBC document.

    Shape note: this deliberately mirrors the object model a real .dbc file
    has (Network -> Messages -> Signals), so replacing it with a
    QAbstractItemModel exposed from C++ is a drop-in change for the views:
    keep the role names and the mutator function signatures identical and
    DbcEditorView needs no edits.

    Roles are fixed by the FIRST ListElement of each list - every element must
    therefore declare every role, and every role must be a literal.
*/
QtObject {
    id: root

    // Endianness follows the .dbc convention: @0 = Motorola (big endian),
    // @1 = Intel (little endian).
    readonly property int byteOrderMotorola: 0
    readonly property int byteOrderIntel: 1

    readonly property var byteOrderNames: [ qsTr("Motorola (BE)"), qsTr("Intel (LE)") ]
    readonly property var valueTypeNames: [ qsTr("Unsigned"), qsTr("Signed") ]
    readonly property var nodeNames: [ "ECU", "BMS", "VCU", "Dashboard", "Charger" ]

    /*! Name of the loaded database (shown in the page subtitle). */
    property string databaseName: "demo_powertrain.dbc"
    /*! Set by any mutator; the editor shows an asterisk while true. */
    property bool modified: false

    // ---------------------------------------------------------------- data
    readonly property ListModel messages: ListModel {
        id: messageList

        ListElement {
            name: "EngineData"
            canId: 256              // 0x100
            dlc: 8
            transmitter: "ECU"
            cycleMs: 10
            comment: "Primary engine status frame"
            expanded: true
            signalList: [
                ListElement {
                    name: "EngineSpeed"
                    startBit: 0
                    bitLength: 16
                    byteOrder: 1
                    valueType: 0
                    factor: 0.125
                    offset: 0.0
                    minimum: 0.0
                    maximum: 8000.0
                    unit: "rpm"
                    receiver: "Dashboard"
                    comment: "Crankshaft speed"
                },
                ListElement {
                    name: "CoolantTemp"
                    startBit: 16
                    bitLength: 8
                    byteOrder: 1
                    valueType: 1
                    factor: 1.0
                    offset: -40.0
                    minimum: -40.0
                    maximum: 215.0
                    unit: "degC"
                    receiver: "Dashboard"
                    comment: "Coolant temperature"
                },
                ListElement {
                    name: "ThrottlePos"
                    startBit: 24
                    bitLength: 8
                    byteOrder: 1
                    valueType: 0
                    factor: 0.4
                    offset: 0.0
                    minimum: 0.0
                    maximum: 100.0
                    unit: "%"
                    receiver: "VCU"
                    comment: "Pedal position"
                }
            ]
        }

        ListElement {
            name: "BatteryStatus"
            canId: 512              // 0x200
            dlc: 8
            transmitter: "BMS"
            cycleMs: 100
            comment: "High voltage pack summary"
            expanded: true
            signalList: [
                ListElement {
                    name: "PackVoltage"
                    startBit: 0
                    bitLength: 16
                    byteOrder: 0
                    valueType: 0
                    factor: 0.01
                    offset: 0.0
                    minimum: 0.0
                    maximum: 650.0
                    unit: "V"
                    receiver: "VCU"
                    comment: ""
                },
                ListElement {
                    name: "PackCurrent"
                    startBit: 16
                    bitLength: 16
                    byteOrder: 0
                    valueType: 1
                    factor: 0.1
                    offset: -3200.0
                    minimum: -3200.0
                    maximum: 3200.0
                    unit: "A"
                    receiver: "VCU"
                    comment: "Positive = discharge"
                }
            ]
        }

        ListElement {
            name: "VehicleCommand"
            canId: 768              // 0x300
            dlc: 4
            transmitter: "VCU"
            cycleMs: 20
            comment: ""
            expanded: false
            signalList: [
                ListElement {
                    name: "TorqueRequest"
                    startBit: 0
                    bitLength: 12
                    byteOrder: 1
                    valueType: 1
                    factor: 0.5
                    offset: -1024.0
                    minimum: -1024.0
                    maximum: 1023.5
                    unit: "Nm"
                    receiver: "ECU"
                    comment: ""
                }
            ]
        }
    }

    // ----------------------------------------------------------- accessors
    function messageCount() {
        return messages.count
    }

    function message(messageIndex) {
        if (messageIndex < 0 || messageIndex >= messages.count)
            return null
        return messages.get(messageIndex)
    }

    function signalsOf(messageIndex) {
        const msg = message(messageIndex)
        return msg ? msg.signalList : null
    }

    function signalAt(messageIndex, signalIndex) {
        const list = signalsOf(messageIndex)
        if (!list || signalIndex < 0 || signalIndex >= list.count)
            return null
        return list.get(signalIndex)
    }

    // ------------------------------------------------------------ mutators
    function setMessageProperty(messageIndex, role, value) {
        if (!message(messageIndex))
            return
        messages.setProperty(messageIndex, role, value)
        modified = true
    }

    function setSignalProperty(messageIndex, signalIndex, role, value) {
        if (!signalAt(messageIndex, signalIndex))
            return
        signalsOf(messageIndex).setProperty(signalIndex, role, value)
        modified = true
    }

    /*! Appends a new message and returns its index. */
    function addMessage() {
        const nextId = 256 + messages.count * 16
        messages.append({
            name: "NewMessage_%1".arg(messages.count + 1),
            canId: nextId,
            dlc: 8,
            transmitter: nodeNames[0],
            cycleMs: 100,
            comment: "",
            expanded: true,
            signalList: []
        })
        modified = true
        return messages.count - 1
    }

    /*! Appends a signal to \a messageIndex and returns its index (-1 on failure). */
    function addSignal(messageIndex) {
        const list = signalsOf(messageIndex)
        if (!list)
            return -1

        // Place the new signal after the currently highest occupied bit.
        let nextBit = 0
        for (let i = 0; i < list.count; ++i)
            nextBit = Math.max(nextBit, list.get(i).startBit + list.get(i).bitLength)

        list.append({
            name: "NewSignal_%1".arg(list.count + 1),
            startBit: Math.min(nextBit, 63),
            bitLength: 8,
            byteOrder: byteOrderIntel,
            valueType: 0,
            factor: 1.0,
            offset: 0.0,
            minimum: 0.0,
            maximum: 255.0,
            unit: "",
            receiver: nodeNames[0],
            comment: ""
        })
        messages.setProperty(messageIndex, "expanded", true)
        modified = true
        return list.count - 1
    }

    function removeMessage(messageIndex) {
        if (!message(messageIndex))
            return
        messages.remove(messageIndex)
        modified = true
    }

    function removeSignal(messageIndex, signalIndex) {
        if (!signalAt(messageIndex, signalIndex))
            return
        signalsOf(messageIndex).remove(signalIndex)
        modified = true
    }

    function toggleExpanded(messageIndex) {
        const msg = message(messageIndex)
        if (msg)
            messages.setProperty(messageIndex, "expanded", !msg.expanded)
    }

    /*! Convenience for display: 0x100 style. */
    function formatCanId(id) {
        return "0x" + Number(id).toString(16).toUpperCase().padStart(3, "0")
    }
}
