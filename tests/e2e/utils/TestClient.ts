/**
 * WebSocket test client for E2E tests
 *
 * Provides a simple interface for connecting to the whisper-stream-server,
 * sending audio chunks, and collecting transcription messages.
 */

import WebSocket from 'ws';

// Server message types
interface ReadyMessage {
  type: 'ready';
  model: string;
  contexts: number;
}

interface PartialMessage {
  type: 'partial';
  text: string;
}

interface FinalMessage {
  type: 'final';
  text: string;
}

interface ErrorMessage {
  type: 'error';
  message: string;
}

export type ServerMessage = ReadyMessage | PartialMessage | FinalMessage | ErrorMessage;

interface TestClientOptions {
  url?: string;
  debug?: boolean;
}

export class TestClient {
  private ws: WebSocket | null = null;
  private messages: ServerMessage[] = [];
  private url: string;
  private debug: boolean;
  private messageResolvers: Array<{
    resolve: (msg: ServerMessage) => void;
    reject: (err: Error) => void;
    filter?: (msg: ServerMessage) => boolean;
  }> = [];
  private closePromise: Promise<void> | null = null;

  constructor(options: TestClientOptions = {}) {
    this.url = options.url ?? 'ws://localhost:9090';
    this.debug = options.debug ?? false;
  }

  /**
   * Connect to the server
   */
  async connect(): Promise<void> {
    return new Promise((resolve, reject) => {
      this.ws = new WebSocket(this.url);
      this.ws.binaryType = 'arraybuffer';

      const timeout = setTimeout(() => {
        reject(new Error('Connection timeout'));
        this.ws?.close();
      }, 5000);

      this.ws.on('open', () => {
        if (this.debug) console.log('[TestClient] Connected');
        clearTimeout(timeout);
        resolve();
      });

      this.ws.on('message', (data: Buffer | ArrayBuffer) => {
        if (typeof data === 'string' || data instanceof Buffer) {
          const text = data.toString();
          try {
            const msg = JSON.parse(text) as ServerMessage;
            if (this.debug) console.log('[TestClient] Received:', msg);
            this.messages.push(msg);

            // Resolve any waiting promises
            for (let i = this.messageResolvers.length - 1; i >= 0; i--) {
              const resolver = this.messageResolvers[i];
              if (!resolver.filter || resolver.filter(msg)) {
                resolver.resolve(msg);
                this.messageResolvers.splice(i, 1);
              }
            }
          } catch (e) {
            console.error('[TestClient] Failed to parse message:', text);
          }
        }
      });

      this.ws.on('error', (err) => {
        clearTimeout(timeout);
        reject(err);
      });

      this.ws.on('close', () => {
        if (this.debug) console.log('[TestClient] Disconnected');
      });
    });
  }

  /**
   * Wait for the ready message
   */
  async waitForReady(timeout: number = 5000): Promise<ReadyMessage> {
    // Check if we already have a ready message
    const existing = this.messages.find((m) => m.type === 'ready') as ReadyMessage | undefined;
    if (existing) return existing;

    return this.waitForMessage(
      (msg) => msg.type === 'ready',
      timeout
    ) as Promise<ReadyMessage>;
  }

  /**
   * Wait for any message matching a filter
   */
  async waitForMessage(
    filter?: (msg: ServerMessage) => boolean,
    timeout: number = 5000
  ): Promise<ServerMessage> {
    return new Promise((resolve, reject) => {
      // Check existing messages first
      if (filter) {
        const existing = this.messages.find(filter);
        if (existing) {
          resolve(existing);
          return;
        }
      } else if (this.messages.length > 0) {
        resolve(this.messages[this.messages.length - 1]);
        return;
      }

      const timer = setTimeout(() => {
        const idx = this.messageResolvers.findIndex((r) => r.resolve === resolve);
        if (idx !== -1) this.messageResolvers.splice(idx, 1);
        reject(new Error('Timeout waiting for message'));
      }, timeout);

      this.messageResolvers.push({
        resolve: (msg) => {
          clearTimeout(timer);
          resolve(msg);
        },
        reject: (err) => {
          clearTimeout(timer);
          reject(err);
        },
        filter,
      });
    });
  }

  /**
   * Wait for a partial message
   */
  async waitForPartial(timeout: number = 10000): Promise<string> {
    const msg = await this.waitForMessage((m) => m.type === 'partial', timeout);
    return (msg as PartialMessage).text;
  }

  /**
   * Wait for a final message
   */
  async waitForFinal(timeout: number = 15000): Promise<string> {
    const msg = await this.waitForMessage((m) => m.type === 'final', timeout);
    return (msg as FinalMessage).text;
  }

  /**
   * Send audio chunks with optional delay between each
   */
  async sendChunks(chunks: Int16Array[], delayMs: number = 0): Promise<void> {
    for (const chunk of chunks) {
      if (this.ws?.readyState !== WebSocket.OPEN) {
        throw new Error('WebSocket not connected');
      }

      // Send as binary ArrayBuffer
      this.ws.send(chunk.buffer);

      if (delayMs > 0) {
        await new Promise((r) => setTimeout(r, delayMs));
      }
    }
  }

  /**
   * Send a single audio chunk
   */
  sendAudio(samples: Int16Array): void {
    if (this.ws?.readyState !== WebSocket.OPEN) {
      throw new Error('WebSocket not connected');
    }
    this.ws.send(samples.buffer);
  }

  /**
   * Get all received messages
   */
  getMessages(): ServerMessage[] {
    return [...this.messages];
  }

  /**
   * Get all partial messages
   */
  getPartials(): string[] {
    return this.messages
      .filter((m) => m.type === 'partial')
      .map((m) => (m as PartialMessage).text);
  }

  /**
   * Get all final messages
   */
  getFinals(): string[] {
    return this.messages
      .filter((m) => m.type === 'final')
      .map((m) => (m as FinalMessage).text);
  }

  /**
   * Clear message history
   */
  clearMessages(): void {
    this.messages = [];
  }

  /**
   * Check if connected
   */
  isConnected(): boolean {
    return this.ws?.readyState === WebSocket.OPEN;
  }

  /**
   * Disconnect from server
   */
  disconnect(): void {
    if (this.ws) {
      this.ws.close(1000, 'Test complete');
      this.ws = null;
    }
  }

  /**
   * Wait for disconnect
   */
  async waitForClose(timeout: number = 5000): Promise<void> {
    if (!this.ws || this.ws.readyState === WebSocket.CLOSED) {
      return;
    }

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        reject(new Error('Timeout waiting for close'));
      }, timeout);

      this.ws!.once('close', () => {
        clearTimeout(timer);
        resolve();
      });
    });
  }
}
