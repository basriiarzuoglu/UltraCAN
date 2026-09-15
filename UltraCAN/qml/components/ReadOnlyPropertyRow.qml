import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import UltraCAN.Theme

/*!
    \qmltype ReadOnlyPropertyRow
    \brief One "Property : Value" line of a datasheet-style inspector.

    The label is dimmed, the value is high-contrast and selectable, so the
    reader can copy an id or a factor straight out of the sheet without the
    page ever becoming editable.
*/
RowLayout {
    id: root

    /*! Left-hand property name. */
    property string label: ""
    /*! Right-hand value, already formatted for display. */
    property string value: ""
    /*! Optional unit rendered after the value in a dim tone. */
    property string unit: ""
    /*! Width of the label column; keep identical across one sheet. */
    property int labelWidth: Theme.labelColumnWidth
    /*! Render the value in the monospace face (ids, bit positions, numbers). */
    property bool mono: false
    /*! Tint the value with the accent colour (use for the primary identifier). */
    property bool highlighted: false
    /*! Page gutter; rows align with the section captions by default. */
    property int horizontalMargin: Theme.spacingLg

    Layout.fillWidth: true
    Layout.leftMargin: horizontalMargin
    Layout.rightMargin: horizontalMargin
    spacing: Theme.spacingMd

    Label {
        Layout.preferredWidth: root.labelWidth
        Layout.alignment: Qt.AlignTop
        text: root.label
        color: Theme.textDisabled
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeMd
        horizontalAlignment: Text.AlignRight
        elide: Text.ElideRight
    }

    // TextEdit rather than Label: read-only, but the value stays selectable
    // and copyable - the thing people actually want from a datasheet.
    TextEdit {
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignTop
        text: root.value.length > 0 ? root.value : "-"
        readOnly: true
        selectByMouse: true
        wrapMode: Text.WordWrap
        color: root.value.length === 0 ? Theme.textDisabled
                                       : (root.highlighted ? Theme.accent : Theme.textPrimary)
        selectionColor: Theme.accent
        selectedTextColor: Theme.background
        font.family: root.mono ? Theme.monoFontFamily : Theme.fontFamily
        font.pixelSize: Theme.fontSizeMd
        font.weight: root.highlighted ? Font.DemiBold : Font.Normal
    }

    Label {
        visible: root.unit.length > 0
        Layout.preferredWidth: 40
        Layout.alignment: Qt.AlignTop
        text: root.unit
        color: Theme.textDisabled
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontSizeSm
    }
}
