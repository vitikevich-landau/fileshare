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

# --- Core library (cross-platform: MSVC + GCC) ------------------------------
add_library(fileshare_core
    src/crc32.cpp
    src/checksum.cpp
    src/protocol.cpp
    src/config.cpp)
target_include_directories(fileshare_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fileshare_core
    PUBLIC  nlohmann_json::nlohmann_json
    PRIVATE fileshare_warnings fileshare_sanitizers)

# Server/client targets arrive in M1 (blocking) and M4 (epoll, Linux-only).

# --- Tests ------------------------------------------------------------------
if(FILESHARE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
