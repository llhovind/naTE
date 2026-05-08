cmake_minimum_required(VERSION 3.20)
project(naTE LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(wxWidgets REQUIRED COMPONENTS core base)
include(${wxWidgets_USE_FILE})

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

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
    src/transport/LoopbackTransport.cpp
    src/transport/PtyTransport.cpp
    src/parser/Parser.cpp
    src/ui/wxKeyAdapter.cpp
    src/ui/MainFrame.cpp
    src/ui/NewConnectionDialog.cpp
    src/ui/TerminalPanel.cpp
    src/ui/DocLayout.cpp
    src/ui/UIManager.cpp
    src/ui/SearchController.cpp
    src/ui/SearchBar.cpp
    src/ui/SelectionActionRegistry.cpp
    src/ui/SelectionActions.cpp
)

if(APPLE)
    add_executable(naTE MACOSX_BUNDLE ${SOURCES})
else()
    add_executable(naTE ${SOURCES})
endif()

target_link_libraries(naTE PRIVATE ${wxWidgets_LIBRARIES} ${GTK3_LIBRARIES})

if(UNIX AND NOT APPLE)
    target_link_libraries(naTE PRIVATE util)
endif()
target_include_directories(naTE PRIVATE src ${GTK3_INCLUDE_DIRS})

enable_testing()
add_subdirectory(tests)

add_custom_command(TARGET naTE POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/config.ini $<TARGET_FILE_DIR:naTE>/config.ini
    COMMENT "Copying config.ini"
)
