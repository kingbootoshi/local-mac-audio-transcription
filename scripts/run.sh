#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$SERVER_DIR/build"
BINARY="$BUILD_DIR/whisper-stream-server"

# Default model paths (whisper.cpp is a sibling directory to whisper-server)
DEFAULT_MODEL="$SERVER_DIR/../whisper.cpp/models/ggml-base.en.bin"
DEFAULT_VAD_MODEL="$SERVER_DIR/../whisper.cpp/models/ggml-silero-v6.2.0.bin"

if [ ! -f "$BINARY" ]; then
    echo "Error: Server binary not found at $BINARY"
    echo "Run ./scripts/build.sh first"
    exit 1
fi

# Check for whisper model
MODEL="${MODEL:-$DEFAULT_MODEL}"
if [ ! -f "$MODEL" ]; then
    echo "Error: Model not found at $MODEL"
    echo "Download with: cd ../whisper.cpp && ./models/download-ggml-model.sh base.en"
    exit 1
fi

# Check for VAD model (optional)
VAD_MODEL="${VAD_MODEL:-$DEFAULT_VAD_MODEL}"
VAD_ARGS=""
if [ -f "$VAD_MODEL" ]; then
    echo "VAD model found: $VAD_MODEL"
    VAD_ARGS="--vad-model $VAD_MODEL --vad-threshold ${VAD_THRESHOLD:-0.5} --vad-silence ${VAD_SILENCE:-1000}"
else
    echo "VAD model not found at $VAD_MODEL"
    echo "To enable VAD, download with: cd ../whisper.cpp && ./models/download-vad-model.sh silero-v6.2.0"
    echo "Starting without VAD..."
fi

echo ""
echo "Starting whisper-stream-server..."
echo "Model: $MODEL"
if [ -n "$VAD_ARGS" ]; then
    echo "VAD: enabled (threshold=${VAD_THRESHOLD:-0.5}, silence=${VAD_SILENCE:-1000}ms)"
else
    echo "VAD: disabled"
fi
echo ""

exec "$BINARY" \
    --model "$MODEL" \
    --port "${PORT:-9090}" \
    --contexts "${CONTEXTS:-2}" \
    --threads "${THREADS:-4}" \
    $VAD_ARGS \
    "$@"
