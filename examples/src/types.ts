// Message types from server
export interface ReadyMessage {
  type: 'ready';
  model: string;
  contexts: number;
}

export interface PartialMessage {
  type: 'partial';
  text: string;
}

export interface FinalMessage {
  type: 'final';
  text: string;
}

export interface ErrorMessage {
  type: 'error';
  message: string;
}

export type ServerMessage = ReadyMessage | PartialMessage | FinalMessage | ErrorMessage;

// Client options
export interface WhisperClientOptions {
  url: string;
  onReady?: (message: ReadyMessage) => void;
  onPartial?: (text: string) => void;
  onFinal?: (text: string) => void;
  onError?: (error: Error) => void;
  onClose?: () => void;
  onConnecting?: () => void;
}

// Audio capture options
export interface AudioCaptureOptions {
  sampleRate?: number;      // Target sample rate (default: 16000)
  chunkDurationMs?: number; // How often to send chunks (default: 100ms)
  onAudioChunk?: (chunk: Int16Array) => void;
  onError?: (error: Error) => void;
}
