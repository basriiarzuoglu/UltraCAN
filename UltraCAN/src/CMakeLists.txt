# ---------------------------------------------------------------- UltraCAN.Core
# The C++ backend: bus clock, hardware device, frame type, and the real models
# that replace the QML dummies. Exposed to QML as its own module so the views
# can simply `import UltraCAN.Core` - no qmlRegisterType calls anywhere.
#
# Depends on Qt only, so it sits at the very bottom of the dependency graph:
#   Core <- Views, Core <- app
qt_add_library(UltraCANCore STATIC)

qt_add_qml_module(UltraCANCore
    URI UltraCAN.Core
    VERSION 1.0
    # C++ types marked QML_ELEMENT / QML_SINGLETON go in SOURCES, not QML_FILES.
    # List the headers AND the .cpp files: moc must see the headers, and the
    # generated <target>_qmltyperegistrations.cpp is built from what moc found.
    SOURCES
        core/CanFrame.h
        core/CanBusManager.h
        core/CanBusManager.cpp
        core/SettingsManager.h
        core/SettingsManager.cpp
        core/LogExporter.h
        core/LogExporter.cpp
        core/TraceReader.h
        core/TraceReader.cpp
        core/TraceFormat.h
        core/TraceFormat.cpp
        core/LogFilter.h
        core/TriggerCondition.h
        core/BusReplayPacer.h
        core/BusReplayPacer.cpp
        core/TraceStreamWriter.h
        core/TraceStreamWriter.cpp
        core/ConfigManager.h
        core/ConfigManager.cpp
        can/CanDevice.h
        can/CanDevice.cpp
        uds/IsoTpLayer.h
        uds/IsoTpLayer.cpp
        uds/UdsClient.h
        uds/UdsClient.cpp
        dbc/DbcParser.h
        dbc/DbcParser.cpp
        dbc/ValueFormat.h
        dbc/CanDecoder.h
        dbc/CanDecoder.cpp
        models/CanAnalyzerModel.h
        models/CanAnalyzerModel.cpp
        models/CanLogModel.h
        models/CanLogModel.cpp
        models/CanLogSortProxyModel.h
        models/CanLogSortProxyModel.cpp
        models/CanPlaybackModel.h
        models/CanPlaybackModel.cpp
        models/CanSenderModel.h
        models/CanSenderModel.cpp
        models/DbcFlatModel.h
        models/DbcFlatModel.cpp
        models/DbcModel.h
        models/DbcModel.cpp
        models/LogReplayModel.h
        models/LogReplayModel.cpp
        models/GraphDataModel.h
        models/GraphDataModel.cpp
        models/MathChannelManager.h
        models/MathChannelManager.cpp
        ui/BitStripItem.h
        ui/BitStripItem.cpp
)

# CRITICAL: qmltyperegistrar writes the BASENAME of each header into the
# generated registration file, guarded by __has_include:
#
#     #if __has_include(<CanBusManager.h>)
#     #  include <CanBusManager.h>
#     #endif
#
# With the headers in subdirectories, <CanBusManager.h> does not resolve, the
# guard silently drops the include (so there is no "file not found") and the
# build dies with "'CanBusManager' was not declared in this scope". Every
# directory that holds a moc'd header must therefore be on the include path by
# itself - not just their common parent.
target_include_directories(UltraCANCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}            # lets us write "core/CanFrame.h"
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/can
    ${CMAKE_CURRENT_SOURCE_DIR}/uds
    ${CMAKE_CURRENT_SOURCE_DIR}/dbc
    ${CMAKE_CURRENT_SOURCE_DIR}/models
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)

# Qt6::SerialBus provides QCanBus / QCanBusDevice / QCanBusFrame. PUBLIC because
# CanDevice.h includes <QCanBusDevice> in its interface.
target_link_libraries(UltraCANCore PUBLIC
    Qt6::Quick
    Qt6::SerialBus
    # GraphDataModel drives QXYSeries directly - the only fast way to feed a
    # chart from C++ (QXYSeries::replace() instead of per-point append()).
    Qt6::Charts
)
