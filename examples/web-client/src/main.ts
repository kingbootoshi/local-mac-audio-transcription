import { WhisperClient } from './WhisperClient';
import { AudioCapture } from './AudioCapture';

// DOM Elements
const connectBtn = document.getElementById('connectBtn') as HTMLButtonElement;
const startBtn = document.getElementById('startBtn') as HTMLButtonElement;
const stopBtn = document.getElementById('stopBtn') as HTMLButtonElement;
const clearBtn = document.getElementById('clearBtn') as HTMLButtonElement;
const statusEl = document.getElementById('status') as HTMLSpanElement;
const partialEl = document.getElementById('partial') as HTMLDivElement;
const transcriptEl = document.getElementById('transcript') as HTMLDivElement;
const serverUrlEl = document.getElementById('serverUrl') as HTMLInputElement;
const authTokenEl = document.getElementById('authToken') as HTMLInputElement;

// State
let client: WhisperClient | null = null;
let audioCapture: AudioCapture | null = null;

// UI Helpers
function setStatus(status: string, color: string = '#666') {
  statusEl.textContent = status;
  statusEl.style.color = color;
}

function updateButtons() {
  const connected = client?.isConnected() ?? false;
  const capturing = audioCapture?.isActive() ?? false;

  connectBtn.disabled = connected;
  startBtn.disabled = !connected || capturing;
  stopBtn.disabled = !capturing;
  clearBtn.disabled = false;
}

function appendTranscript(text: string) {
  const p = document.createElement('p');
  p.textContent = text;
  p.style.margin = '8px 0';
  p.style.padding = '8px';
  p.style.background = '#f5f5f5';
  p.style.borderRadius = '4px';
  transcriptEl.appendChild(p);
  transcriptEl.scrollTop = transcriptEl.scrollHeight;
}

// Event Handlers
async function handleConnect() {
  let url = serverUrlEl.value.trim();
  if (!url) {
    setStatus('Please enter server URL', '#c00');
    return;
  }

  // Append auth token if provided
  const token = authTokenEl.value.trim();
  if (token) {
    const separator = url.includes('?') ? '&' : '?';
    url = `${url}${separator}token=${encodeURIComponent(token)}`;
  }

  setStatus('Connecting...', '#f90');

  client = new WhisperClient({
    url,
    onConnecting: () => {
      setStatus('Connecting...', '#f90');
    },
    onReady: (msg) => {
      setStatus(`Connected (${msg.contexts} contexts available)`, '#0a0');
      updateButtons();
    },
    onPartial: (text) => {
      partialEl.textContent = text;
      partialEl.style.opacity = '0.7';
    },
    onFinal: (text) => {
      partialEl.textContent = '';
      appendTranscript(text);
    },
    onError: (err) => {
      setStatus(`Error: ${err.message}`, '#c00');
      console.error('Client error:', err);
    },
    onClose: () => {
      setStatus('Disconnected', '#666');
      handleStop();
      updateButtons();
    },
  });

  try {
    await client.connect();
  } catch (err) {
    setStatus('Connection failed', '#c00');
    client = null;
  }

  updateButtons();
}

async function handleStart() {
  if (!client?.isConnected()) {
    setStatus('Not connected', '#c00');
    return;
  }

  audioCapture = new AudioCapture({
    sampleRate: 16000,
    chunkDurationMs: 100,
    onAudioChunk: (chunk) => {
      client?.sendAudio(chunk);
    },
    onError: (err) => {
      setStatus(`Audio error: ${err.message}`, '#c00');
      console.error('Audio error:', err);
    },
  });

  try {
    await audioCapture.start();
    setStatus('Recording...', '#c00');
    updateButtons();
  } catch (err) {
    setStatus('Failed to start microphone', '#c00');
    audioCapture = null;
  }
}

function handleStop() {
  if (audioCapture) {
    audioCapture.stop();
    audioCapture = null;
  }
  partialEl.textContent = '';
  if (client?.isConnected()) {
    setStatus('Connected (stopped)', '#0a0');
  }
  updateButtons();
}

function handleClear() {
  transcriptEl.innerHTML = '';
  partialEl.textContent = '';
}

function handleDisconnect() {
  handleStop();
  if (client) {
    client.disconnect();
    client = null;
  }
  setStatus('Disconnected', '#666');
  updateButtons();
}

// Initialize
connectBtn.addEventListener('click', handleConnect);
startBtn.addEventListener('click', handleStart);
stopBtn.addEventListener('click', handleStop);
clearBtn.addEventListener('click', handleClear);

// Keyboard shortcut: Space to toggle recording
document.addEventListener('keydown', (e) => {
  if (e.code === 'Space' && e.target === document.body) {
    e.preventDefault();
    if (audioCapture?.isActive()) {
      handleStop();
    } else if (client?.isConnected()) {
      handleStart();
    }
  }
});

// Initial state
updateButtons();
setStatus('Disconnected', '#666');

console.log('[WhisperClient] Ready. Press Space to toggle recording after connecting.');
