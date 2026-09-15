cmake_minimum_required(VERSION 3.16)

project(UltraCAN VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Widgets is NOT optional once Qt Charts is in the build: ChartView renders
# through QGraphicsScene, whose text items call QApplication::style().
find_package(Qt6 6.8 REQUIRED COMPONENTS Quick QuickControls2 Charts Widgets QuickDialogs2 SerialBus)

qt_standard_project_setup(REQUIRES 6.8)

# Qt Creator / qmllint resolve imports from the build dir; keep modules together.
set(QT_QML_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml)

# --- Windows executable icon --------------------------------------------------
# This is what Explorer, a desktop shortcut and a PINNED taskbar button read;
# QApplication::setWindowIcon() in main.cpp only covers windows the running
# process creates. Both are needed - neither substitutes for the other.
#
# enable_language(RC) is not optional: project() above declares LANGUAGES CXX,
# which switches OFF CMake's automatic language detection, so a .rc added to a
# target would be treated as an unknown file type and silently dropped from the
# link - producing a binary with the default Qt icon and no warning at all.
# MinGW compiles it with windres; MSVC with rc.exe. Same line for both.
if(WIN32)
    enable_language(RC)
    # Guarded, because naming a missing file in qt_add_executable() is a hard
    # CONFIGURE error - "Cannot find source file" - not a link-time warning.
    # An absent icon script therefore took the entire build down instead of
    # costing an icon, which is what a fresh clone hit once app_icon.rc turned
    # out to be untracked. Degrade loudly instead.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/app_icon.rc)
        set(ULTRACAN_APP_ICON_RC ${CMAKE_CURRENT_SOURCE_DIR}/app_icon.rc)
    else()
        message(WARNING
            "app_icon.rc is missing - building WITHOUT an executable icon. "
            "Explorer, shortcuts and the pinned taskbar button will show the "
            "default Qt icon. The file is source, not generated: check it out "
            "rather than regenerating it.")
    endif()
endif()

qt_add_executable(appUltraCAN
    main.cpp
    ${ULTRACAN_APP_ICON_RC}      # empty on every other platform
)

# --- The application's own QML module (the shell) ----------------------------
qt_add_qml_module(appUltraCAN
    URI UltraCAN
    VERSION 1.0
    QML_FILES
        Main.qml
    RESOURCES
        # Caption artwork. Listing them here puts them at
        #   qrc:/qt/qml/UltraCAN/assets/icons/<name>
        # which is the path TitleBar.iconRoot defaults to. A file that is NOT
        # listed still loads from disk during development and then silently
        # 404s in a deployed build, so anything the UI references must appear
        # in this block.
        assets/icons/ultracan_logo.png
        assets/icons/minimize-sign.png
        assets/icons/maximize.png
        assets/icons/close.png
        # Transport glyphs. PNG rather than a Unicode codepoint: the Pause
        # button used "❚❚" (HEAVY VERTICAL BAR), which Segoe UI does not
        # carry - it fell back to whatever font did, or to a tofu box.
        # @2x is picked up automatically by Qt on a high-DPI screen.
        assets/icons/pause.png
        assets/icons/pause@2x.png
        assets/icons/play.png
        assets/icons/play@2x.png
        # Application icon, one real render per size. main.cpp hands all of
        # them to a single QIcon so Qt never has to resample.
        assets/icons/ultracan_logo_16.png
        assets/icons/ultracan_logo_24.png
        assets/icons/ultracan_logo_32.png
        assets/icons/ultracan_logo_48.png
        assets/icons/ultracan_logo_64.png
        assets/icons/ultracan_logo_128.png
        assets/icons/ultracan_logo_256.png
)

# --- Sub-modules -------------------------------------------------------------
# Order matters only for readability; CMake resolves the real deps via
# target_link_libraries inside each subdirectory.
# Dependency order (acyclic):
#   Core   <-  everything (C++ backend, Qt only)
#   Theme  <-  Components  <-  Views  <-  Routing
#   Theme  <-  Models      <-  Views
add_subdirectory(src)
add_subdirectory(qml/theme)
add_subdirectory(qml/components)
add_subdirectory(qml/models)
add_subdirectory(qml/views)
add_subdirectory(qml/routing)

target_link_libraries(appUltraCAN PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    # Linking Charts here is what makes windeployqt ship the QtCharts QML
    # plugin next to the executable; the QML `import QtCharts` alone would
    # build fine and then fail at runtime in a deployed folder.
    Qt6::Charts
    # Required by Qt Charts (QGraphicsScene / QStyle). main.cpp must also use
    # QApplication rather than QGuiApplication - see the comment there.
    Qt6::Widgets
    # Backs `import QtQuick.Dialogs` (FileDialog in the DBC editor / viewer);
    # linking it is what makes windeployqt ship the QML plugin.
    Qt6::QuickDialogs2
    # Each qt_add_qml_module target ships an auto-generated "<target>plugin"
    # that registers the module's types. Linking the plugin is what makes
    # `import UltraCAN.Theme` resolve in a statically linked QML build.
    # C++ backend: the library for main.cpp, the plugin for `import UltraCAN.Core`.
    UltraCANCore
    UltraCANCoreplugin
    UltraCANThemeplugin
    UltraCANComponentsplugin
    UltraCANModelsplugin
    UltraCANViewsplugin
    UltraCANRoutingplugin
)

target_compile_definitions(appUltraCAN PRIVATE APP_VERSION="${PROJECT_VERSION}")

set_target_properties(appUltraCAN PROPERTIES
    # The CMake target keeps Qt Creator's "app" prefix; the FILE users double
    # click must not. This is also what the README and the installer tell them
    # to run, so a mismatch here is a broken instruction, not a cosmetic one.
    OUTPUT_NAME UltraCAN
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS appUltraCAN
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# --- Linux desktop integration ------------------------------------------------
# What makes the installed binary an APPLICATION rather than a file in
# /usr/bin: the launcher entry, and the icon that entry names.
#
# UNIX AND NOT APPLE, not UNIX: macOS carries its launcher metadata inside the
# .app bundle (MACOSX_BUNDLE above), and an XDG entry there is dead weight.
#
# The two destinations are not interchangeable and neither is optional:
#   - applications/ultracan.desktop is what the menu reads. Its base name must
#     match the string main.cpp passes to setDesktopFileName(), or a Wayland
#     compositor cannot tie a running window back to this entry.
#   - the icon is installed under the NAME the entry's Icon= key asks for -
#     "ultracan" - which is why it is RENAMEd. Icon= is a theme LOOKUP key, not
#     a path: an icon left as ultracan_logo_256.png is simply never found, and
#     the entry falls back to a blank generic glyph with no warning anywhere.
#
# hicolor is the fallback theme every icon theme inherits from, so one entry
# there works under GNOME, KDE, XFCE and the rest.
if(UNIX AND NOT APPLE)
    # The .desktop file lives at the REPOSITORY root, one level above this
    # CMake source directory - move it next to CMakeLists.txt and this path
    # becomes a plain ${CMAKE_CURRENT_SOURCE_DIR}/ultracan.desktop.
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../ultracan.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )

    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/ultracan_logo_256.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
        RENAME ultracan.png
    )
