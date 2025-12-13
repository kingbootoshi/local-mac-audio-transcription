# Whisper Stream Server Architecture

This document explains the design decisions, threading model, and component interactions in the whisper-stream-server.

## System Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              BROWSER CLIENT                                  │
│  ┌─────────────────┐    ┌──────────────────┐    ┌────────────────────────┐  │
│  │  AudioCapture   │───▶│  WhisperClient   │───▶│    UI (index.html)     │  │
│  │  (mic → PCM)    │    │  (WebSocket)     │    │  partial/final display │  │
│  └─────────────────┘    └──────────────────┘    └────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ WebSocket (ws://localhost:9090)
                                    │ Binary: 16-bit PCM audio
                                    │ JSON: transcription results
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                              C++ SERVER                                      │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                        MAIN THREAD                                    │   │
│  │  ┌─────────────────────────────────────────────────────────────────┐ │   │
│  │  │                    uWebSockets Event Loop                        │ │   │
│  │  │                                                                  │ │   │
│  │  │  onOpen()   → createSession() (no context yet)                  │ │   │
│  │  │  onMessage() → onAudioReceived() → push to AudioBuffer          │ │   │
│  │  │  onClose()  → destroySession() → release context if held        │ │   │
│  │  └─────────────────────────────────────────────────────────────────┘ │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                      INFERENCE THREAD                                 │   │
│  │  ┌─────────────────────────────────────────────────────────────────┐ │   │
│  │  │                     inferenceLoop()                              │ │   │
│  │  │                                                                  │ │   │
│  │  │  Every 500ms:                                                    │ │   │
│  │  │    for each active session:                                      │ │   │
│  │  │      1. Get audio from AudioBuffer                               │ │   │
│  │  │      2. Build sliding window [old + new]                         │ │   │
│  │  │      3. Run whisper_full() inference                             │ │   │
│  │  │      4. Extract text from segments                               │ │   │
│  │  │      5. Send partial/final via WebSocket                         │ │   │
│  │  └─────────────────────────────────────────────────────────────────┘ │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                        CONTEXT POOL                                   │   │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────────┐     │   │
│  │  │  Slot 0    │  │  Slot 1    │  │  Slot 2    │  │  Slot N    │     │   │
│  │  │ whisper_ctx│  │ whisper_ctx│  │ whisper_ctx│  │ whisper_ctx│     │   │
│  │  │ in_use: T  │  │ in_use: F  │  │ in_use: T  │  │ in_use: F  │     │   │
│  │  └────────────┘  └────────────┘  └────────────┘  └────────────┘     │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                         SESSIONS MAP                                  │   │
│  │  session_id → Session { AudioBuffer, context_slot, send_message }    │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Why This Architecture?

### Problem: whisper.cpp is NOT Thread-Safe

The whisper.cpp library maintains internal state in `whisper_context`. You cannot:
- Use the same context from multiple threads simultaneously
- Run inference on one context while another thread reads from it

### Solution: Context Leasing

We pre-load N independent whisper contexts at startup, but lease them on-demand:

```cpp
// Each slot has its own whisper_context
struct ContextSlot {
    whisper_context* ctx = nullptr;  // Independent whisper instance
    bool in_use = false;             // Protected by mutex
    int slot_id = 0;
};

// Pool of contexts
std::vector<std::unique_ptr<ContextSlot>> context_pool_;
```

**Context lifecycle (leasing model):**
1. Client connects → session created with `context_slot = nullptr` (no context yet)
2. VAD detects speech → `acquireContext()` leases a context for this utterance
3. Inference runs while user is speaking → partials emitted
4. VAD detects silence → final emitted → `releaseContext()` returns context to pool
5. Client can speak again → lease a new context (may be different slot)

**WAITING_FOR_CONTEXT state:** If all contexts are busy when speech starts:
- Session enters `WAITING_FOR_CONTEXT` state
- Audio continues buffering (up to 30 seconds)
- When a context becomes available, session transitions to `SPEAKING`
- If user stops speaking before getting a context, catch-up inference runs when context is available

**Benefits:**
- Unlimited idle connections (only active speakers use contexts)
- Memory scales with concurrent speakers, not total connections
- No connection rejection - clients just wait for context availability

**Memory Trade-off**: Each context loads the full model (~388 MB for base.en). With 4 contexts, you need ~1.5 GB RAM. Configure `--contexts` based on your expected **concurrent speakers** (not total connections).

## Threading Model

### Thread 1: Main Thread (uWebSockets Event Loop)

Handles all WebSocket I/O:
- Accepts new connections
- Receives binary audio frames
- Sends JSON transcription results
- Single-threaded, non-blocking, event-driven

```cpp
// main.cpp
app.ws<PerSocketData>("/*", {
    .open = [&](auto* ws) {
        // Create session, acquire context
    },
    .message = [&](auto* ws, std::string_view message, uWS::OpCode op) {
        // Push audio to buffer (fast, non-blocking)
    },
    .close = [&](auto* ws, int, std::string_view) {
        // Destroy session, release context
    }
}).listen(port, ...);
```

### Thread 2: Inference Thread

Runs the inference loop on a fixed interval:

```cpp
// whisper_server.cpp
void WhisperServer::inferenceLoop() {
    while (running_) {
        auto start = steady_clock::now();

        // Get all sessions with enough audio
        for (auto& session : active_sessions) {
            runInference(session);  // Blocking whisper_full() call
        }

        // Sleep to maintain step interval (500ms default)
        auto elapsed = steady_clock::now() - start;
        sleep_for(step_ms - elapsed);
    }
}
```

**Why a single inference thread?**
- Simplicity: One thread processes sessions sequentially
- GPU contention: Metal GPU is shared; parallel inference may not help
- Predictable latency: Each session gets processed every `step_ms`

For higher throughput, you could use thread-per-context, but that adds complexity.

## Audio Pipeline

### 1. Browser Capture (AudioCapture.ts)

```
Microphone (48kHz) → Resample (16kHz) → Float32 → Int16 → WebSocket
```

- Uses Web Audio API's ScriptProcessorNode
- Resamples from browser's native rate (usually 44.1/48kHz) to whisper's 16kHz
- Converts Float32 [-1, 1] to Int16 [-32768, 32767]
- Sends chunks every ~100ms (4096 samples at 48kHz ≈ 85ms)

### 2. Server Buffer (AudioBuffer)

Thread-safe ring buffer that:
- Receives Int16 PCM from WebSocket
- Converts to Float32 for whisper
- Accumulates until inference loop reads it

```cpp
class AudioBuffer {
    std::vector<float> buffer_;
    std::mutex mutex_;

    void push(const int16_t* data, size_t len);  // From WebSocket
    std::vector<float> getAll();                  // For inference
};
```

### 3. Sliding Window (runInference)

Whisper needs context to transcribe accurately. We use a sliding window:

```
Time →
├── keep_ms ──┼─────────── length_ms ───────────┤
│  (overlap)  │         (context window)         │
└─────────────┴──────────────────────────────────┘

Window N:   [====][========================]
Window N+1:      [====][========================]
                  ↑
            Overlap prevents cutting words
```

```cpp
// Build sliding window: [keep from old] + [new audio]
std::vector<float> pcmf32;
pcmf32.resize(n_samples_take + pcmf32_new.size());

// Copy tail of old audio (overlap)
memcpy(pcmf32.data(), old_tail, n_samples_take * sizeof(float));

// Copy new audio
memcpy(pcmf32.data() + n_samples_take, pcmf32_new.data(), ...);

// Save for next iteration
session->pcmf32_old = pcmf32;
```

## Key whisper.cpp APIs Used

### Context Initialization

```cpp
#include "whisper.h"

// Configure context
whisper_context_params cparams = whisper_context_default_params();
cparams.use_gpu = true;      // Enable Metal
cparams.flash_attn = true;   // Flash attention optimization

// Load model
whisper_context* ctx = whisper_init_from_file_with_params(
    "models/ggml-base.en.bin", cparams
);
```

### Running Inference

```cpp
// Configure inference
whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
wparams.print_progress = false;
wparams.print_realtime = false;
wparams.single_segment = true;   // One segment output
wparams.max_tokens = 0;          // No limit
wparams.language = "en";
wparams.n_threads = 4;           // CPU threads
wparams.no_context = true;       // Don't use previous context
wparams.no_timestamps = true;    // We don't need timestamps

// Run inference
int result = whisper_full(ctx, wparams, pcmf32.data(), pcmf32.size());

// Extract results
int n_segments = whisper_full_n_segments(ctx);
for (int i = 0; i < n_segments; ++i) {
    const char* text = whisper_full_get_segment_text(ctx, i);
    // Send to client
}
```

### Cleanup

```cpp
whisper_free(ctx);  // Called in destructor
```

## Message Flow

```
Client                          Server
  │                               │
  │──── Connect (WS) ────────────▶│
  │                               │ createSession() [no context yet]
  │◀─── {"type":"ready"} ─────────│
  │                               │
  │──── Binary PCM (speech) ─────▶│
  │                               │ AudioBuffer.push()
  │                               │ VAD detects speech
  │                               │ acquireContext() → lease context
  │──── Binary PCM ──────────────▶│
  │                               │
  │                               │ [Inference thread wakes]
  │                               │ runInference()
  │◀─── {"type":"partial"} ───────│
  │                               │
  │──── Binary PCM ──────────────▶│
  │                               │ [Next inference cycle]
  │◀─── {"type":"partial"} ───────│
  │                               │
  │──── Binary PCM (silence) ────▶│
  │                               │ VAD detects silence
  │                               │ emitFinal()
  │◀─── {"type":"final"} ─────────│
  │                               │ releaseContext() → return to pool
  │                               │
  │──── Binary PCM (speech) ─────▶│ [User speaks again]
  │                               │ acquireContext() → may get different slot
  │                               │ ...
  │                               │
  │──── Close ───────────────────▶│
  │                               │ destroySession()
  │                               │ releaseContext() if held
  │                               │
```

## Performance Characteristics

### Latency Breakdown

| Stage | Time |
|-------|------|
| Audio capture + send | ~10ms |
| Buffer accumulation | 0-500ms (depends on step_ms) |
| Whisper inference | 100-300ms (base.en on M1) |
| WebSocket send | ~1ms |
| **Total** | **~300-800ms** |

### Tuning Parameters

| Parameter | Effect |
|-----------|--------|
| `step_ms` ↓ | Lower latency, higher CPU |
| `step_ms` ↑ | Higher latency, lower CPU |
| `length_ms` ↓ | Less context, faster, less accurate |
| `length_ms` ↑ | More context, slower, more accurate |
| `contexts` ↑ | More concurrent users, more memory |

### Memory Usage

```
Base memory:        ~50 MB (server binary + runtime)
Per context:        ~388 MB (base.en model)
Per session buffer: ~2 MB (30 seconds of audio)

2 contexts:  ~850 MB total
4 contexts:  ~1.6 GB total
8 contexts:  ~3.2 GB total
```

## Future Improvements

1. **Thread-per-context**: Run inference in parallel for multiple sessions
2. **SSL/TLS**: Native TLS support or reverse proxy
3. **Streaming segments**: Send tokens as they're decoded (requires whisper.cpp callback)
4. **Language detection**: Auto-detect language for multilingual models
5. **Priority queuing**: Prefer recently-active sessions when allocating contexts
