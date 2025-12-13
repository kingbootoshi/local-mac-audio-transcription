# Whisper Stream Server API Reference

This document describes the WebSocket protocol for communicating with the whisper-stream-server.

## Connection

### Endpoint

```
ws://localhost:9090
```

Or with a custom path (server accepts any path):
```
ws://localhost:9090/transcribe
ws://localhost:9090/api/v1/stream
```

### Example Connection (JavaScript)

```javascript
const ws = new WebSocket('ws://localhost:9090');

ws.onopen = () => {
  console.log('Connected');
};

ws.onmessage = (event) => {
  const msg = JSON.parse(event.data);
  console.log('Received:', msg);
};

ws.onclose = () => {
  console.log('Disconnected');
};
```

## Message Protocol

### Direction: Server → Client

All server messages are JSON text frames.

#### Ready Message

Sent immediately after connection when a context is successfully acquired.

```json
{
  "type": "ready",
  "model": "models/ggml-base.en.bin",
  "contexts": 2
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Always `"ready"` |
| `model` | string | Path to loaded model |
| `contexts` | number | Total contexts in pool |

#### Partial Message

Sent during speech with the current transcription. Updates frequently as more audio is processed.

```json
{
  "type": "partial",
  "text": "Hello how are you"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Always `"partial"` |
| `text` | string | Current transcription (may change) |

**Note**: Partials replace each other. Only display the most recent partial.

#### Final Message

Sent when VAD detects speech has ended (silence threshold exceeded). Contains the complete transcription for the utterance.

```json
{
  "type": "final",
  "text": "Hello, how are you doing today?"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Always `"final"` |
| `text` | string | Finalized transcription |

**VAD Behavior**: When VAD is enabled, the server tracks speech state:
- `IDLE` → `SPEAKING`: Speech detected above threshold
- `SPEAKING` → `ENDING`: Silence exceeds `--vad-silence` duration (default: 1000ms)
- `ENDING` → emits `final` message and returns to `IDLE`

Finals are only emitted when VAD is enabled (`--vad-model` specified).

#### Error Message

Sent when an error occurs.

```json
{
  "type": "error",
  "message": "No available contexts"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Always `"error"` |
| `message` | string | Human-readable error description |

### Direction: Client → Server

Audio data is sent as binary WebSocket frames.

#### Audio Format

| Property | Value |
|----------|-------|
| Encoding | 16-bit signed integer (little-endian) |
| Sample Rate | 16000 Hz |
| Channels | 1 (mono) |
| Byte Order | Little-endian |

#### Chunk Size

Recommended: 100-200ms of audio per frame
- 100ms = 1600 samples = 3200 bytes
- 200ms = 3200 samples = 6400 bytes

Smaller chunks = lower latency, more overhead
Larger chunks = higher latency, less overhead

#### Example: Sending Audio (JavaScript)

```javascript
// Assuming you have Int16Array audio data
const audioChunk = new Int16Array(1600); // 100ms at 16kHz

// Send as binary
ws.send(audioChunk.buffer);
```

## Client Implementation Guide

### 1. Basic Client Structure

```typescript
interface ServerMessage {
  type: 'ready' | 'partial' | 'final' | 'error';
  text?: string;
  message?: string;
  model?: string;
  contexts?: number;
}

class WhisperClient {
  private ws: WebSocket | null = null;

  connect(url: string): Promise<void> {
    return new Promise((resolve, reject) => {
      this.ws = new WebSocket(url);

      this.ws.onopen = () => {
        // Don't resolve yet - wait for 'ready' message
      };

      this.ws.onmessage = (event) => {
        const msg: ServerMessage = JSON.parse(event.data);

        switch (msg.type) {
          case 'ready':
            resolve();
            break;
          case 'partial':
            this.onPartial(msg.text!);
            break;
          case 'final':
            this.onFinal(msg.text!);
            break;
          case 'error':
            this.onError(new Error(msg.message));
            break;
        }
      };

      this.ws.onerror = () => reject(new Error('Connection failed'));
      this.ws.onclose = () => this.onClose();
    });
  }

  sendAudio(pcm: Int16Array): void {
    if (this.ws?.readyState === WebSocket.OPEN) {
      this.ws.send(pcm.buffer);
    }
  }

  disconnect(): void {
    this.ws?.close();
  }

  // Override these
  onPartial(text: string): void {}
  onFinal(text: string): void {}
  onError(err: Error): void {}
  onClose(): void {}
}
```

### 2. Audio Capture (Browser)

```typescript
async function captureAudio(onChunk: (pcm: Int16Array) => void) {
  const stream = await navigator.mediaDevices.getUserMedia({
    audio: {
      channelCount: 1,
      sampleRate: { ideal: 16000 },
      echoCancellation: true,
      noiseSuppression: true,
    }
  });

  const audioContext = new AudioContext({ sampleRate: 16000 });
  const source = audioContext.createMediaStreamSource(stream);
  const processor = audioContext.createScriptProcessor(4096, 1, 1);

  processor.onaudioprocess = (event) => {
    const float32 = event.inputBuffer.getChannelData(0);
    const int16 = float32ToInt16(float32);
    onChunk(int16);
  };

  source.connect(processor);
  processor.connect(audioContext.destination);

  return () => {
    stream.getTracks().forEach(t => t.stop());
    audioContext.close();
  };
}

function float32ToInt16(float32: Float32Array): Int16Array {
  const int16 = new Int16Array(float32.length);
  for (let i = 0; i < float32.length; i++) {
    const s = Math.max(-1, Math.min(1, float32[i]));
    int16[i] = s < 0 ? s * 0x8000 : s * 0x7FFF;
  }
  return int16;
}
```

### 3. Resampling (if browser rate != 16kHz)

```typescript
function resample(
  input: Float32Array,
  fromRate: number,
  toRate: number
): Float32Array {
  if (fromRate === toRate) return input;

  const ratio = fromRate / toRate;
  const outputLength = Math.round(input.length / ratio);
  const output = new Float32Array(outputLength);

  for (let i = 0; i < outputLength; i++) {
    const srcIndex = i * ratio;
    const srcFloor = Math.floor(srcIndex);
    const srcCeil = Math.min(srcFloor + 1, input.length - 1);
    const t = srcIndex - srcFloor;

    // Linear interpolation
    output[i] = input[srcFloor] * (1 - t) + input[srcCeil] * t;
  }

  return output;
}
```

## Error Handling

### Connection Errors

| Scenario | Behavior |
|----------|----------|
| Server not running | `onerror` fires, connection fails |
| All contexts busy | `ready` not sent, then error message |
| Invalid audio format | Server ignores, no response |

### Reconnection Strategy

```typescript
async function connectWithRetry(url: string, maxRetries = 5) {
  for (let i = 0; i < maxRetries; i++) {
    try {
      await client.connect(url);
      return;
    } catch (err) {
      console.log(`Attempt ${i + 1} failed, retrying...`);
      await sleep(1000 * Math.pow(2, i)); // Exponential backoff
    }
  }
  throw new Error('Failed to connect after retries');
}
```

## Rate Limiting

The server does not implement rate limiting. For production:
- Limit connections per IP
- Limit audio data rate per connection
- Use a reverse proxy (nginx) for rate limiting

## Security Considerations

### Current State

- **No authentication**: Any client can connect
- **No encryption**: Plain WebSocket (ws://)
- **No input validation**: Server trusts audio format

### Production Recommendations

1. **Use TLS**: Deploy behind nginx/Caddy with SSL
   ```
   wss://your-domain.com/transcribe
   ```

2. **Add authentication**: Token in URL or first message
   ```javascript
   const ws = new WebSocket('wss://host/transcribe?token=xxx');
   ```

3. **Rate limit**: nginx `limit_conn` and `limit_req`

4. **Validate origin**: Check `Origin` header in production

## Protocol Versioning

The current protocol is version 1 (implicit). Current features:
- Real-time partial transcriptions
- VAD-gated final transcriptions (when VAD model loaded)
- Event-driven message delivery (messages flush immediately, not dependent on incoming audio)

Future versions may add:

```json
{
  "type": "ready",
  "version": 2,
  "model": "...",
  "features": ["vad", "timestamps", "speaker-id"]
}
```

## Testing with wscat

```bash
# Install
npm install -g wscat

# Connect
wscat -c ws://localhost:9090

# You'll receive the ready message
< {"type":"ready","model":"models/ggml-base.en.bin","contexts":2}

# Send audio (not practical via wscat, use browser client)
```

## cURL Testing (HTTP upgrade)

```bash
# Check if server is listening (will fail upgrade but confirms port)
curl -v --include \
  --header "Connection: Upgrade" \
  --header "Upgrade: websocket" \
  --header "Sec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==" \
  --header "Sec-WebSocket-Version: 13" \
  http://localhost:9090/
```
