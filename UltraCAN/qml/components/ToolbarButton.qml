import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

import UltraCAN.Theme

/*!
    \qmltype ToolbarButton
    \brief Compact action button for page toolbars.

    Set \l primary for the one call-to-action per toolbar, and \l destructive
    for delete-style actions.
*/
Button {
    id: control

    /*! Optional leading glyph. Ignored when \l iconSource is set. */
    property string glyph: ""
    /*!
        Optional leading image, e.g.
        \c {"qrc:/qt/qml/UltraCAN/assets/icons/pause.png"}.

        \b {Mind the prefix.} Files listed under RESOURCES in
        \c qt_add_qml_module land at \c {qrc:/qt/qml/<URI>/<relative path>},
        NOT at \c {qrc:/icons/...}. A wrong path is not an error - Image just
        renders nothing and the button comes up blank.

        The artwork is white on transparent, which reads correctly both on the
        panel fill and on the accent fill of a \l primary button. It is not
        tinted: recolouring an Image needs MultiEffect or the Qt5Compat
        ColorOverlay, and neither is worth pulling in for two shapes. */
    property url iconSource: ""
    /*! Edge length of \l iconSource, in logical pixels. */
    property int iconSize: 14
    /*! Accent-filled emphasis. */
    property bool primary: false
    /*! Error-coloured text/border. */
    property bool destructive: false
    /*!
        Overrides the accent for one button - the logger's Start control has
        THREE resting states (idle, armed, recording) and \l primary /
        \l destructive only name two of them.

        Transparent (the default) means "use the normal rules"; any other
        colour tints the text and the border, leaving the fill alone so the
        button still reads as a toolbar button rather than a badge.
    */
    property color statusColor: "transparent"
    readonly property bool _tinted: control.statusColor.a > 0

    readonly property color _fg: !enabled ? Theme.textDisabled
                                          : _tinted ? control.statusColor
                                          : primary ? Theme.background
                                          : destructive ? Theme.error
                                          : Theme.textPrimary

    implicitHeight: Theme.inputHeight
    leftPadding: Theme.spacingMd
    rightPadding: Theme.spacingMd
    /*!
        Zero vertical padding, on purpose.

        A Control lays its contentItem out inside
        \c{(leftPadding, topPadding, width - l - r, height - t - b)}. The Basic
        style's default \c padding is 6, and this contentItem is a RowLayout -
        which does NOT centre an over-tall child, it places it at the box's top
        edge and lets it overflow downward. So any caller that made the button
        shorter than 12 px + text got a label sitting exactly \c padding pixels
        too low. That is measurable: the DBC grid's Setup buttons were 6 px low
        on every row.

        With the padding at zero the content box IS the button, and the labels'
        own \c{Layout.alignment: Qt.AlignVCenter} does the centring.
    */
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    background: Rectangle {
        radius: Theme.radiusSm
        color: {
            if (!control.enabled)
                return Theme.panel
            if (control._tinted)
                // Was `control.down ? panelElevated : panel` - hover did
                // nothing, so a tinted button (the Logger's tri-state Start
                // control, the Log Replay Arm/Disarm control) looked inert
                // next to every plain button beside it, which DOES lighten
                // on hover. Tinted buttons need the same hover feedback,
                // not less of it - they are usually the more consequential
                // control, not a decorative one.
                return control.down ? Theme.panelElevated
                     : control.hovered ? Theme.panelElevated
                     : Theme.panel
            if (control.primary)
                return control.down ? Theme.accentPressed : Theme.accent
            if (control.down)
                return Theme.panelElevated
            return control.hovered ? Theme.panelElevated : Theme.panel
        }
        // A tinted button's colour IS its meaning (armed/recording/warning),
        // so its border is heavier than the plain 1px hairline the rest of
        // the toolbar uses - it should never be mistaken for a label.
        border.width: control._tinted ? 2 : 1
        border.color: control._tinted ? control.statusColor
                                      : control.primary ? "transparent"
                                      : (control.hovered ? Theme.accent : Theme.border)
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
    }

    contentItem: RowLayout {
        spacing: Theme.spacingXs

        Image {
            visible: control.iconSource.toString().length > 0
            source: control.iconSource
            // Explicit sourceSize, or Qt rasterises the @2x variant at its
            // native 32 px and the button grows to fit it.
            sourceSize.width: control.iconSize
            sourceSize.height: control.iconSize
            Layout.preferredWidth: control.iconSize
            Layout.preferredHeight: control.iconSize
            Layout.alignment: Qt.AlignVCenter
            fillMode: Image.PreserveAspectFit
            smooth: true
            opacity: control.enabled ? 1.0 : 0.4
        }
        Label {
            visible: control.glyph.length > 0
                     && control.iconSource.toString().length === 0
            text: control.glyph
            color: control._fg
            font.pixelSize: Theme.fontSizeMd
            Layout.alignment: Qt.AlignVCenter
        }
        Label {
            text: control.text
            color: control._fg
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeMd
            font.weight: control.primary ? Font.DemiBold : Font.Normal
            Layout.alignment: Qt.AlignVCenter
        }
    }
}
