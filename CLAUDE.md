# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Real-time speech-to-text transcription server using whisper.cpp with WebSocket streaming. Optimized for Apple Silicon with Metal GPU acceleration.

## Build & Run Commands

```bash
# Build the C++ server (requires whisper.cpp cloned at ../whisper.cpp)
./scripts/build.sh

# Run the server (downloads model if needed)
./scripts/run.sh

# Run with custom options
./build/whisper-stream-server --model ../whisper.cpp/models/ggml-base.en.bin --port 9090 --contexts 2

# Run the browser test client
cd examples/web-client && npm install && npm run dev
# Opens http://localhost:5173
```

## Architecture

**Two-thread model:**
1. **Main thread**: uWebSockets event loop handles WebSocket I/O (connect/disconnect, audio receive, JSON send)
2. **Inference thread**: Runs `inferenceLoop()` every `step_ms` (default 500ms), processes all active sessions sequentially

**Thread-safe message passing**: The inference thread never calls `ws->send()` directly (uWebSockets is not thread-safe). Instead:
- Inference thread enqueues messages to `Session::outgoing_messages` (mutex-protected)
- Inference thread calls `notifySessionHasMessages()` which schedules a flush via `uWS::Loop::defer()`
- Event loop thread runs `flushSessionMessagesOnEventLoop()` to drain and send messages
- Messages are delivered immediately (event-driven), not dependent on incoming audio

**Context pooling**: whisper.cpp is NOT thread-safe. The server pre-loads N independent `whisper_context` instances at startup. Each WebSocket connection acquires a context slot; when all slots are busy, new connections are rejected.

**Audio pipeline**:
- Browser: Mic (48kHz) → Resample (16kHz) → Float32 → Int16 → WebSocket binary frames
- Server: Int16 → Float32 → AudioBuffer (thread-safe ring buffer) → Sliding window inference

**Sliding window inference**: Each inference uses `[keep_ms overlap from previous] + [new audio]` to maintain context continuity across chunks.

## Key Source Files

| File | Purpose |
|------|---------|
| `src/main.cpp` | Entry point, CLI parsing, uWebSockets handlers |
| `src/whisper_server.hpp/cpp` | WhisperServer class: context pool, session management, inference loop |
| `src/audio_buffer.hpp/cpp` | Thread-safe ring buffer, int16→float32 conversion |
| `examples/web-client/src/WhisperClient.ts` | Browser WebSocket client |
| `examples/web-client/src/AudioCapture.ts` | Browser mic capture with resampling |

## WebSocket Protocol

**Client → Server**: Binary frames with 16-bit signed PCM audio at 16kHz mono

**Server → Client**: JSON text frames:
```json
{ "type": "ready", "model": "...", "contexts": 2 }
{ "type": "partial", "text": "..." }
{ "type": "final", "text": "..." }
{ "type": "error", "message": "..." }
```

**VAD behavior**: When VAD is enabled (`--vad-model`), the server emits `final` after silence exceeds `--vad-silence` (default 1000ms). Without VAD, only `partial` messages are emitted.

## Dependencies

- **whisper.cpp**: Must be cloned at `../whisper.cpp` (sibling directory)
- **uWebSockets/uSockets**: Fetched automatically by CMake
- **nlohmann/json**: Single header, downloaded by CMake to `src/json.hpp`

## CLI Options

| Option | Default | Description |
|--------|---------|-------------|
| `--model` | `models/ggml-base.en.bin` | Path to whisper model |
| `--port` | `9090` | WebSocket server port |
| `--contexts` | `2` | Number of parallel contexts (~388MB each for base.en) |
| `--threads` | `4` | CPU threads per inference |
| `--step` | `500` | Inference interval (ms) |
| `--length` | `5000` | Audio context window (ms) |
| `--keep` | `200` | Overlap between windows (ms) |
| `--no-gpu` | - | Disable Metal GPU |
| `--vad-model` | - | Path to VAD model (enables VAD) |
| `--vad-threshold` | `0.5` | Speech probability threshold (0.0-1.0) |
| `--vad-silence` | `1000` | Silence duration to trigger final (ms) |

## Memory Usage

- 2 contexts with base.en: ~850 MB
- 4 contexts with base.en: ~1.6 GB
- Per session buffer: ~2 MB (30 seconds of audio)

## Known Limitations

1. Single inference thread processes sessions sequentially (not in parallel)
2. Context-per-connection model (idle connections still hold contexts)
3. No TLS - use reverse proxy for production
4. No authentication - any client can connect

## Testing

```bash
# Run all tests
./scripts/run_tests.sh

# Run only unit tests (no model required)
./scripts/run_tests.sh unit

# Run only E2E tests (requires running server)
./scripts/run_tests.sh e2e
```

See `docs/TESTING.md` for full test documentation.
