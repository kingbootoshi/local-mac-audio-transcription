cmake_minimum_required(VERSION 3.14)
project(whisper-stream-server VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(WHISPER_METAL "Enable Metal GPU acceleration" ON)
option(WHISPER_COREML "Enable CoreML acceleration" OFF)

# Fetch whisper.cpp automatically
include(FetchContent)

FetchContent_Declare(
    whisper
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG        v1.8.2
)

# Don't build whisper examples/tests
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(whisper)

# Fetch uWebSockets and its dependencies

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
    ${whisper_SOURCE_DIR}/include
    ${whisper_SOURCE_DIR}/ggml/include
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

# ============================================
# Testing
# ============================================
option(BUILD_TESTS "Build tests" OFF)

if(BUILD_TESTS)
    # Fetch Catch2
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.0
    )
    FetchContent_MakeAvailable(Catch2)

    enable_testing()

    # Unit tests - AudioBuffer
    add_executable(test_audio_buffer tests/unit/test_audio_buffer.cpp src/audio_buffer.cpp)
    target_link_libraries(test_audio_buffer PRIVATE Catch2::Catch2WithMain)
    target_include_directories(test_audio_buffer PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME AudioBuffer COMMAND test_audio_buffer)

    # Unit tests - VAD State Machine
    add_executable(test_vad_state_machine tests/unit/test_vad_state_machine.cpp)
    target_link_libraries(test_vad_state_machine PRIVATE Catch2::Catch2WithMain)
    target_include_directories(test_vad_state_machine PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME VADStateMachine COMMAND test_vad_state_machine)

    # Integration tests - requires whisper model
    add_executable(test_transcription
        tests/integration/test_transcription.cpp
        src/audio_buffer.cpp
        src/whisper_server.cpp
    )
    target_link_libraries(test_transcription PRIVATE
        Catch2::Catch2WithMain
        whisper
        uSockets
        ZLIB::ZLIB
    )
    target_include_directories(test_transcription PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${uwebsockets_SOURCE_DIR}/src
        ${usockets_SOURCE_DIR}/src
        ${whisper_SOURCE_DIR}/include
        ${whisper_SOURCE_DIR}/ggml/include
    )
    if(APPLE)
        target_link_libraries(test_transcription PRIVATE
            "-framework Foundation"
            "-framework Accelerate"
        )
        if(WHISPER_METAL)
            target_link_libraries(test_transcription PRIVATE
                "-framework Metal"
                "-framework MetalKit"
            )
        endif()
    endif()
    add_test(NAME Transcription COMMAND test_transcription)
endif()
