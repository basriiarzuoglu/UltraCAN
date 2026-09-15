# --------------------------------------------------------------- UltraCAN.Views
# One file per full-page module. Views may depend on Theme, Components, Models
# and Core (the DBC editor/viewer read the C++ DbcModel, the logger reads
# CanBusManager.logModel, GraphPlotView reads PlotSignalCatalog).
qt_add_qml_module(UltraCANViews
    URI UltraCAN.Views
    VERSION 1.0
    STATIC
    QML_FILES
        DbcEditorView.qml
        GraphPlotView.qml
        CanLoggerView.qml
        CanMessageSenderView.qml
        DiagnosticsView.qml
        RawAnalyzerView.qml
        # Graph Plot internals. They need QtCharts and UltraCAN.Core, and
        # UltraCAN.Components is documented as depending on Theme ONLY - so
        # they live here rather than widening that module's dependencies.
        GraphPaneArea.qml
        GraphSplitNode.qml
        GraphWorkspace.qml
        SignalPickerDialog.qml
        MathChannelDialog.qml
        TriggerSetupDialog.qml
        TriggerConditionRow.qml
        LogFilterRow.qml
        SplittableGraphPane.qml
        # Same reasoning: it drives the DbcModel / DbcFlatModel singletons
        # directly, which UltraCAN.Components is not allowed to import.
        CanChannelSetupDialog.qml
    # Declared so qmllint / qmlcachegen know this module's QML pulls in
    # QtCharts types. Without it you get "Warning: Unknown type ChartView".
    DEPENDENCIES
        QtQuick
        QtCharts
        QtQuick.Dialogs
        QtQuick.Window
        UltraCAN.Core
        UltraCAN.Theme
        UltraCAN.Components
)

target_link_libraries(UltraCANViews PRIVATE
    UltraCANThemeplugin
    UltraCANComponentsplugin
    UltraCANModelsplugin
    UltraCANCoreplugin
    Qt6::Charts
)
