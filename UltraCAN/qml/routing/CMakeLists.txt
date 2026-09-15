# ------------------------------------------------------------- UltraCAN.Routing
# The navigation table. It instantiates the view Components, so it sits ABOVE
# Views in the dependency graph. Keeping it out of UltraCAN.Models is what
# prevents a Models <-> Views import cycle now that the views consume models.

set_source_files_properties(Navigation.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(UltraCANRouting
    URI UltraCAN.Routing
    VERSION 1.0
    STATIC
    QML_FILES
        NavRoute.qml
        Navigation.qml
)

target_link_libraries(UltraCANRouting PRIVATE UltraCANViewsplugin)
