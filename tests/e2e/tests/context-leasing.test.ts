/**
 * Context leasing tests for whisper-stream-server
 *
 * Tests that contexts are leased on speech start and released after final,
 * allowing more connections than available contexts.
 */

import { describe, it, expect, afterEach } from 'vitest';
import { TestClient } from '../utils/TestClient.js';
import {
  loadWavAsChunks,
  createSilence,
  splitIntoChunks,
  getFixturePath,
} from '../utils/WavLoader.js';

const SERVER_URL = process.env.WHISPER_SERVER_URL ?? 'ws://localhost:9090';
const JFK_WAV = getFixturePath('jfk.wav');

describe('Context Leasing', () => {
  const clients: TestClient[] = [];

  afterEach(async () => {
    // Clean up all clients
    for (const client of clients) {
      client.disconnect();
    }
    clients.length = 0;
    await new Promise((r) => setTimeout(r, 300));
  });

  it('allows more connections than contexts (server has 2 contexts)', async () => {
    // Connect 5 clients - more than the 2 contexts available
    for (let i = 0; i < 5; i++) {
      const client = new TestClient({ url: SERVER_URL });
      clients.push(client);
      await client.connect();
      await client.waitForReady();
    }

    // All 5 should be connected (no rejection)
    expect(clients.every((c) => c.isConnected())).toBe(true);
  });

  it('client can speak after another finishes (context reuse)', async () => {
    const client1 = new TestClient({ url: SERVER_URL });
    const client2 = new TestClient({ url: SERVER_URL });
    clients.push(client1, client2);

    await client1.connect();
    await client1.waitForReady();
    await client2.connect();
    await client2.waitForReady();

    // Client 1 speaks and finishes
    const audioChunks = loadWavAsChunks(JFK_WAV, 100);
    await client1.sendChunks(audioChunks.slice(0, 30), 20); // First 3 seconds

    // Send silence with real-time delays to trigger VAD ENDING state
    const silence = createSilence(100);
    for (let i = 0; i < 20; i++) {  // 2 seconds of silence
      client1.sendAudio(silence);
      await new Promise((r) => setTimeout(r, 100));
    }

    // Wait for final to be emitted and context released
    await new Promise((r) => setTimeout(r, 2000));

    const client1Finals = client1.getFinals();
    expect(client1Finals.length).toBeGreaterThan(0);

    // Now client 2 speaks - should get the released context
    await client2.sendChunks(audioChunks.slice(0, 30), 20);

    // Send silence with real-time delays
    for (let i = 0; i < 20; i++) {
      client2.sendAudio(silence);
      await new Promise((r) => setTimeout(r, 100));
    }

    await new Promise((r) => setTimeout(r, 2000));

    const client2Finals = client2.getFinals();
    expect(client2Finals.length).toBeGreaterThan(0);
  });

  it('multiple clients can speak sequentially sharing contexts', async () => {
    // Create 4 clients (more than 2 contexts)
    for (let i = 0; i < 4; i++) {
      const client = new TestClient({ url: SERVER_URL });
      clients.push(client);
      await client.connect();
      await client.waitForReady();
    }

    const audioChunks = loadWavAsChunks(JFK_WAV, 100);
    const silence = createSilence(100);

    // Each client speaks in sequence
    for (let i = 0; i < 4; i++) {
      const client = clients[i];

      // Send audio
      await client.sendChunks(audioChunks.slice(0, 20), 30); // 2 seconds

      // Send silence with real-time delays to trigger final
      for (let j = 0; j < 20; j++) {
        client.sendAudio(silence);
        await new Promise((r) => setTimeout(r, 100));
      }

      // Wait for final
      await new Promise((r) => setTimeout(r, 2500));

      const finals = client.getFinals();
      expect(finals.length).toBeGreaterThan(0);
    }
  });

  it('idle clients do not block active speakers', async () => {
    // Connect 3 idle clients first
    const idleClients: TestClient[] = [];
    for (let i = 0; i < 3; i++) {
      const client = new TestClient({ url: SERVER_URL });
      idleClients.push(client);
      clients.push(client);
      await client.connect();
      await client.waitForReady();
    }

    // Now connect an active speaker
    const speaker = new TestClient({ url: SERVER_URL });
    clients.push(speaker);
    await speaker.connect();
    await speaker.waitForReady();

    // Speaker should be able to speak despite 3 idle connections
    const audioChunks = loadWavAsChunks(JFK_WAV, 100);
    await speaker.sendChunks(audioChunks.slice(0, 30), 20);

    const silence = createSilence(2000);
    const silenceChunks = splitIntoChunks(silence, 100);
    await speaker.sendChunks(silenceChunks, 20);

    await new Promise((r) => setTimeout(r, 2500));

    // Speaker should have gotten transcription
    const finals = speaker.getFinals();
    const partials = speaker.getPartials();

    expect(finals.length + partials.length).toBeGreaterThan(0);

    // Idle clients should have no transcriptions (they never spoke)
    for (const idle of idleClients) {
      expect(idle.getFinals().length).toBe(0);
      expect(idle.getPartials().length).toBe(0);
    }
  });
});
