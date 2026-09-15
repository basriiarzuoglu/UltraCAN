import QtQuick
import QtQuick.Controls.Basic

import UltraCAN.Theme

/*!
    \qmltype BitMatrix
    \brief The CANdb++ 8x8 bit layout grid: which 64 bits a signal actually occupies.

    Eight rows of eight cells. A row is a byte, columns run bit 7 down to bit 0
    left to right, and every cell is labelled with its absolute bit index - so
    row 1 reads 15 14 13 12 11 10 9 8. That is the layout every CAN tool draws,
    and matching it is the point: an engineer reading a .dbc in CANdb++ and the
    same .dbc here must see the same picture.

    \section1 Why this exists

    "Start bit 12, length 10, Motorola" is not something anyone can hold in
    their head. Two mistakes are almost impossible to catch from the spin boxes
    alone and obvious the instant they are drawn:
    \list
    \li a Motorola signal does NOT occupy a contiguous ascending run - the
        one-line summary "bits 12..21" is simply wrong for big-endian, and this
        grid is what shows the real sawtooth;
    \li two signals silently overlapping, which decodes as garbage that looks
        like a hardware fault.
    \endlist
    So \l otherSpans draws the message's other signals underneath, and any cell
    claimed twice is painted in the error colour.

    \section1 The bit walk

    Deliberately identical to \c CanDecoder::extractRaw(). If these two ever
    disagree, the picture is a lie about what the decoder will read, which is
    worse than having no picture. Verified by walking both over the same
    (startBit, length, byteOrder) space.
*/
Item {
    id: root

    // -- the signal being edited ---------------------------------------------
    property int startBit: 0
    property int bitLength: 1
    /*! DbcModel's convention, matching the .dbc file format: 0 = Motorola, 1 = Intel. */
    property int byteOrder: 1
    /*! Bytes the message actually carries. Cells past it are drawn as
        unavailable, which is how "this signal runs off the end of its message"
        becomes visible instead of merely being wrong. */
    property int dlc: 8

    /*! The message's OTHER signals, as
        [{ startBit, bitLength, byteOrder, name }], drawn muted so a collision
        is visible before it is debugged. */
    property var otherSpans: []

    property bool interactive: true
    /*! Emitted when a cell is clicked; the editor turns it into a new start bit. */
    signal bitClicked(int bit)

    // -- geometry ------------------------------------------------------------
    readonly property int headerWidth: 20
    readonly property int cellHeight: 17
    readonly property real cellWidth: Math.max(22, (width - headerWidth - 2) / 8)

    implicitWidth: headerWidth + cellWidth * 8 + 2
    implicitHeight: cellHeight * 9 + 2

    // ======================================================== the bit walk
    //
    // One function, used for the edited signal and for every other span, so
    // there is exactly one place where this can be wrong.

    /*!
        Absolute bit indices a signal occupies, in signal order: index 0 is the
        signal's MSB for Motorola and its LSB for Intel - the same order
        CanDecoder walks.
    */
    function bitsOf(start, length, order) {
        const bits = []
        if (length <= 0 || start < 0)
            return bits

        if (order === 1) {
            // Intel / little endian: bit i of the signal sits at start + i.
            for (let i = 0; i < length; ++i)
                bits.push(start + i)
            return bits
        }

        // Motorola / big endian: start is the MSB. Walk DOWN inside the byte,
        // then continue at bit 7 of the next byte. In absolute numbering that
        // is "minus one, except at a byte boundary, where it is plus fifteen".
        let bit = start
        for (let i = 0; i < length; ++i) {
            bits.push(bit)
            bit = (bit % 8 === 0) ? bit + 15 : bit - 1
        }
        return bits
    }

    /*! 64 entries: the signal's own bit position (0 = LSB) or -1. */
    readonly property var ownMap: {
        const map = new Array(64).fill(-1)
        const bits = bitsOf(root.startBit, root.bitLength, root.byteOrder)
        for (let i = 0; i < bits.length; ++i) {
            const absolute = bits[i]
            if (absolute < 0 || absolute > 63)
                continue
            // Intel walks LSB first, Motorola MSB first.
            map[absolute] = (root.byteOrder === 1) ? i : (bits.length - 1 - i)
        }
        return map
    }

    /*! 64 entries: the name of an overlapping other signal, or "". */
    readonly property var otherMap: {
        const map = new Array(64).fill("")
        for (let s = 0; s < root.otherSpans.length; ++s) {
            const span = root.otherSpans[s]
            const bits = bitsOf(span.startBit, span.bitLength, span.byteOrder)
            for (let i = 0; i < bits.length; ++i) {
                if (bits[i] >= 0 && bits[i] <= 63)
                    map[bits[i]] = span.name
            }
        }
        return map
    }

    readonly property int collisionCount: {
        let n = 0
        for (let b = 0; b < 64; ++b)
            if (ownMap[b] >= 0 && otherMap[b].length > 0)
                ++n
        return n
    }

    /*!
        The occupied bits as a human-readable run list: "bits 12..21" for a
        contiguous Intel field, "bits 8..12, 19..23" for a Motorola sawtooth.
        The properties panel prints this instead of the start..start+len-1 it
        used to assume, which was wrong for every big-endian signal.
    */
    readonly property string occupancyText: {
        const bits = bitsOf(root.startBit, root.bitLength, root.byteOrder)
                     .filter(function(b) { return b >= 0 && b <= 63 })
        if (bits.length === 0)
            return qsTr("no bits")

        const sorted = bits.slice().sort(function(a, b) { return a - b })
        const runs = []
        let runStart = sorted[0]
        let previous = sorted[0]
        for (let i = 1; i < sorted.length; ++i) {
            if (sorted[i] === previous + 1) {
                previous = sorted[i]
                continue
            }
            runs.push(runStart === previous ? String(runStart)
                                            : (runStart + ".." + previous))
            runStart = sorted[i]
            previous = sorted[i]
        }
        runs.push(runStart === previous ? String(runStart) : (runStart + ".." + previous))
        return qsTr("bits %1").arg(runs.join(", "))
    }

    /*! True when the signal runs past the message's last byte. */
    readonly property bool overrunsDlc: {
        const bits = bitsOf(root.startBit, root.bitLength, root.byteOrder)
        for (let i = 0; i < bits.length; ++i)
            if (bits[i] > root.dlc * 8 - 1 || bits[i] > 63)
                return true
        return false
    }

    // ============================================================== the grid
    //
    // ONE Repeater of 81 cells rather than a Loader per slot: nine of them are
    // headers and the rest are bits, which is a difference of content, not of
    // structure. The Loader version needed the grid position read back off
    // parent at runtime; here every cell derives its own position from index.
    Rectangle {
        anchors.fill: parent
        color: Theme.surface
        border.width: 1
        border.color: Theme.border

        Grid {
            x: 1
            y: 1
            columns: 9
            rowSpacing: 1
            columnSpacing: 1

            Repeater {
                model: 81

                delegate: Rectangle {
                    id: cell

                    required property int index

                    // Row 0 is the column-header strip; column 0 is the byte
                    // label. Everything else is a bit.
                    readonly property int gridRow: Math.floor(cell.index / 9)
                    readonly property int gridColumn: cell.index % 9
                    readonly property bool isHeader: gridRow === 0 || gridColumn === 0

                    readonly property int byteIndex: gridRow - 1
                    // Columns run bit 7 (leftmost) to bit 0 (rightmost), which
                    // is the CANdb++ layout the whole grid is imitating.
                    readonly property int bitInByte: 8 - gridColumn
                    readonly property int absoluteBit: isHeader ? -1
                                                                : byteIndex * 8 + bitInByte

                    readonly property int ownPosition: isHeader ? -1 : root.ownMap[absoluteBit]
                    readonly property bool mine: ownPosition >= 0
                    readonly property string otherName: isHeader ? "" : root.otherMap[absoluteBit]
                    readonly property bool collides: mine && otherName.length > 0
                    readonly property bool outsideDlc: !isHeader && byteIndex >= root.dlc
                    readonly property bool isMsb: mine && ownPosition === root.bitLength - 1
                    readonly property bool isLsb: mine && ownPosition === 0

                    width: gridColumn === 0 ? root.headerWidth : root.cellWidth
                    height: root.cellHeight
                    radius: isHeader ? 0 : 2

                    color: isHeader ? "transparent"
                         : collides ? Theme.error
                         : mine     ? (outsideDlc ? Theme.warning : Theme.accent)
                         : otherName.length > 0 ? Theme.panelElevated
                         : outsideDlc ? Theme.background
                         : Theme.panel

                    // A ring on the first and last bit, so the DIRECTION of the
                    // walk is readable rather than inferred - that is what
                    // distinguishes Intel from Motorola at a glance.
                    border.width: (isMsb || isLsb) ? 1 : 0
                    border.color: Theme.textPrimary

                    Label {
                        anchors.centerIn: parent
                        text: cell.gridRow === 0 && cell.gridColumn === 0 ? ""
                            : cell.gridRow === 0 ? String(8 - cell.gridColumn)
                            : cell.gridColumn === 0 ? String(cell.byteIndex)
                            // The absolute index, always shown: it is the
                            // number you are matching against a spec sheet.
                            : String(cell.absoluteBit)
                        color: cell.isHeader ? Theme.textSecondary
                             : (cell.mine ? Theme.background
                                          : (cell.otherName.length > 0 ? Theme.textSecondary
                                                                       : Theme.textDisabled))
                        font.family: Theme.monoFontFamily
                        font.pixelSize: Theme.fontSizeSm
                        font.weight: (cell.isHeader || cell.mine) ? Font.DemiBold : Font.Normal
                    }

                    // MSB / LSB corner tick.
                    Rectangle {
                        visible: cell.isMsb || cell.isLsb
                        width: 4
                        height: 4
                        radius: 2
                        color: Theme.background
                        anchors.top: parent.top
                        anchors.left: cell.isMsb ? parent.left : undefined
                        anchors.right: cell.isLsb ? parent.right : undefined
                        anchors.margins: 1
                    }

                    HoverHandler {
                        id: cellHover
                        enabled: root.interactive && !cell.isHeader
                    }
                    TapHandler {
                        enabled: root.interactive && !cell.isHeader
                        onSingleTapped: root.bitClicked(cell.absoluteBit)
                    }

                    Rectangle {
                        anchors.fill: parent
                        radius: parent.radius
                        color: Theme.textPrimary
                        opacity: cellHover.hovered ? 0.18 : 0
                        Behavior on opacity { NumberAnimation { duration: 80 } }
                    }

                    ToolTip.visible: cellHover.hovered
                    ToolTip.delay: 400
                    ToolTip.text: {
                        if (cell.isHeader)
                            return ""
                        const where = qsTr("bit %1  (byte %2, bit %3)")
                                      .arg(cell.absoluteBit).arg(cell.byteIndex).arg(cell.bitInByte)
                        if (cell.collides)
                            return qsTr("%1 - COLLIDES with %2").arg(where).arg(cell.otherName)
                        if (cell.mine)
                            return qsTr("%1 - this signal, bit %2%3")
                                   .arg(where).arg(cell.ownPosition)
                                   .arg(cell.isMsb ? qsTr(" (MSB)")
                                                   : (cell.isLsb ? qsTr(" (LSB)") : ""))
                        if (cell.otherName.length > 0)
                            return qsTr("%1 - %2").arg(where).arg(cell.otherName)
                        if (cell.outsideDlc)
                            return qsTr("%1 - beyond the message's %2 bytes")
                                   .arg(where).arg(root.dlc)
                        return qsTr("%1 - free").arg(where)
                    }
                }
            }
        }
    }
}
