cmake_minimum_required(VERSION 3.20)
project(naTE LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(wxWidgets REQUIRED COMPONENTS core base)
include(${wxWidgets_USE_FILE})

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(LIBSSH2 REQUIRED libssh2)

include(FetchContent)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3)
FetchContent_MakeAvailable(nlohmann_json)

set(SOURCES
    src/main.cpp
    src/app/App.cpp
    src/input/InputRouter.cpp
    src/config/Config.cpp
    src/document/Document.cpp
    src/session/Connection.cpp
    src/session/InputEncoder.cpp
    src/session/Session.cpp
    src/session/SessionManager.cpp
    src/ui/ConnectionFactory.cpp
    src/transport/EnvUtils.cpp
    src/transport/LoopbackTransport.cpp
    src/transport/PtyTransport.cpp
    src/transport/SerialTransport.cpp
    src/transport/SshTransport.cpp
    src/parser/Parser.cpp
    src/ui/wxKeyAdapter.cpp
    src/ui/MainFrame.cpp
    src/ui/NewConnectionDialog.cpp
    src/ui/ConnectionManagerDialog.cpp
    src/ui/FileTransferDialog.cpp
    src/ui/RemoteFileBrowserDialog.cpp
    src/ui/GeometryDialog.cpp
    src/ui/TerminalPanel.cpp
    src/ui/TerminalTile.cpp
    src/ui/WrapControl.cpp
    src/ui/TabStrip.cpp
    src/ui/TerminalGrid.cpp
    src/ui/DocLayout.cpp
    src/ui/UIManager.cpp
    src/ui/SearchController.cpp
    src/ui/SearchBar.cpp
    src/ui/SelectionActionRegistry.cpp
    src/ui/SelectionActions.cpp
    src/db/JsonConnectionRepository.cpp
    src/db/ConnectionStore.cpp
)

if(APPLE)
    add_executable(naTE MACOSX_BUNDLE ${SOURCES})
else()
    add_executable(naTE ${SOURCES})
endif()

target_link_libraries(naTE PRIVATE ${wxWidgets_LIBRARIES} ${GTK3_LIBRARIES} ${LIBSSH2_LIBRARIES} nlohmann_json::nlohmann_json)

if(UNIX AND NOT APPLE)
    target_link_libraries(naTE PRIVATE util)
endif()
target_include_directories(naTE PRIVATE src ${GTK3_INCLUDE_DIRS} ${LIBSSH2_INCLUDE_DIRS})

enable_testing()
add_subdirectory(tests)

add_custom_command(TARGET naTE POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/config.ini $<TARGET_FILE_DIR:naTE>/config.ini
    COMMENT "Copying config.ini"
)
