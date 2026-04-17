cmake_minimum_required(VERSION 3.20)
project(naTE LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(wxWidgets REQUIRED COMPONENTS core base)
include(${wxWidgets_USE_FILE})

set(SOURCES
    src/main.cpp
    src/App.cpp
    src/MainFrame.cpp
    src/TerminalPanel.cpp
    src/Buffer.cpp
    src/Config.cpp
)

if(APPLE)
    add_executable(naTE MACOSX_BUNDLE ${SOURCES})
else()
    add_executable(naTE ${SOURCES})
endif()

target_link_libraries(naTE PRIVATE ${wxWidgets_LIBRARIES})
target_include_directories(naTE PRIVATE src)

enable_testing()
add_subdirectory(tests)

add_custom_command(TARGET naTE POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/config.ini $<TARGET_FILE_DIR:naTE>/config.ini
    COMMENT "Copying config.ini"
)
