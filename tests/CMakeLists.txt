include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    FIND_PACKAGE_ARGS NAMES GTest)
# Keep MSVC from mixing static/dynamic CRT between gtest and our targets.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(fileshare_tests
    test_crc32.cpp
    test_protocol.cpp
    test_config.cpp
    test_cli.cpp
    test_integration.cpp
    test_concurrency.cpp)
target_link_libraries(fileshare_tests PRIVATE
    fileshare_core
    fileshare_server_lib
    fileshare_client_lib
    GTest::gtest_main
    fileshare_sanitizers)

gtest_discover_tests(fileshare_tests)
