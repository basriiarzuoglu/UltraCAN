# ----------------------------------------------------------- UltraCAN.Components
# Reusable, view-agnostic UI building blocks. May depend on Theme only -
# never on Models or Views (keeps the dependency graph acyclic).
qt_add_qml_module(UltraCANComponents
    URI UltraCAN.Components
    VERSION 1.0
    STATIC
    QML_FILES
        # shell
        BasePage.qml
        TitleBar.qml
        WindowResizeBorders.qml
        BitMatrix.qml
        NavButton.qml
        Sidebar.qml
        LoadingOverlay.qml
        # form / input primitives
        PropertyField.qml
        ReadOnlyPropertyRow.qml
        ThemedCheckBox.qml
        ThemedComboBox.qml
        ThemedSlider.qml
        ThemedSpinBox.qml
        ThemedSwitch.qml
        ThemedTextField.qml
        ToolbarButton.qml
        # Takes the signal to edit as a plain property and imports Theme only,
        # so it stays inside this module's Theme-only contract.
        SignalValuesDialog.qml
        # domain-flavoured but model-agnostic widgets
        DbcExplorer.qml
        TableHeaderBar.qml
        LogRow.qml
    # Declared so qmllint / qmlcachegen resolve the modules this QML imports.
    # Deliberately does NOT list UltraCAN.Core: this module's contract is Theme
    # only, and anything needing a backend singleton belongs in UltraCAN.Views.
    DEPENDENCIES
        QtQuick
        QtQuick.Layouts
        QtQuick.Controls.Basic
        QtQuick.Window
        UltraCAN.Theme
)

target_link_libraries(UltraCANComponents PRIVATE UltraCANThemeplugin)
