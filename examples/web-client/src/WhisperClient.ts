import type { WhisperClientOptions, ServerMessage } from './types';

export class WhisperClient {
  private ws: WebSocket | null = null;
  private options: WhisperClientOptions;
  private reconnectAttempts = 0;
  private maxReconnectAttempts = 3;

  constructor(options: WhisperClientOptions) {
    this.options = options;
  }

  async connect(): Promise<void> {
    return new Promise((resolve, reject) => {
      if (this.ws?.readyState === WebSocket.OPEN) {
        resolve();
        return;
      }

      this.options.onConnecting?.();

      try {
        this.ws = new WebSocket(this.options.url);
        this.ws.binaryType = 'arraybuffer';

        this.ws.onopen = () => {
          console.log('[WhisperClient] Connected to server');
          this.reconnectAttempts = 0;
          // Don't resolve yet - wait for 'ready' message
        };

        this.ws.onmessage = (event) => {
          if (typeof event.data === 'string') {
            try {
              const message: ServerMessage = JSON.parse(event.data);
              this.handleMessage(message, resolve);
            } catch (e) {
              console.error('[WhisperClient] Failed to parse message:', e);
            }
          }
        };

        this.ws.onerror = (event) => {
          console.error('[WhisperClient] WebSocket error:', event);
          const error = new Error('WebSocket connection failed');
          this.options.onError?.(error);
          reject(error);
        };

        this.ws.onclose = (event) => {
          console.log('[WhisperClient] Connection closed:', event.code, event.reason);
          this.ws = null;
          this.options.onClose?.();
        };
      } catch (error) {
        reject(error);
      }
    });
  }

  private handleMessage(message: ServerMessage, resolveConnect?: (value: void) => void): void {
    switch (message.type) {
      case 'ready':
        console.log('[WhisperClient] Server ready:', message.model);
        this.options.onReady?.(message);
        resolveConnect?.();
        break;

      case 'partial':
        this.options.onPartial?.(message.text);
        break;

      case 'final':
        this.options.onFinal?.(message.text);
        break;

      case 'error':
        console.error('[WhisperClient] Server error:', message.message);
        this.options.onError?.(new Error(message.message));
        break;
    }
  }

  disconnect(): void {
    if (this.ws) {
      this.ws.close(1000, 'Client disconnect');
      this.ws = null;
    }
  }

  sendAudio(pcmData: Int16Array): void {
    if (this.ws?.readyState === WebSocket.OPEN) {
      this.ws.send(pcmData.buffer);
    }
  }

  isConnected(): boolean {
    return this.ws?.readyState === WebSocket.OPEN;
  }
}
