import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import UltraCAN.Theme
import UltraCAN.Components

/*!
    \qmltype LogFilterRow
    \brief The pass-filter form: a mode dropdown and the fields that mode needs.

    Fields are shown and hidden rather than created and destroyed, so switching
    to ID Range to check something and back to Single ID does not lose the id
    that was already typed.

    Everything id-shaped is entered and displayed in HEX, because that is how a
    CAN id is written everywhere else in this application and on every tool the
    user will compare it against. \l parseHex() accepts it with or without the
    0x, since half the world writes one and half writes the other.
*/
ColumnLayout {
    id: root

    property var modeLabels: []

    /*! LogFilter::Mode - 0 pass all, 1 single id, 2 range, 3 payload mask. */
    property int mode: 0
    property string startIdText: "0x100"
    property string endIdText: "0x1FF"
    property int byteIndex: 0
    property string maskText: "FF"
    property string matchText: "00"

    /*! -1 when \a text is not a hex number. Callers must treat -1 as a typo and
        refuse, never as zero: 0x000 is a real id that a lot of ECUs use. */
    function parseHex(text) {
        const cleaned = String(text).trim().replace(/^0[xX]/, "")
        if (cleaned.length === 0 || !/^[0-9a-fA-F]+$/.test(cleaned))
            return -1
        return parseInt(cleaned, 16)
    }

    spacing: Theme.spacingXs

    Label {
        text: qsTr("PASS FILTERS")
        color: Theme.textDisabled
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeSm
        font.letterSpacing: 1.1
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("What reaches the file at all. A frame the filter rejects is not "
                 + "buffered, is not written, and cannot fire a trigger.")
        color: Theme.textDisabled
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeSm
        wrapMode: Text.WordWrap
    }

    ThemedComboBox {
        Layout.fillWidth: true
        model: root.modeLabels
        currentIndex: root.mode
        onActivated: root.mode = currentIndex
    }

    // -- Single ID
    RowLayout {
        Layout.fillWidth: true
        visible: root.mode === 1
        spacing: Theme.spacingSm

        Label {
            Layout.preferredWidth: 80
            text: qsTr("CAN ID")
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSm
        }
        ThemedTextField {
            Layout.preferredWidth: 130
            text: root.startIdText
            placeholderText: "0x100"
            font.family: Theme.monoFontFamily
            onTextChanged: root.startIdText = text
        }
        Item { Layout.fillWidth: true }
    }

    // -- ID Range
    RowLayout {
        Layout.fillWidth: true
        visible: root.mode === 2
        spacing: Theme.spacingSm

        Label {
            Layout.preferredWidth: 80
            text: qsTr("From")
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSm
        }
        ThemedTextField {
            Layout.preferredWidth: 110
            text: root.startIdText
            placeholderText: "0x100"
            font.family: Theme.monoFontFamily
            onTextChanged: root.startIdText = text
        }
        Label {
            text: qsTr("to")
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSm
        }
        ThemedTextField {
            Layout.preferredWidth: 110
            text: root.endIdText
            placeholderText: "0x1FF"
            font.family: Theme.monoFontFamily
            onTextChanged: root.endIdText = text
        }
        Item { Layout.fillWidth: true }
    }

    // -- Payload Mask
    ColumnLayout {
        Layout.fillWidth: true
        visible: root.mode === 3
        spacing: Theme.spacingXs

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSm

            Label {
                Layout.preferredWidth: 80
                text: qsTr("Target ID")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSm
            }
            ThemedTextField {
                Layout.preferredWidth: 110
                text: root.startIdText
                placeholderText: "0x100"
                font.family: Theme.monoFontFamily
                onTextChanged: root.startIdText = text
            }
            Label {
                text: qsTr("Byte")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSm
            }
            ThemedSpinBox {
                Layout.preferredWidth: 92
                from: 0
                to: 7
                value: root.byteIndex
                onValueModified: root.byteIndex = value
            }
            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSm

            Label {
                Layout.preferredWidth: 80
                text: qsTr("Mask")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSm
            }
            ThemedTextField {
                Layout.preferredWidth: 70
                text: root.maskText
                placeholderText: "FF"
                font.family: Theme.monoFontFamily
                onTextChanged: root.maskText = text
            }
            Label {
                text: qsTr("equals")
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeSm
            }
            ThemedTextField {
                Layout.preferredWidth: 70
                text: root.matchText
                placeholderText: "00"
                font.family: Theme.monoFontFamily
                onTextChanged: root.matchText = text
            }
            Item { Layout.fillWidth: true }
        }

        // Reads back the rule in the form it is actually evaluated in, because
        // "mask FF match 00" is not something anyone can check at a glance.
        Label {
            Layout.fillWidth: true
            text: {
                const id = root.parseHex(root.startIdText)
                const mask = root.parseHex(root.maskText)
                const match = root.parseHex(root.matchText)
                if (id < 0 || mask < 0 || match < 0)
                    return qsTr("Enter hex values, e.g. 0x100, FF, 00.")
                if ((match & ~mask & 0xFF) !== 0)
                    return qsTr("Impossible rule: the match value has bits the mask "
                              + "clears, so nothing can ever satisfy it.")
                return qsTr("Logs a frame with id 0x%1 when byte %2 & 0x%3 == 0x%4.")
                       .arg(id.toString(16).toUpperCase()).arg(root.byteIndex)
                       .arg(mask.toString(16).toUpperCase().padStart(2, "0"))
                       .arg(match.toString(16).toUpperCase().padStart(2, "0"))
            }
            color: (root.parseHex(root.matchText) & ~root.parseHex(root.maskText) & 0xFF) !== 0
                   ? Theme.warning : Theme.textDisabled
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeSm
            wrapMode: Text.WordWrap
        }
    }
}
