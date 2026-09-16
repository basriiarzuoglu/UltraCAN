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

# Directories that install() creates on its way to a destination inherit the
# BUILD MACHINE'S UMASK, which on most developer accounts is 002 - so the .deb
# ships /usr, /usr/bin and /usr/share as 0775 and lintian rejects every one of
# them (non-standard-dir-perm). Pinning the mode here makes the packaged tree
# identical whatever umask the build ran under, which is the only way a package
# built on a laptop and a package built in CI can be the same package.
# Must be set BEFORE the install() rules below: it is read as they are
# generated, not when CPack runs.
set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE)                    # 0755

include(GNUInstallDirs)

# --- standalone layout: where a bundled build puts itself ---------------------
# ULTRACAN_BUNDLE_QT makes the Linux package self-contained: the Qt libraries,
# the Qt plugins and the QML modules travel INSIDE it, so the application runs
# on a distribution whose own Qt is 6.2, or that has no Qt at all. It is the
# Linux counterpart of the windeployqt step further down.
#
# Off means "link against the Qt the target system provides", which is the
# right answer when packaging FOR a distribution rather than around one.
option(ULTRACAN_BUNDLE_QT
    "Ship Qt's libraries, plugins and QML modules inside the Linux package"
    ON)

if(UNIX AND NOT APPLE AND ULTRACAN_BUNDLE_QT)
    # ------------------------------------------------------------------------
    # WHY THE BINARY MOVES OUT OF /usr/bin
    # ------------------------------------------------------------------------
    # A bundled Qt cannot go anywhere the system linker already looks. Dropping
    # libQt6Core.so.6 into /usr/lib (or the multiarch dir underneath it) does
    # not give THIS application its own Qt - it replaces the Qt of every other
    # application on the machine, including the desktop shell. The whole bundle
    # therefore lives in one private directory that nothing else reads:
    #
    #   /usr/lib/ultracan/bin/UltraCAN     the real executable
    #   /usr/lib/ultracan/bin/qt.conf      tells Qt where the rest is
    #   /usr/lib/ultracan/lib/             libQt6*.so.6 and friends
    #   /usr/lib/ultracan/plugins/         platforms/, imageformats/, ...
    #   /usr/lib/ultracan/qml/             QtQuick/, QtCharts/, ...
    #   /usr/bin/UltraCAN                  symlink into the above
    #
    # qt.conf is the reason the executable moves rather than staying in
    # /usr/bin with an RPATH. Qt looks for qt.conf next to the running
    # executable, so a bundled app in /usr/bin would have to ship
    # /usr/bin/qt.conf - a file every OTHER Qt program in /usr/bin would then
    # read as its own, sending them all to look for plugins in our private
    # tree. It is a system-wide break, not an inconvenience.
    #
    # The symlink keeps `Exec=UltraCAN` in the desktop entry working: the
    # loader resolves $ORIGIN against the REAL path of the executable (it reads
    # /proc/self/exe, which follows symlinks), so the bundle is found whichever
    # name started it.
    set(ULTRACAN_PRIVATE_DIR "lib/ultracan")
    set(ULTRACAN_RUNTIME_DESTINATION "${ULTRACAN_PRIVATE_DIR}/bin")
    set(ULTRACAN_BUNDLING TRUE)
