cmake_minimum_required(VERSION 3.20)

# Set policy CMP0135 to use DOWNLOAD_EXTRACT_TIMESTAMP for FetchContent
# This avoids warnings and ensures reproducible builds
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(alasia VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2)
endif()

# Enable warnings as errors for better code quality
option(WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
if(WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
endif()

# ─── Dependencies ──────────────────────────────────────────────────────────────

# libcurl (system)
find_package(CURL REQUIRED)

# OpenSSL (system)
find_package(OpenSSL REQUIRED)

# nlohmann/json (header-only, FetchContent)
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

# argparse (header-only, FetchContent)
# Using specific commit hash for reproducible builds
FetchContent_Declare(
    argparse
    GIT_REPOSITORY https://github.com/p-ranav/argparse.git
    GIT_TAG        v3.1
    GIT_SHALLOW    FALSE  # Full history for better reproducibility
)
FetchContent_MakeAvailable(argparse)

# ─── Sources ───────────────────────────────────────────────────────────────────

set(SOURCES
    src/main.cpp
    src/log.cpp
    src/config.cpp
    src/cache.cpp
    src/ip_getter.cpp
    src/curl_pool.cpp
    src/http_client.cpp
    src/provider/cloudflare.cpp
    src/provider/aliyun.cpp
)

# Platform-specific sources
if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    # FreeBSD uses ioctl for IPv6 address retrieval
    list(APPEND SOURCES src/ip_getter_bsd.cpp)
endif()

add_executable(alasia ${SOURCES})

target_include_directories(alasia PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(alasia PRIVATE
    CURL::libcurl
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    argparse
    pthread
)

# ─── Version info via compile definitions ──────────────────────────────────────
# Can be overridden with: cmake -DAPP_VERSION=v1.0.0 -DAPP_COMMIT=abc1234 ...
set(APP_VERSION "dev" CACHE STRING "Application version string")
set(APP_COMMIT  ""    CACHE STRING "Git commit hash")
set(APP_BUILD_DATE "" CACHE STRING "Build date (UTC ISO 8601)")
set(APP_COMPILER "" CACHE STRING "Compiler version string")

target_compile_definitions(alasia PRIVATE
    APP_VERSION="${APP_VERSION}"
    APP_COMMIT="${APP_COMMIT}"
    APP_BUILD_DATE="${APP_BUILD_DATE}"
    APP_COMPILER="${APP_COMPILER}"
)

# ─── Install ───────────────────────────────────────────────────────────────────
install(TARGETS alasia DESTINATION bin)
