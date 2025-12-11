cmake_minimum_required(VERSION 3.14)
project(whisper-stream-server VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(WHISPER_METAL "Enable Metal GPU acceleration" ON)
option(WHISPER_COREML "Enable CoreML acceleration" OFF)

# Paths
set(WHISPER_CPP_DIR "${CMAKE_SOURCE_DIR}/../whisper.cpp" CACHE PATH "Path to whisper.cpp")

# Build whisper.cpp as a subdirectory
add_subdirectory(${WHISPER_CPP_DIR} ${CMAKE_BINARY_DIR}/whisper.cpp)

# Fetch uWebSockets and its dependencies
include(FetchContent)

FetchContent_Declare(
    uSockets
    GIT_REPOSITORY https://github.com/uNetworking/uSockets.git
    GIT_TAG v0.8.8
)

FetchContent_Declare(
    uWebSockets
    GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
    GIT_TAG v20.67.0
)

FetchContent_MakeAvailable(uSockets)

# Build uSockets as a static library
file(GLOB USOCKETS_SOURCES
    "${usockets_SOURCE_DIR}/src/*.c"
    "${usockets_SOURCE_DIR}/src/eventing/*.c"
    "${usockets_SOURCE_DIR}/src/crypto/*.c"
)

add_library(uSockets STATIC ${USOCKETS_SOURCES})
target_include_directories(uSockets PUBLIC ${usockets_SOURCE_DIR}/src)
target_compile_definitions(uSockets PRIVATE LIBUS_NO_SSL)

# Get uWebSockets headers
FetchContent_MakeAvailable(uWebSockets)

# Download nlohmann/json header
set(JSON_HEADER "${CMAKE_SOURCE_DIR}/src/json.hpp")
if(NOT EXISTS ${JSON_HEADER})
    file(DOWNLOAD
        "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
        ${JSON_HEADER}
        SHOW_PROGRESS
    )
endif()

# Main executable
add_executable(whisper-stream-server
    src/main.cpp
    src/audio_buffer.cpp
    src/whisper_server.cpp
)

target_include_directories(whisper-stream-server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${uwebsockets_SOURCE_DIR}/src
    ${usockets_SOURCE_DIR}/src
    ${WHISPER_CPP_DIR}/include
    ${WHISPER_CPP_DIR}/ggml/include
)

# Find zlib (required by uWebSockets for compression)
find_package(ZLIB REQUIRED)

target_link_libraries(whisper-stream-server PRIVATE
    whisper
    uSockets
    ZLIB::ZLIB
)

# Platform-specific settings
if(APPLE)
    target_link_libraries(whisper-stream-server PRIVATE
        "-framework Foundation"
        "-framework Accelerate"
    )
    if(WHISPER_METAL)
        target_link_libraries(whisper-stream-server PRIVATE
            "-framework Metal"
            "-framework MetalKit"
        )
    endif()
endif()

# Install
install(TARGETS whisper-stream-server DESTINATION bin)
