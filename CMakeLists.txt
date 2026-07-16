cmake_minimum_required(VERSION 3.24)   # 3.24 for FetchContent FIND_PACKAGE_ARGS
project(fileshare LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(FILESHARE_BUILD_TESTS  "Build unit tests"        ON)
option(FILESHARE_ENABLE_ASAN  "Enable AddressSanitizer" OFF)
option(FILESHARE_ENABLE_TSAN  "Enable ThreadSanitizer"  OFF)
option(FILESHARE_USE_SHA256   "Use SHA-256 (OpenSSL) instead of CRC32 for file checksums" OFF)
option(FILESHARE_USE_COROUTINES "Use the C++20 coroutine epoll server (Linux)" OFF)
option(FILESHARE_BUILD_TUI    "Build the FTXUI commander client" ON)

# --- Warnings (zero-warnings policy, §8) ------------------------------------
add_library(fileshare_warnings INTERFACE)
if(MSVC)
    target_compile_options(fileshare_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(fileshare_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

# --- Sanitizers -------------------------------------------------------------
add_library(fileshare_sanitizers INTERFACE)
if(FILESHARE_ENABLE_ASAN AND FILESHARE_ENABLE_TSAN)
    message(FATAL_ERROR "ASan and TSan cannot be enabled at the same time")
endif()
if(FILESHARE_ENABLE_ASAN)
    target_compile_options(fileshare_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(fileshare_sanitizers INTERFACE -fsanitize=address)
endif()
if(FILESHARE_ENABLE_TSAN)
    target_compile_options(fileshare_sanitizers INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(fileshare_sanitizers INTERFACE -fsanitize=thread)
endif()

# --- Dependencies -----------------------------------------------------------
include(FetchContent)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    FIND_PACKAGE_ARGS NAMES nlohmann_json)
FetchContent_MakeAvailable(nlohmann_json)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# --- Core library (cross-platform: MSVC + GCC) ------------------------------
add_library(fileshare_core
    src/crc32.cpp
    src/checksum.cpp
    src/protocol.cpp
    src/config.cpp
    src/net.cpp
    src/cli.cpp
    src/thread_pool.cpp)
target_include_directories(fileshare_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fileshare_core
    PUBLIC  nlohmann_json::nlohmann_json Threads::Threads
    PRIVATE fileshare_warnings fileshare_sanitizers)
if(WIN32)
    target_link_libraries(fileshare_core PUBLIC ws2_32)   # Winsock
endif()
if(FILESHARE_USE_SHA256)
    find_package(OpenSSL REQUIRED)
    target_sources(fileshare_core PRIVATE src/sha256.cpp)
    target_compile_definitions(fileshare_core PUBLIC FILESHARE_USE_SHA256)
    target_link_libraries(fileshare_core PUBLIC OpenSSL::Crypto)
endif()

# --- v2 core library (protocol v2 + wire transport) -------------------------
# Self-contained fileshare::v2 module reusing the v1 wire primitives and net
# shim. Kept in its own library so v1 and its tests stay untouched.
add_library(fileshare_v2_core
    src/v2/protocol.cpp
    src/v2/wire.cpp
    src/v2/vfs.cpp
    src/v2/crypto.cpp
    src/v2/auth.cpp
    src/v2/settings.cpp
    src/v2/session.cpp
    src/v2/dispatch.cpp
    src/v2/log.cpp
    src/v2/fs_watcher.cpp
    src/v2/server_context.cpp
    src/v2/server.cpp
    src/v2/client.cpp)
target_link_libraries(fileshare_v2_core
    PUBLIC  fileshare_core
    PRIVATE fileshare_warnings fileshare_sanitizers)

# v2 daemon executable (Linux/macOS/Windows: thread-per-connection).
add_executable(fileshare_daemon_app src/v2/daemon_main.cpp)
target_link_libraries(fileshare_daemon_app
    PRIVATE fileshare_v2_core fileshare_warnings fileshare_sanitizers)
set_target_properties(fileshare_daemon_app PROPERTIES OUTPUT_NAME fileshare-daemon)

# --- TUI commander client ---------------------------------------------------
# tui_core is UI logic + the connection worker (no FTXUI, unit-testable). The
# FTXUI view lives only in the executable.
add_library(fileshare_tui_core
    src/v2/tui/model.cpp
    src/v2/tui/connection.cpp)
target_link_libraries(fileshare_tui_core
    PUBLIC  fileshare_v2_core
    PRIVATE fileshare_warnings fileshare_sanitizers)

if(FILESHARE_BUILD_TUI)
    FetchContent_Declare(ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        GIT_TAG        v5.0.0
        FIND_PACKAGE_ARGS NAMES ftxui)
    set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(ftxui)

    # The FTXUI view as a library, so a headless render test can link it too.
    add_library(fileshare_tui_view src/v2/tui/view.cpp)
    target_link_libraries(fileshare_tui_view
        PUBLIC  fileshare_tui_core ftxui::screen ftxui::dom
        PRIVATE fileshare_warnings fileshare_sanitizers)

    add_executable(fileshare_commander_app src/v2/commander_main.cpp)
    target_link_libraries(fileshare_commander_app
        PRIVATE fileshare_tui_view ftxui::component
                fileshare_warnings fileshare_sanitizers)
    set_target_properties(fileshare_commander_app PROPERTIES OUTPUT_NAME fileshare-commander)
endif()

# --- Server / client libraries ----------------------------------------------
# The M1 server is portable (blocking / thread-per-connection). From M4 the
# epoll reactor is added Linux-only; the client stays cross-platform.
add_library(fileshare_server_lib src/server.cpp src/server_core.cpp src/client_registry.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(fileshare_server_lib PRIVATE src/epoll_server.cpp)  # epoll reactor (M4)
    target_compile_definitions(fileshare_server_lib PUBLIC FILESHARE_HAVE_EPOLL)
    if(FILESHARE_USE_COROUTINES)
        target_sources(fileshare_server_lib PRIVATE src/epoll_coro_server.cpp)  # coroutine reactor (M6)
        target_compile_definitions(fileshare_server_lib PUBLIC FILESHARE_USE_COROUTINES)
    endif()
endif()
target_link_libraries(fileshare_server_lib
    PUBLIC  fileshare_core
    PRIVATE fileshare_warnings fileshare_sanitizers)

add_library(fileshare_client_lib src/client.cpp)
target_link_libraries(fileshare_client_lib
    PUBLIC  fileshare_core
    PRIVATE fileshare_warnings fileshare_sanitizers)

# --- Executables ------------------------------------------------------------
add_executable(fileshare_server_app src/server_main.cpp)
target_link_libraries(fileshare_server_app
    PRIVATE fileshare_server_lib fileshare_warnings fileshare_sanitizers)
set_target_properties(fileshare_server_app PROPERTIES OUTPUT_NAME fileshare_server)

add_executable(fileshare_client_app src/client_main.cpp)
target_link_libraries(fileshare_client_app
    PRIVATE fileshare_client_lib fileshare_warnings fileshare_sanitizers)
set_target_properties(fileshare_client_app PROPERTIES OUTPUT_NAME fileshare_client)

# Load-test client (M3): drives N concurrent downloads to measure throughput.
add_executable(fileshare_loadtest src/loadtest_main.cpp)
target_link_libraries(fileshare_loadtest
    PRIVATE fileshare_core fileshare_warnings fileshare_sanitizers)

# --- Tests ------------------------------------------------------------------
if(FILESHARE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
