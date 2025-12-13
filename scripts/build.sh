#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$SERVER_DIR/build"

echo "=== Whisper Stream Server Build ==="
echo "Server dir: $SERVER_DIR"
echo "Build dir: $BUILD_DIR"

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with CMake (whisper.cpp is fetched automatically via FetchContent)
echo ""
echo "=== Configuring with CMake ==="
echo "Note: First build downloads whisper.cpp (~100MB), please wait..."
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DWHISPER_METAL=ON

# Build
echo ""
echo "=== Building ==="
cmake --build . -j$(sysctl -n hw.ncpu)

echo ""
echo "=== Build Complete ==="
echo "Binary: $BUILD_DIR/whisper-stream-server"
