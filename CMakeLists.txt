cmake_minimum_required(VERSION 3.28)

# Derive version from the nearest annotated or lightweight git tag (e.g. v1.2.3).
# Fallback to 0.0.0 when building outside a git repo or before the first tag.
execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE _GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
string(REGEX REPLACE "^v" "" _GIT_VERSION "${_GIT_TAG}")
if(NOT _GIT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
    set(_GIT_VERSION "0.0.0")
endif()

project(naTE VERSION "${_GIT_VERSION}" LANGUAGES CXX)

include(GNUInstallDirs)

execute_process(
    COMMAND git rev-list --count HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_BUILD_NUMBER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_BUILD_NUMBER)
    set(GIT_BUILD_NUMBER "0")
endif()
if(NOT GIT_COMMIT_HASH)
    set(GIT_COMMIT_HASH "unknown")
endif()

configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CheckIPOSupported)
check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_output)
if(NOT _ipo_supported)
    message(STATUS "IPO/LTO not supported by this toolchain: ${_ipo_output}")
endif()

option(NATE_SYSTEM_DEPS "Use system-installed libssh2 and nlohmann_json instead of FetchContent" OFF)

include(FetchContent)
FetchContent_Declare(wxWidgets
    GIT_REPOSITORY https://github.com/wxWidgets/wxWidgets.git
    GIT_TAG        v3.2.6
    GIT_SHALLOW    TRUE
)
# wxBUILD_INSTALL OFF keeps wxWidgets' own install rules (wx-config, headers,
# static libs) out of our package staging — we ship only the naTE binary and
# its assets, not the build-time toolchain of a bundled dependency.
set(wxBUILD_INSTALL   OFF CACHE BOOL "" FORCE)
set(wxBUILD_SHARED    OFF CACHE BOOL "" FORCE)
set(wxBUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(wxBUILD_DEMOS     OFF CACHE BOOL "" FORCE)
set(wxBUILD_SAMPLES   OFF CACHE BOOL "" FORCE)
set(wxUSE_OPENGL      OFF CACHE BOOL "" FORCE)
set(wxUSE_WEBVIEW     OFF CACHE BOOL "" FORCE)
set(wxUSE_MEDIACTRL   OFF CACHE BOOL "" FORCE)
set(wxUSE_RICHTEXT    OFF CACHE BOOL "" FORCE)
set(wxUSE_AUI         OFF CACHE BOOL "" FORCE)
set(wxUSE_HTML        OFF CACHE BOOL "" FORCE)
set(wxUSE_XRC         OFF CACHE BOOL "" FORCE)
set(wxUSE_STC         OFF CACHE BOOL "" FORCE)
set(wxUSE_HELP        OFF CACHE BOOL "" FORCE)
set(wxUSE_WXHTML_HELP OFF CACHE BOOL "" FORCE)
set(wxUSE_LIBJPEG     OFF CACHE STRING "" FORCE)
set(wxUSE_LIBTIFF     OFF CACHE STRING "" FORCE)
FetchContent_MakeAvailable(wxWidgets)

# GTK3 — Linux only, always system-provided.
find_package(PkgConfig)
if(UNIX AND NOT APPLE)
    if(NOT PkgConfig_FOUND)
        message(FATAL_ERROR "PkgConfig is required on Linux")
    endif()
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
endif()

if(NATE_SYSTEM_DEPS)
    # System libssh2
    if(UNIX AND NOT APPLE)
        pkg_check_modules(LIBSSH2 REQUIRED libssh2)
    else()
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBSSH2 libssh2)
        endif()
        if(NOT LIBSSH2_FOUND)
            find_library(LIBSSH2_LIB  NAMES ssh2  REQUIRED)
            find_path   (LIBSSH2_INC  NAMES libssh2.h REQUIRED)
            set(LIBSSH2_LIBRARIES    "${LIBSSH2_LIB}")
            set(LIBSSH2_INCLUDE_DIRS "${LIBSSH2_INC}")
        endif()
    endif()
    set(NATE_LIBSSH2_LINK    "${LIBSSH2_LIBRARIES}")
    set(NATE_LIBSSH2_INCLUDE "${LIBSSH2_INCLUDE_DIRS}")

    # System nlohmann_json
    find_package(nlohmann_json REQUIRED)
