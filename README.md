# Local Mac Audio Transcription

Real-time speech-to-text transcription server using [whisper.cpp](https://github.com/ggerganov/whisper.cpp) with WebSocket streaming. Optimized for Apple Silicon with Metal GPU acceleration.

## Features

- **Real-time streaming transcription** via WebSocket
- **Voice Activity Detection (VAD)** - automatically detects speech end and emits final transcripts
- **Metal GPU acceleration** on Apple Silicon (M1/M2/M3/M4)
- **Multi-client support** with context pooling (configurable parallel sessions)
- **Low latency** (~300-500ms inference on M-series chips)
- **24/7 operation** - designed for always-on deployments via launchd

## Quick Start

### Prerequisites

- macOS with Apple Silicon (M1/M2/M3/M4)
- [whisper.cpp](https://github.com/ggerganov/whisper.cpp) cloned as sibling directory
- CMake 3.14+
- Xcode Command Line Tools

```bash
# Install build tools
brew install cmake
xcode-select --install

# Clone whisper.cpp (required)
cd /path/to/your/dev
git clone https://github.com/ggerganov/whisper.cpp.git

# Clone this repo
git clone https://github.com/kingbootoshi/local-mac-audio-transcription.git
cd local-mac-audio-transcription

# Download a whisper model
cd ../whisper.cpp
./models/download-ggml-model.sh base.en
cd -
```

### Build

```bash
./scripts/build.sh
```

### Run

```bash
./scripts/run.sh
```

Or with custom options:
```bash
./build/whisper-stream-server \
  --model ../whisper.cpp/models/ggml-base.en.bin \
  --port 9090 \
  --contexts 2
```

### Test with Browser Client

```bash
cd examples/web-client
npm install
npm run dev
```

Open http://localhost:5173, click **Connect**, then **Start Recording**.

## Project Structure

```
local-mac-audio-transcription/
├── src/                        # C++ server source
│   ├── main.cpp               # Entry point, WebSocket handlers
│   ├── whisper_server.cpp     # Core transcription + VAD logic
│   ├── whisper_server.hpp
│   ├── audio_buffer.cpp       # Thread-safe audio buffer
│   ├── audio_buffer.hpp
│   └── json.hpp               # nlohmann/json (auto-downloaded)
│
├── scripts/
│   ├── build.sh               # Build script
│   └── run.sh                 # Run script
│
├── examples/                   # Example clients
│   └── web-client/            # TypeScript browser client
│
├── install/                    # Mac deployment
│   ├── install.sh             # Install as launchd service
│   ├── uninstall.sh
│   └── com.whisper.stream-server.plist
│
├── docs/
│   ├── ARCHITECTURE.md        # Server design deep-dive
│   ├── API.md                 # WebSocket protocol reference
│   └── CPP_GUIDE.md           # C++ learning guide
│
├── CMakeLists.txt
└── README.md
```

## Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `--model` | `models/ggml-base.en.bin` | Path to whisper model |
| `--port` | `9090` | WebSocket server port |
| `--contexts` | `2` | Number of parallel transcription contexts |
| `--threads` | `4` | CPU threads per inference |
| `--step` | `500` | Inference interval (ms) |
| `--length` | `5000` | Audio context window (ms) |
| `--keep` | `200` | Overlap between windows (ms) |
| `--vad-threshold` | `0.6` | Voice activity detection threshold |
| `--silence-ms` | `1000` | Silence duration to trigger final (ms) |
| `--language` | `en` | Language code |
| `--no-gpu` | - | Disable Metal GPU |

## WebSocket Protocol

### Client → Server
- **Binary frames**: 16-bit signed PCM audio at 16kHz mono

### Server → Client
```json
{ "type": "ready", "model": "base.en", "contexts": 2 }
{ "type": "partial", "text": "Hello how are" }
{ "type": "final", "text": "Hello, how are you?" }
{ "type": "error", "message": "..." }
```

## Mac Mini Deployment (24/7)

```bash
cd install
sudo ./install.sh --port 9090 --contexts 4
```

See `install/` for launchd service management.

## Memory Usage

| Contexts | Model | RAM |
|----------|-------|-----|
| 2 | base.en | ~850 MB |
| 4 | base.en | ~1.6 GB |
| 2 | small.en | ~1.8 GB |
| 4 | small.en | ~3.5 GB |

## Documentation

- [ARCHITECTURE.md](docs/ARCHITECTURE.md) - Threading, context pooling, VAD
- [API.md](docs/API.md) - WebSocket protocol, client examples
- [CPP_GUIDE.md](docs/CPP_GUIDE.md) - C++ learning guide

## License

MIT
