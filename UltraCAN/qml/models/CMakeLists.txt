# -------------------------------------------------------------- UltraCAN.Models
# Effectively empty now. The DBC database, the live trace, the plot buffers,
# the transmit jobs and (as of phase 12) the recorded trace are all C++
# (UltraCAN.Core). CanLogDummyModel.qml is the last file here and nothing
# imports it - the module is kept only so the four views that still carry a
# stale `import UltraCAN.Models` keep resolving. Deleting both is a safe
# follow-up: drop this directory, its add_subdirectory(), and the
# UltraCANModelsplugin entries in the root and views CMakeLists.

# Singleton markers MUST precede qt_add_qml_module() - the qmldir is generated
# inside that call and a later property change is silently ignored.
set_source_files_properties(
        CanLogDummyModel.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(UltraCANModels
    URI UltraCAN.Models
    VERSION 1.0
    STATIC
    QML_FILES
        CanLogDummyModel.qml
    DEPENDENCIES
        QtQuick
        UltraCAN.Core
)

target_link_libraries(UltraCANModels PRIVATE UltraCANCoreplugin)
