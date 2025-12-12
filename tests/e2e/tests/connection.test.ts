/**
 * Connection tests for whisper-stream-server
 *
 * Tests basic WebSocket connectivity, ready message, and disconnect handling.
 */

import { describe, it, expect, beforeEach, afterEach, beforeAll, afterAll } from 'vitest';
import { TestClient } from '../utils/TestClient.js';

const SERVER_URL = process.env.WHISPER_SERVER_URL ?? 'ws://localhost:9090';

// Run connection tests sequentially to avoid interference
describe.sequential('Connection', () => {
  let client: TestClient;

  beforeEach(async () => {
    client = new TestClient({ url: SERVER_URL });
    // Small delay between tests to let server recover
    await new Promise((r) => setTimeout(r, 200));
  });

  afterEach(async () => {
    client.disconnect();
    await new Promise((r) => setTimeout(r, 200));
  });

  it('receives ready message on connect', async () => {
    await client.connect();
    const ready = await client.waitForReady(5000);

    expect(ready.type).toBe('ready');
    expect(ready.model).toBeTruthy();
    expect(typeof ready.contexts).toBe('number');
    expect(ready.contexts).toBeGreaterThan(0);
  });

  it('can disconnect cleanly', async () => {
    await client.connect();
    await client.waitForReady();

    expect(client.isConnected()).toBe(true);
    client.disconnect();

    // Give it time to close
    await new Promise((r) => setTimeout(r, 500));
    expect(client.isConnected()).toBe(false);
  });

  it('can reconnect after disconnect', async () => {
    // First connection
    await client.connect();
    const ready1 = await client.waitForReady();
    expect(ready1.type).toBe('ready');

    client.disconnect();
    // Wait longer for cleanup
    await new Promise((r) => setTimeout(r, 500));

    // Create new client and reconnect
    const client2 = new TestClient({ url: SERVER_URL });
    await client2.connect();
    const ready2 = await client2.waitForReady();

    expect(ready2.type).toBe('ready');
    client2.disconnect();
    await new Promise((r) => setTimeout(r, 200));
  });

  it('supports multiple simultaneous clients', async () => {
    const client1 = new TestClient({ url: SERVER_URL });
    const client2 = new TestClient({ url: SERVER_URL });

    try {
      // Connect sequentially to avoid race conditions
      await client1.connect();
      await client1.waitForReady();

      await client2.connect();
      await client2.waitForReady();

      expect(client1.isConnected()).toBe(true);
      expect(client2.isConnected()).toBe(true);
    } finally {
      client1.disconnect();
      await new Promise((r) => setTimeout(r, 100));
      client2.disconnect();
      await new Promise((r) => setTimeout(r, 100));
    }
  });
});