else()
    set(ULTRACAN_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    set(ULTRACAN_BUNDLING FALSE)
endif()

if(ULTRACAN_BUNDLING)
    # The bundle's own libraries must win over anything the system offers, and
    # $ORIGIN is what makes that independent of where the package is unpacked.
    # Set on the target rather than through CMAKE_INSTALL_RPATH so it survives
    # whatever qt_standard_project_setup() decided for the build tree.
    #
    # INSTALL_RPATH_USE_LINK_PATH stays OFF (its default): switching it on
    # would append the BUILD machine's Qt directory to this list, and the
    # packaged binary would then silently prefer a developer's ~/Qt over its
    # own bundle on any machine that happened to have one.
    set_target_properties(appUltraCAN PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
    )
endif()

install(TARGETS appUltraCAN
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${ULTRACAN_RUNTIME_DESTINATION}
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

    # Debian policy 12.5: every package MUST ship
    # /usr/share/doc/<package>/copyright. It is not advisory - lintian raises
    # no-copyright-file as an ERROR and an archive will refuse the upload. The
    # destination uses the DEBIAN package name (lowercase "ultracan"), not the
    # CMake project name, because that is the directory dpkg looks in.
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/ultracan
        RENAME copyright
    )

    # --- the Qt bundle ------------------------------------------------------
    if(ULTRACAN_BUNDLING)
        # qt_generate_deploy_QML_app_script, not qt_generate_deploy_app_script.
        # The plain one deploys libraries and plugins only; this is a Qt Quick
        # application, and its `import QtQuick`, `import QtCharts` and
        # `import QtQuick.Dialogs` resolve against Qt's qml/ tree at RUNTIME
        # even though this project's OWN QML is compiled into the binary. The
        # non-QML variant produces a package that links fine and then dies on
        # "module QtQuick is not installed".
        qt_generate_deploy_qml_app_script(
            TARGET appUltraCAN
            OUTPUT_SCRIPT ULTRACAN_DEPLOY_SCRIPT
            # The generated script hard-codes GENERATE_QT_CONF, which is what
            # writes lib/ultracan/bin/qt.conf and points Qt at the private
            # plugins/ and qml/ directories below.
            NO_UNSUPPORTED_PLATFORM_ERROR
        )

        # These QT_DEPLOY_* variables are the documented way to redirect the
        # deployment, and they have to be set HERE rather than in this file's
        # scope: install(SCRIPT) runs the deploy script inside the generated
        # cmake_install.cmake, which shares no variables with configure time.
        # install(CODE) injects them into that same script scope, and every
        # QT_DEPLOY_* default is guarded with if(NOT ...), so a value set first
        # wins.
        #
        # Every one of them MUST be overridden. The defaults are relative to
        # the install prefix - lib (i.e. the multiarch system library dir),
        # plugins, qml - which under CPACK_PACKAGING_INSTALL_PREFIX=/usr means
        # /usr/lib/x86_64-linux-gnu, /usr/plugins and /usr/qml. The first of
        # those overwrites the distribution's Qt; the other two are not
        # directories any Linux system has.
        install(CODE "
            set(QT_DEPLOY_BIN_DIR          \"${ULTRACAN_PRIVATE_DIR}/bin\")
            set(QT_DEPLOY_LIB_DIR          \"${ULTRACAN_PRIVATE_DIR}/lib\")
            set(QT_DEPLOY_PLUGINS_DIR      \"${ULTRACAN_PRIVATE_DIR}/plugins\")
            set(QT_DEPLOY_QML_DIR          \"${ULTRACAN_PRIVATE_DIR}/qml\")
            set(QT_DEPLOY_TRANSLATIONS_DIR \"${ULTRACAN_PRIVATE_DIR}/translations\")
            # The deploy script copies with file(INSTALL), which creates its
            # directories under the build machine's umask - 0775 on a typical
            # developer account, which lintian rejects on every one of the
            # hundreds of directories a Qt bundle contains. Same reasoning as
            # the top-level setting; it has to be repeated because this is a
            # different script scope.
            set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
                OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE)
        ")
        install(SCRIPT ${ULTRACAN_DEPLOY_SCRIPT})

        # --- clean up after the deploy script ----------------------------------
        # Two things the Qt deployment API leaves in a state Debian rejects.
        # Shell tools rather than CMake loops: this whole branch is Linux-only,
        # and `find -exec ... +` does in one process what file(GLOB_RECURSE) plus
        # a foreach would do in thousands.
        install(CODE "
            set(_ultracan_bundle \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${ULTRACAN_PRIVATE_DIR}\")

            # 1. STRIP. CPACK_STRIP_FILES cannot reach any of this: it works by
            #    handing CMAKE_INSTALL_DO_STRIP to the install rules CMake
            #    generates, and the Qt deploy script copies with a bare
            #    file(INSTALL), which ignores it. Most Qt libraries arrive
            #    already stripped; the ICU ones the Qt online installer ships do
            #    not, and they are both the largest files in the bundle and
            #    three lintian errors.
            #
            #    lib/ ONLY. The plugins under plugins/ and qml/ have just had
            #    their RPATH rewritten by patchelf, and running strip across a
            #    freshly patched ELF is a known way to corrupt one.
            find_program(_ultracan_strip strip)
            if(_ultracan_strip)
                execute_process(
                    COMMAND find \"\${_ultracan_bundle}/lib\" -type f -name \"*.so*\"
                            -exec \"\${_ultracan_strip}\" --strip-unneeded {} +
                    ERROR_QUIET
                )
            endif()

            # 2. PERMISSIONS. file(INSTALL) keeps the source mode and the
            #    generated files (qt.conf, and every .qm lconvert writes) are
            #    created under the build machine's umask - 0664 and 0775 on a
            #    typical developer account. Same determinism argument as
            #    CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS at the top of this
            #    file; it has to be redone here because these files were never
            #    created by an install() rule.
            execute_process(COMMAND find \"\${_ultracan_bundle}\" -type d -exec chmod 0755 {} +)
            execute_process(COMMAND find \"\${_ultracan_bundle}\" -type f -exec chmod 0644 {} +)
            execute_process(COMMAND chmod 0755 \"\${_ultracan_bundle}/bin/UltraCAN\")
        ")

        # /usr/bin/UltraCAN -> ../lib/ultracan/bin/UltraCAN
        #
        # install(DIRECTORY) with no source creates the directory through the
        # normal install machinery, so it picks up the pinned 0755 above;
        # file(MAKE_DIRECTORY) inside install(CODE) would take the umask again.
        install(DIRECTORY DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(CODE "
            set(_ultracan_link \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/UltraCAN\")
            file(REMOVE \"\${_ultracan_link}\")
            # RELATIVE target on purpose: an absolute one would point at the
            # staging directory CPack builds the package in, not at where the
            # package is eventually unpacked.
            file(CREATE_LINK \"../${ULTRACAN_PRIVATE_DIR}/bin/UltraCAN\"
                             \"\${_ultracan_link}\" SYMBOLIC)
            message(STATUS \"Symlinking: \${_ultracan_link} -> ../${ULTRACAN_PRIVATE_DIR}/bin/UltraCAN\")
        ")
    endif()
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

# =============================================================================
#                        LINUX PACKAGING  (CPack -> .deb)
# =============================================================================
# THIS BLOCK MUST STAY LAST IN THIS FILE. include(CPack) snapshots every CPACK_*
# variable at the moment it is called, so anything set after that line is
# ignored - silently. The symptom is "my setting had no effect", never an error,
# which is why the rule is written down rather than assumed.
#
# Windows keeps Inno Setup (the CreateInstaller target above); CPACK_GENERATOR
# is a global, so setting it to DEB unconditionally would also hand `cpack` a
# Debian generator on a Windows build host.
if(UNIX AND NOT APPLE)

    # --- where the package puts its files ------------------------------------
    # CPack DEB builds its payload by replaying this project's install() rules
    # under CPACK_PACKAGING_INSTALL_PREFIX - NOT under CMAKE_INSTALL_PREFIX.
    # Left at CMake's /usr/local default it would produce a .deb that writes
    # into /usr/local, which Debian policy reserves for the local administrator
    # and which no distribution package may touch. /usr is the only correct
    # answer for a packaged application.
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

    set(CPACK_GENERATOR "DEB")

    # --- identity ------------------------------------------------------------
    # Debian package names are lowercase, digits, '+', '-' and '.' only. The
    # CMake project is "UltraCAN"; the PACKAGE is "ultracan". They are allowed
    # to differ and here they must: dpkg rejects the capitals outright.
    # The installed BINARY keeps its OUTPUT_NAME UltraCAN, which is what
    # Exec=UltraCAN in ultracan.desktop resolves to on $PATH.
    set(CPACK_PACKAGE_NAME "ultracan")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_VENDOR "Basri Arzuoglu")
    set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/basriiarzuoglu/UltraCAN")

    # Required by dpkg, and it must carry a mail address in RFC822 form.
    # Without it CPack substitutes a placeholder that lintian flags.
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER
        "Hasan Basri Arzuoglu <basri.arzuoglu@robeff.com>")

    # --- description ---------------------------------------------------------
    # Debian's Description field is one synopsis line followed by an indented
    # extended description. CPack assembles exactly that: SUMMARY becomes the
    # synopsis, and every line below is indented for us (blank lines become the
    # " ." paragraph separator). So these lines are written FLUSH LEFT on
    # purpose - a line that already starts with a space is treated as verbatim
    # text and will not be reflowed by the package viewers.
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "CAN bus analyzer, trace logger and DBC signal plotter")
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
"UltraCAN attaches to a CAN interface - SocketCAN can0/vcan0 on Linux - and
shows what is actually on the bus: a live trace, a raw bit and byte heatmap
for buses nobody has a database for, DBC-decoded signal plots, a transmit
sender with cyclic frames, background log recording with replay, and a
UDS/ISO-TP diagnostics tester.
.
The bitrate of a SocketCAN interface belongs to the kernel and is set with
ip(8), not from inside the application.")

    # --- archive metadata ----------------------------------------------------
    # DEB-DEFAULT is what produces the conventional ultracan_1.0.0_amd64.deb
    # rather than CPack's own ultracan-1.0.0-Linux.deb spelling, which some
    # repository tooling will not parse.
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
    set(CPACK_DEBIAN_PACKAGE_SECTION "electronics")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    # Architecture is left unset on purpose: CPack asks
    # `dpkg --print-architecture`, so a cross build and an arm64 host both get
    # the right answer without a second place to keep in sync.

    # Everything a bus analyzer session wants next to it: candump, cansend and
    # `ip link set can0 type can bitrate ...`. A Suggests, not a Depends - the
    # application is perfectly usable without them.
    set(CPACK_DEBIAN_PACKAGE_SUGGESTS "can-utils")

    # Debian expects shipped binaries stripped; the debug info belongs in a
    # separate -dbgsym package, not in the one users install.
    set(CPACK_STRIP_FILES TRUE)

    # Requested: force 0644/0755 on the control files rather than inheriting
    # whatever umask the build ran under. A control file that arrives as 0600
    # makes dpkg refuse the package on some systems.
    set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

    # No postinst/postrm here on purpose. desktop-file-utils and
    # hicolor-icon-theme both register dpkg TRIGGERS on /usr/share/applications
    # and /usr/share/icons/hicolor, so the menu database and the icon cache are
    # refreshed by dpkg itself the moment this package lands. Hand-written
    # update-desktop-database calls would duplicate that and then have to be
    # kept correct through every dpkg change.

    # --- automatic shared-library dependencies -------------------------------
    # Runs dpkg-shlibdeps over every ELF file in the staged package - the
    # executable AND, in a bundled build, the Qt libraries and plugins that
    # travel with it - and turns each SONAME into the Debian package that
    # ships it. That is still exactly what a standalone build wants: Qt itself
    # needs libxcb, fontconfig, freetype, glib and a dozen others that come
    # from the distribution and must NOT be bundled, and this is what puts
    # them in Depends: without anyone maintaining the list by hand.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

    if(ULTRACAN_BUNDLING)
        # Point the tool at the BUNDLED library directory, spelled relative to
        # the package staging root because that is dpkg-shlibdeps' working
        # directory when CPack runs it, and the absolute path does not exist
        # until then.
        #
        # This resolves Qt's internal references (libQt6Quick.so.6 needing
        # libQt6Core.so.6) to files inside this very package. dpkg-shlibdeps
        # finds no owning package for them, --ignore-missing-info lets that
        # pass, and NO dependency is emitted - which is correct: the package
        # already ships them. Only the genuinely external libraries end up in
        # Depends:.
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "usr/${ULTRACAN_PRIVATE_DIR}/lib")
    else()
        # Unbundled: Qt may still not live in a system directory.
        # dpkg-shlibdeps resolves each NEEDED entry against the linker search
        # path first, and a Qt from the online installer is not on it - the
        # tool then fails outright with "cannot find library libQt6Quick.so.6"
        # and takes the whole package build down. Handing it the directory
        # with -l lets it find the file; whether it can then attribute that
        # file to a package is a separate question, answered below.
        if(DEFINED QT6_INSTALL_PREFIX AND DEFINED QT6_INSTALL_LIBS)
            set(ULTRACAN_QT_LIB_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBS}")
        else()
            get_target_property(_ultracan_qt_core_lib Qt6::Core LOCATION)
            get_filename_component(ULTRACAN_QT_LIB_DIR "${_ultracan_qt_core_lib}" DIRECTORY)
        endif()
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "${ULTRACAN_QT_LIB_DIR}")

        # --- is this Qt packageable at all? ----------------------------------
        # The honest limit of SHLIBDEPS: it can only name a dependency for a
        # library that some INSTALLED dpkg package owns. Build against the Qt
        # online installer with bundling off and dpkg-shlibdeps finds the .so,
        # finds no owner, and - because CPack passes --ignore-missing-info -
        # carries on and emits a .deb whose Depends: names libc6 and
        # libstdc++6 but not one Qt package. That .deb installs cleanly on a
        # machine with no Qt at all and then dies at startup on a missing
        # libQt6Quick.so.6, which is the worst possible way to find out.
        #
        # Not a concern with ULTRACAN_BUNDLE_QT on, which is why this check
        # lives in the else branch: there the package carries its own Qt.
        find_program(ULTRACAN_DPKG_QUERY dpkg-query)
        if(ULTRACAN_DPKG_QUERY AND EXISTS "${ULTRACAN_QT_LIB_DIR}/libQt6Core.so.6")
            execute_process(
                COMMAND "${ULTRACAN_DPKG_QUERY}" -S "${ULTRACAN_QT_LIB_DIR}/libQt6Core.so.6"
                RESULT_VARIABLE ULTRACAN_QT_IS_PACKAGED
                OUTPUT_QUIET ERROR_QUIET
            )
            if(NOT ULTRACAN_QT_IS_PACKAGED EQUAL 0)
                message(WARNING
                    "Qt at ${ULTRACAN_QT_LIB_DIR} is not owned by any installed "
                    ".deb, so dpkg-shlibdeps cannot turn it into a Depends: "
                    "entry. The .deb will build, and will then install onto "
                    "machines that cannot run it. Either turn "
                    "ULTRACAN_BUNDLE_QT back ON, or build against the "
                    "distribution's Qt 6 development packages, or state the "
                    "runtime requirement yourself with "
                    "-DULTRACAN_DEB_EXTRA_DEPENDS=\"libqt6quick6 (>= 6.8), ...\".")
            endif()
        endif()
    endif()

    # Hand-written Depends, merged with (not replaced by) whatever
    # dpkg-shlibdeps works out - CPack appends its result to this variable.
    set(ULTRACAN_DEB_EXTRA_DEPENDS "" CACHE STRING
        "Extra Debian Depends: entries, appended to the dpkg-shlibdeps result")
    if(ULTRACAN_DEB_EXTRA_DEPENDS)
        set(CPACK_DEBIAN_PACKAGE_DEPENDS "${ULTRACAN_DEB_EXTRA_DEPENDS}")
    endif()

    include(CPack)
endif()
