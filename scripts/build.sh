#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$SERVER_DIR/build"

echo "=== Whisper Stream Server Build ==="
echo "Server dir: $SERVER_DIR"
echo "Build dir: $BUILD_DIR"

# Check for whisper.cpp
WHISPER_DIR="$SERVER_DIR/../whisper.cpp"
if [ ! -d "$WHISPER_DIR" ]; then
    echo "Error: whisper.cpp not found at $WHISPER_DIR"
    echo "Please clone whisper.cpp to /Users/saint/Dev/whisper.cpp"
    exit 1
fi

echo "Using whisper.cpp at: $WHISPER_DIR"

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with CMake
echo ""
echo "=== Configuring with CMake ==="
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DWHISPER_METAL=ON \
    -DWHISPER_CPP_DIR="$WHISPER_DIR"

# Build
echo ""
echo "=== Building ==="
cmake --build . -j$(sysctl -n hw.ncpu)

echo ""
echo "=== Build Complete ==="
echo "Binary: $BUILD_DIR/whisper-stream-server"
