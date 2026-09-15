pragma Singleton

import QtQuick

/*!
    \qmltype Theme
    \brief Central design-token singleton for the whole application.

    Every colour, radius, spacing and font size used by the UI MUST come from
    here. Never hardcode a colour literal inside a view or component: doing so
    breaks theming and makes a future light-theme / high-contrast mode
    impossible.
*/
QtObject {
    id: root

    // ---------------------------------------------------------------- colors
    // Industrial dark palette (Dewesoft-inspired).
    readonly property color background:      "#1E1E1E"   // app canvas
    readonly property color surface:         "#252526"   // content area
    readonly property color panel:           "#2D2D2D"   // sidebar / cards
    readonly property color panelElevated:   "#383838"   // hover / raised
    readonly property color border:          "#3C3C3C"   // 1px separators

    readonly property color accent:          "#FF8C1A"   // primary (orange)
    readonly property color accentPressed:   "#E07715"
    readonly property color accentAlt:       "#0A84FF"   // secondary (blue)

    readonly property color textPrimary:     "#E8E8E8"
    readonly property color textSecondary:   "#9D9D9D"
    readonly property color textDisabled:    "#5A5A5A"

    readonly property color selection:       "#3A4A5C"   // selected row fill
    readonly property color selectionText:   "#FFFFFF"

    // -- window chrome ------------------------------------------------------
    // The close button's hover fill is Windows' own #E81123 rather than
    // Theme.error: a custom title bar that reacts in a DIFFERENT red from every
    // other window on the desktop reads as a bug, not as branding.
    readonly property color closeHover:        "#E81123"
    readonly property color closeHoverPressed: "#C50E1F"
    /*! Tint+opacity for the drop shadow behind the rounded main window. Alpha
        does the actual work here - this is deliberately translucent black,
        not a themed hue, the same way "transparent" is used as a literal
        elsewhere rather than a token. */
    readonly property color windowShadow:      "#80000000"

    // -- plotting -----------------------------------------------------------
    readonly property color plotBackground:  "#1A1A1A"   // plot area fill
    readonly property color gridLine:        "#333333"   // subtle chart grid
    readonly property color gridLineMinor:   "#292929"
    readonly property color cursorLine:      "#6E6E6E"

    /*! Distinct, high-contrast trace colours. Index with signal order. */
    readonly property var seriesPalette: [
        "#00E5FF",  // cyan
        "#FF8C1A",  // orange
        "#7CFF4F",  // lime
        "#FF4FD8",  // magenta
        "#FFD84F",  // amber
        "#4F9BFF",  // blue
        "#FF5C5C",  // red
        "#4FFFC4"   // mint
    ]

    function seriesColor(index) {
        return seriesPalette[index % seriesPalette.length]
    }

    // -- raw data analyzer heatmap ------------------------------------------
    /*!
        Activity ramp, coolest first. Perceptually ordered black -> blue ->
        purple -> magenta -> orange -> yellow -> white, so "hotter" reads as
        hotter at a glance and the busiest bits are the brightest thing on the
        screen. Interpolated by BitStripItem; the C++ side holds no colours.
    */
    readonly property var heatRamp: [
        "#0B0B10",  // idle - near black, not pure black, so a cell still reads as a cell
        "#1B2A6B",  // barely moving
        "#4B2C9B",
        "#8B2BA8",
        "#C42E7E",
        "#F2542D",
        "#FFA51F",
        "#FFE45C",
        "#FFFFFF"   // toggles on essentially every frame
    ]
    readonly property color heatInactive:    "#08080C"
    readonly property color heatGlyph:       "#6E6E78"

    readonly property color success:         "#3FB950"
    readonly property color warning:         "#D29922"
    readonly property color error:           "#F85149"

    // --------------------------------------------------------------- metrics
    readonly property int   spacingXs:   4
    readonly property int   spacingSm:   8
    readonly property int   spacingMd:   12
    readonly property int   spacingLg:   20
    readonly property int   spacingXl:   32

    readonly property int   radiusSm:    3
    readonly property int   radiusMd:    6

    readonly property int   sidebarWidth:          232
    readonly property int   sidebarCollapsedWidth: 56
    readonly property int   headerHeight:          44
    readonly property int   titleBarHeight:        36
    /*! Width of one caption button. 46 px is the Windows caption-button width;
        matching it means muscle memory for "close" still lands. */
    readonly property int   windowButtonWidth:     46
    /*! Thickness of the invisible strips that hand a drag to the OS resizer.
        Below ~5 px it becomes hard to hit; above ~8 px it starts eating clicks
        meant for the content underneath. */
    readonly property int   resizeBorder:          6
    readonly property int   statusBarHeight:       26
    /*! Corner radius of the ROOT window surface - deliberately larger than
        radiusSm/radiusMd, which are for controls and dialogs sitting inside
        it. A window's own silhouette reads as more "modern OS chrome" than
        any button, so it earns a more visible round. */
    readonly property int   windowRadius:          10
    /*! Transparent margin left between the true (frameless) window edge and
        the visible rounded surface, on all four sides. This is where the
        drop shadow is actually drawn - shrink it and the blur gets clipped
        by the window bounds before it can fade out. */
    readonly property int   windowShadowMargin:    16
    readonly property int   navItemHeight:         42
    readonly property int   toolbarHeight:         34
    readonly property int   inputHeight:           28
    readonly property int   rowHeight:             26
    readonly property int   treeIndent:            22
    readonly property int   labelColumnWidth:      120

    // -- property-form grid --------------------------------------------------
    // One label width shared by every row of a form is the whole reason those
    // forms line up; three hand-typed values is how they stop lining up.
    readonly property int   formLabelWidth:        96
    readonly property int   formInputWidth:        128
    /*! How far an input may be squeezed before the grid starts eliding labels
        instead. Without a floor, one long field takes the whole shortfall. */
    readonly property int   formInputMinWidth:     64

    // ----------------------------------------------------------------- fonts
    readonly property string fontFamily:     "Segoe UI"
    readonly property string monoFontFamily: "Consolas"

    readonly property int   fontSizeSm:      11
    readonly property int   fontSizeMd:      13
    /*! Live numeric readouts that get glanced at from a distance rather than
        read - the graph legend's value column. Deliberately between Md and Lg:
        Lg is a heading size and makes a four-trace legend dominate the pane. */
    readonly property int   fontSizeReadout: 15
    readonly property int   fontSizeLg:      16
    readonly property int   fontSizeXl:      20

    // ------------------------------------------------------------ animations
    readonly property int   durationFast:    120
    readonly property int   durationNormal:  200
}
