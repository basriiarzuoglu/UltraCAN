# ---------------------------------------------------------------- UltraCAN.Theme
# Design tokens. Depends on nothing, so every other module may depend on it.

# IMPORTANT: the singleton marker MUST be set BEFORE qt_add_qml_module().
# The qmldir file is generated inside that call - setting the property
# afterwards is silently ignored and the type registers as a normal element,
# which makes every `Theme.<token>` evaluate to undefined at runtime.
set_source_files_properties(Theme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(UltraCANTheme
    URI UltraCAN.Theme
    VERSION 1.0
    STATIC
    QML_FILES
        Theme.qml
)
