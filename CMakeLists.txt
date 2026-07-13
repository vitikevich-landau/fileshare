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

# --- Server / client libraries ----------------------------------------------
# The M1 server is portable (blocking / thread-per-connection). From M4 the
# epoll reactor is added Linux-only; the client stays cross-platform.
add_library(fileshare_server_lib src/server.cpp src/server_core.cpp src/client_registry.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(fileshare_server_lib PRIVATE src/epoll_server.cpp)  # epoll reactor (M4)
    target_compile_definitions(fileshare_server_lib PUBLIC FILESHARE_HAVE_EPOLL)
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
