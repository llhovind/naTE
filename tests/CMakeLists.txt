include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
)
FetchContent_MakeAvailable(Catch2)

add_executable(naTE_tests
    main.cpp
    test_config.cpp
    ${CMAKE_SOURCE_DIR}/src/config/Config.cpp
)

target_link_libraries(naTE_tests PRIVATE
    Catch2::Catch2
    ${wxWidgets_LIBRARIES}
)
target_include_directories(naTE_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

include(CTest)
include(Catch)
catch_discover_tests(naTE_tests)
