#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$SERVER_DIR/build"
BINARY="$BUILD_DIR/whisper-stream-server"

# Default model path
DEFAULT_MODEL="$SERVER_DIR/../../whisper.cpp/models/ggml-base.en.bin"

if [ ! -f "$BINARY" ]; then
    echo "Error: Server binary not found at $BINARY"
    echo "Run ./scripts/build.sh first"
    exit 1
fi

# Check for model
MODEL="${MODEL:-$DEFAULT_MODEL}"
if [ ! -f "$MODEL" ]; then
    echo "Error: Model not found at $MODEL"
    echo "Download with: cd ../../whisper.cpp && ./models/download-ggml-model.sh base.en"
    exit 1
fi

echo "Starting whisper-stream-server..."
echo "Model: $MODEL"
echo ""

exec "$BINARY" \
    --model "$MODEL" \
    --port "${PORT:-9090}" \
    --contexts "${CONTEXTS:-2}" \
    --threads "${THREADS:-4}" \
    "$@"