else()
    # Bundled libssh2 1.11.1 — required for SSH agent forwarding support.
    # libssh2 1.10.x silently drops inbound auth-agent@openssh.com channels;
    # 1.11.0 added packet_authagent_open() and LIBSSH2_CALLBACK_AUTHAGENT.
    set(BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING     OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # Compile zlib support into libssh2 so the SshDesc.compress option can
    # negotiate zlib/zlib@openssh.com — without this the flag is a no-op even
    # though the algorithm is selected at handshake time.
    set(ENABLE_ZLIB_COMPRESSION ON CACHE BOOL "" FORCE)
    # EXCLUDE_FROM_ALL keeps libssh2's unconditional install rules (static lib,
    # headers, pkg-config, CMake export) out of our package — libssh2 offers no
    # install opt-out option, so this is the only clean suppression mechanism.
    FetchContent_Declare(libssh2
        GIT_REPOSITORY https://github.com/libssh2/libssh2.git
        GIT_TAG        libssh2-1.11.1
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(libssh2)
    set(NATE_LIBSSH2_LINK    "libssh2")
    set(NATE_LIBSSH2_INCLUDE "")

    # Bundled nlohmann_json
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

set(SOURCES
    src/main.cpp
    src/app/App.cpp
    src/input/InputRouter.cpp
    src/config/Config.cpp
    src/config/ColorScheme.cpp
    src/document/Document.cpp
    src/session/Connection.cpp
    src/session/InputEncoder.cpp
    src/session/Session.cpp
    src/session/SessionManager.cpp
    src/ui/ConnectionFactory.cpp
    src/transport/EnvUtils.cpp
    src/transport/BastionTunnel.cpp
    src/transport/SshConfig.cpp
    src/transport/SshSession.cpp
    src/transport/SshSftpService.cpp
    src/transport/SshPortForwardEngine.cpp
    src/transport/SshPublicKey.cpp
    src/transport/X11Utils.cpp
    src/transport/LoopbackTransport.cpp
    src/transport/PtyTransport.cpp
    src/transport/SerialTransport.cpp
    src/transport/SshAuthenticator.cpp
    src/transport/SshChannelProxy.cpp
    src/transport/SshTransport.cpp
    src/parser/Parser.cpp
    src/ui/wxKeyAdapter.cpp
    src/ui/MainFrame.cpp
    src/ui/NewConnectionDialog.cpp
    src/ui/ConnectionManagerDialog.cpp
    src/ui/TransferFilesDialog.cpp
    src/ui/KbdIntDialog.cpp
    src/ui/PasteConfirmDialog.cpp
    src/ui/RemoteFileBrowserDialog.cpp
    src/ui/RemoteEditSession.cpp
    src/ui/RemoteEditManager.cpp
    src/ui/WorkspaceManagerDialog.cpp
    src/ui/GeometryDialog.cpp
    src/ui/PreferencesDialog.cpp
    src/ui/TerminalPanel.cpp
    src/ui/TerminalTile.cpp
    src/ui/TileIndicatorControl.cpp
    src/ui/WrapControl.cpp
    src/ui/AltScrControl.cpp
    src/ui/X11Control.cpp
    src/ui/PortForwardControl.cpp
    src/ui/AddPortForwardDialog.cpp
    src/ui/PortForwardPanel.cpp
    src/ui/DragGhost.cpp
    src/ui/TabStrip.cpp
    src/ui/TerminalGrid.cpp
    src/layout/DocLayout.cpp
    src/layout/UrlScanner.cpp
    src/layout/WordSelector.cpp
    src/ui/UIManager.cpp
    src/ui/SearchController.cpp
    src/ui/SearchBar.cpp
    src/ui/ReconnectBar.cpp
    src/ui/SelectionActionRegistry.cpp
    src/ui/SelectionActions.cpp
    src/db/JsonConnectionRepository.cpp
    src/db/JsonSessionRestoreRepository.cpp
    src/db/JsonNamedWorkspaceRepository.cpp
    src/db/ConnectionStore.cpp
    src/db/JsonScrollbackRepository.cpp
    src/db/ScrollbackWriter.cpp
    src/db/ScrollbackPurge.cpp
    src/ui/AboutDialog.cpp
)

# ---------------------------------------------------------------------------
# Icon regeneration — opt-in only, never runs during a normal build or CI.
# Run manually:  cmake --build build --target regen-icons
# Then commit the updated AppIcon.h and nate_*.png files.
# ---------------------------------------------------------------------------
add_custom_target(regen-icons
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/gen-icon-header.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Regenerating embedded app icon from packaging/nate.svg"
)

if(APPLE)
    add_executable(naTE MACOSX_BUNDLE ${SOURCES})
    set_target_properties(naTE PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/packaging/Info.plist.in"
    )
    # Include the .icns in the bundle's Resources/ if it has been generated.
    # To generate: cmake --build build --target regen-icns  (macOS only)
    if(EXISTS "${CMAKE_SOURCE_DIR}/packaging/naTE.icns")
        target_sources(naTE PRIVATE "${CMAKE_SOURCE_DIR}/packaging/naTE.icns")
        set_source_files_properties("${CMAKE_SOURCE_DIR}/packaging/naTE.icns"
            PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
        )
    endif()
else()
    add_executable(naTE ${SOURCES})
endif()

# ---------------------------------------------------------------------------
# .icns regeneration — opt-in only, macOS only, never runs during CI.
# Uses committed PNGs + macOS built-in sips/iconutil; no extra dependencies.
# Run manually:  cmake --build build --target regen-icns
# Then commit:   git add packaging/naTE.icns
# ---------------------------------------------------------------------------
if(APPLE)
    add_custom_target(regen-icns
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/gen-icns.sh"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Regenerating packaging/naTE.icns from packaging/nate.svg"
    )
endif()

# Apply LTO to naTE directly rather than via cmake's IPO mechanism.
# cmake 4.x + GCC generates thin LTO (-fno-fat-lto-objects) which breaks when
# linking against non-LTO C static archives (libssh2, wxWidgets) at the ltrans
# step.  Fat LTO embeds both GIMPLE IR and native code so the archive remains
# linkable even when the LTO pass cannot see all translation units.
if(_ipo_supported AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(naTE PRIVATE $<$<CONFIG:Release>:-flto=auto -ffat-lto-objects>)
    target_link_options   (naTE PRIVATE $<$<CONFIG:Release>:-flto=auto -ffat-lto-objects>)
endif()

target_link_libraries(naTE PRIVATE wx::core wx::base ${NATE_LIBSSH2_LINK} nlohmann_json::nlohmann_json)
if(UNIX AND NOT APPLE)
    target_link_libraries(naTE PRIVATE ${GTK3_LIBRARIES} util)
endif()
if(APPLE)
    target_link_libraries(naTE PRIVATE proc)
endif()

target_include_directories(naTE PRIVATE src ${CMAKE_CURRENT_BINARY_DIR} ${NATE_LIBSSH2_INCLUDE})
if(UNIX AND NOT APPLE)
    target_include_directories(naTE PRIVATE ${GTK3_INCLUDE_DIRS})
endif()

enable_testing()
add_subdirectory(tests)

add_custom_command(TARGET naTE POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/config.ini $<TARGET_FILE_DIR:naTE>/config.ini
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/themes $<TARGET_FILE_DIR:naTE>/themes
    COMMENT "Copying config.ini and themes/"
)

# ---------------------------------------------------------------------------
# Installation rules (AppImage pipeline and system-wide install)
# ---------------------------------------------------------------------------
install(TARGETS naTE
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(UNIX AND NOT APPLE)
    install(FILES packaging/naTE.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )
    install(FILES packaging/nate.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    )
    foreach(_size 16 32 48 128 256)
        install(FILES "src/ui/resources/nate_${_size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps"
            RENAME nate.png
        )
    endforeach()
    install(FILES packaging/io.github.lhovind.naTE.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo
    )
endif()

install(FILES config.ini
    DESTINATION ${CMAKE_INSTALL_DATADIR}/nate
)
install(DIRECTORY themes/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/nate/themes
)
install(DIRECTORY helpers/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/nate/helpers
)

# ---------------------------------------------------------------------------
# CPack packaging (distro starting point: cpack -G DEB / RPM / TGZ)
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME                "nate")
set(CPACK_PACKAGE_VENDOR              "Lawrence Hovind")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "SSH and serial terminal emulator")
set(CPACK_PACKAGE_VERSION             "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT             "lawrence.hovind@gmail.com")
set(CPACK_PACKAGING_INSTALL_PREFIX    "/usr")
if(EXISTS "${CMAKE_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_LICENSE   "${CMAKE_SOURCE_DIR}/LICENSE")
endif()
set(CPACK_STRIP_FILES                 TRUE)

# Runtime dependencies are derived automatically from the binary's DT_NEEDED
# entries rather than hand-maintained — this keeps the package metadata correct
# as link-time dependencies change (e.g. libz.so.1, pulled in by the bundled
# static libssh2's zlib compression code).  dpkg-shlibdeps (from dpkg-dev)
# powers the Debian path; rpmbuild's find-requires does the RPM path by default.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SECTION      "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY     "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE     "https://github.com/lhovind/naTE")

set(CPACK_RPM_PACKAGE_LICENSE         "MIT")
set(CPACK_RPM_PACKAGE_GROUP           "Applications/Internet")

include(CPack)