endif()

# =============================================================================
#                       WINDOWS DEPLOYMENT AND INSTALLER
# =============================================================================
#
# Two targets:
#   Deploy           - a self-contained folder that runs on a clean machine
#   CreateInstaller  - UltraCAN_Setup.exe, built from that folder
#
# ---------------------------------------------------------------------------
# Why deployment goes to build/deploy and NOT next to the .exe
# ---------------------------------------------------------------------------
# The obvious arrangement - let windeployqt drop Qt's DLLs beside the binary
# and have the installer package the build directory - packages the build
# directory. That means CMakeFiles/, every .obj and .a, the .rcc caches, and
# ${CMAKE_BINARY_DIR}/qml, which is the QML module output tree this project
# sets up for qmllint. Several hundred megabytes of it.
#
# The qml/ folder is not merely wasted space: this application compiles its QML
# INTO the binary via qt_add_qml_module, and shipping a loose copy of the same
# modules next to the executable gives the QML engine a second place to resolve
# them from. An installed build would then be running files from the developer's
# machine instead of its own resources - a class of bug that reproduces nowhere.
#
# So deployment stages into a directory that contains nothing but the shipped
# application, and the installer packages that directory wholesale.
if(WIN32)
    set(ULTRACAN_DEPLOY_DIR "${CMAKE_BINARY_DIR}/deploy")

    # --- locate windeployqt -------------------------------------------------
    # Three independent sources, because each one is missing in some setup:
    # QT_QMAKE_EXECUTABLE is set by the Qt Creator kit, Qt6::qmake exists once
    # Qt6Core is found, and CMAKE_PREFIX_PATH is what a command-line build gets.
    set(_ultracan_qt_bin_hints "")
    if(QT_QMAKE_EXECUTABLE)
        get_filename_component(_dir "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
        list(APPEND _ultracan_qt_bin_hints "${_dir}")
    endif()
    if(TARGET Qt6::qmake)
        get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
        if(_qmake)
            get_filename_component(_dir "${_qmake}" DIRECTORY)
            list(APPEND _ultracan_qt_bin_hints "${_dir}")
        endif()
    endif()
    foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
        list(APPEND _ultracan_qt_bin_hints "${_prefix}/bin")
    endforeach()

    find_program(WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt windeployqt.exe
        HINTS ${_ultracan_qt_bin_hints}
    )

    # --- MinGW runtime ------------------------------------------------------
    # Without these three the .exe does not start on a machine that has no Qt
    # installation - it fails at load time with no message the user can act on.
    # windeployqt's --compiler-runtime can copy them, but only if the compiler's
    # bin directory happens to be on PATH; deriving it from the compiler CMake
    # is already using is deterministic. Globbed rather than named so a MinGW
    # revision that renames one does not fail the build.
    get_filename_component(_ultracan_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(ULTRACAN_RUNTIME_DLLS "")
    if(MINGW)
        foreach(_pattern libgcc_s_*.dll libstdc++-*.dll libwinpthread-*.dll)
            file(GLOB _found "${_ultracan_compiler_bin}/${_pattern}")
            list(APPEND ULTRACAN_RUNTIME_DLLS ${_found})
        endforeach()
        if(NOT ULTRACAN_RUNTIME_DLLS)
            message(WARNING
                "No MinGW runtime DLLs found in ${_ultracan_compiler_bin}. "
                "The deployed build will not start on a machine without MinGW.")
        endif()
    endif()

    # --- the deployment steps, defined once and used twice ------------------
    # A macro rather than two copies: a deploy that runs on POST_BUILD and a
    # deploy run by hand must be the SAME deploy, or the installer packages
    # something no developer has tested.
    macro(ultracan_deploy_steps _out)
        set(${_out}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${ULTRACAN_DEPLOY_DIR}"
            # The executable first: windeployqt resolves dependencies OF a
            # binary and writes them beside it. Pointing it at the original in
            # the build tree and redirecting with --dir splits the two apart,
            # so the copy in deploy/ is what gets deployed against.
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:appUltraCAN>" "${ULTRACAN_DEPLOY_DIR}/"
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                    --$<IF:$<CONFIG:Debug>,debug,release>
                    # Scans the QML SOURCES for imports and copies the Qt Quick
                    # plugins they need. Without it the app starts and then dies
                    # on "module QtQuick is not installed" - the single most
                    # common Qt Quick deployment failure.
                    --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                    # Where THIS project's own modules live, so windeployqt can
                    # resolve `import UltraCAN.Theme` instead of warning about it.
                    # They are compiled into the binary; nothing is copied.
                    --qmlimport "${CMAKE_BINARY_DIR}/qml"
                    --no-translations
                    --no-system-d3d-compiler
                    # NOTE: deliberately NOT --no-opengl-sw. It saves ~20 MB and
                    # costs a blank window on any machine without a usable GPU
                    # driver - an industrial PC, a VM, or an RDP session. All
                    # three are normal places to run a bus analyzer.
                    --verbose 1
                    "${ULTRACAN_DEPLOY_DIR}/$<TARGET_FILE_NAME:appUltraCAN>"
        )
        if(ULTRACAN_RUNTIME_DLLS)
            list(APPEND ${_out}
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${ULTRACAN_RUNTIME_DLLS} "${ULTRACAN_DEPLOY_DIR}/")
        endif()
    endmacro()

    if(WINDEPLOYQT_EXECUTABLE)
        ultracan_deploy_steps(ULTRACAN_DEPLOY_CMDS)

        # --- Deploy: always available, run on demand ------------------------
        add_custom_target(Deploy
            ${ULTRACAN_DEPLOY_CMDS}
            DEPENDS appUltraCAN
            COMMENT "Staging a self-contained build in ${ULTRACAN_DEPLOY_DIR}"
            VERBATIM
        )

        # --- and optionally after every build -------------------------------
        # Defaults ON for Release only. windeployqt copies tens of megabytes and
        # takes seconds; paying that after every incremental DEBUG build, when
        # Qt Creator already runs the binary against the Qt install, is a tax
        # for nothing. Set -DULTRACAN_AUTO_DEPLOY=ON to force it on either way.
        if(CMAKE_BUILD_TYPE STREQUAL "Release"
           OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"
           OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
            set(_ultracan_auto_deploy_default ON)
        else()
            set(_ultracan_auto_deploy_default OFF)
        endif()
        option(ULTRACAN_AUTO_DEPLOY "Run windeployqt after every build"
               ${_ultracan_auto_deploy_default})

        if(ULTRACAN_AUTO_DEPLOY)
            add_custom_command(TARGET appUltraCAN POST_BUILD
                ${ULTRACAN_DEPLOY_CMDS}
                COMMENT "Deploying Qt runtime to ${ULTRACAN_DEPLOY_DIR}"
                VERBATIM
            )
        endif()
    else()
        message(WARNING
            "windeployqt not found - the Deploy and CreateInstaller targets are "
            "unavailable. Looked in: ${_ultracan_qt_bin_hints}")
    endif()

    # --- CreateInstaller ----------------------------------------------------
    find_program(ISCC_EXECUTABLE
        NAMES ISCC ISCC.exe
        HINTS
            "$ENV{PROGRAMFILES}/Inno Setup 6"
            "$ENV{ProgramFiles\(x86\)}/Inno Setup 6"
            "C:/Program Files (x86)/Inno Setup 6"
            "C:/Program Files/Inno Setup 6"
    )

    # Generated at CONFIGURE time. This project uses a single-config generator
    # (MinGW Makefiles), so CMAKE_BUILD_TYPE is already known here. Under a
    # multi-config generator the deploy path would need a generator expression
    # and this would have to move to a build-time configure_file.
    set(ULTRACAN_INSTALLER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/installer")

    # Inno tolerates forward slashes, but a Source line that reads
    # "C:/build/deploy\*" mixes both separators and is a nuisance to eyeball in
    # a log. Hand the template native paths and every path in the .iss looks
    # like a Windows path.
    file(TO_NATIVE_PATH "${ULTRACAN_DEPLOY_DIR}" ULTRACAN_DEPLOY_DIR_NATIVE)
    file(TO_NATIVE_PATH "${ULTRACAN_INSTALLER_OUTPUT_DIR}" ULTRACAN_INSTALLER_OUTPUT_DIR_NATIVE)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/ultracan_logo.ico"
         ULTRACAN_SETUP_ICON_NATIVE)

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/installer.iss.in"
        "${CMAKE_BINARY_DIR}/installer.iss"
        @ONLY
    )

    if(ISCC_EXECUTABLE AND WINDEPLOYQT_EXECUTABLE)
        add_custom_target(CreateInstaller
            COMMAND ${CMAKE_COMMAND} -E make_directory "${ULTRACAN_INSTALLER_OUTPUT_DIR}"
            COMMAND "${ISCC_EXECUTABLE}" "/Qp" "${CMAKE_BINARY_DIR}/installer.iss"
            # Depends on the TARGET, not on the POST_BUILD hook: this is what
            # makes the installer buildable with ULTRACAN_AUTO_DEPLOY off, and
            # what guarantees the folder being packaged was staged by this
            # build rather than left over from a previous one.
            DEPENDS Deploy
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            COMMENT "Compiling ${ULTRACAN_INSTALLER_OUTPUT_DIR}/UltraCAN_Setup.exe"
            VERBATIM
        )
    elseif(NOT ISCC_EXECUTABLE)
        message(STATUS
            "Inno Setup (ISCC.exe) not found - CreateInstaller not defined. "
            "Install Inno Setup 6, or pass -DISCC_EXECUTABLE=<path>.")
    endif()
endif()
