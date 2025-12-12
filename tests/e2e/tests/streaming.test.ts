/**
 * Streaming tests for whisper-stream-server
 *
 * Tests audio streaming and transcription using the JFK fixture.
 */

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { TestClient } from '../utils/TestClient.js';
import { loadWavAsChunks, getFixturePath, createSilence } from '../utils/WavLoader.js';

const SERVER_URL = process.env.WHISPER_SERVER_URL ?? 'ws://localhost:9090';
const JFK_WAV = getFixturePath('jfk.wav');

describe('Streaming', () => {
  let client: TestClient;

  beforeEach(async () => {
    client = new TestClient({ url: SERVER_URL });
    await client.connect();
    await client.waitForReady();
  });

  afterEach(() => {
    client.disconnect();
  });

  it('receives partials when streaming JFK audio', async () => {
    const chunks = loadWavAsChunks(JFK_WAV, 100);  // 100ms chunks

    // Stream with small delay to allow message draining
    // (server drains messages only when audio is received)
    await client.sendChunks(chunks, 10);

    // Send a few more silence chunks to drain remaining messages
    const silence = createSilence(100);
    for (let i = 0; i < 20; i++) {
      client.sendAudio(silence);
      await new Promise((r) => setTimeout(r, 100));
    }

    const partials = client.getPartials();
    expect(partials.length).toBeGreaterThan(0);
  });

  it('partial contains expected keywords', async () => {
    const chunks = loadWavAsChunks(JFK_WAV, 100);

    // Stream with delay to allow draining
    await client.sendChunks(chunks, 10);

    // Drain remaining messages
    const silence = createSilence(100);
    for (let i = 0; i < 20; i++) {
      client.sendAudio(silence);
      await new Promise((r) => setTimeout(r, 100));
    }

    const partials = client.getPartials();
    const allText = partials.join(' ').toLowerCase();

    // Should contain at least one of the expected keywords
    const hasExpectedContent =
      allText.includes('ask') ||
      allText.includes('country') ||
      allText.includes('fellow') ||
      allText.includes('americans');

    expect(hasExpectedContent).toBe(true);
  });

  it('works with 50ms chunks', async () => {
    const chunks = loadWavAsChunks(JFK_WAV, 50);  // Smaller chunks

    await client.sendChunks(chunks, 5);

    // Drain messages
    const silence = createSilence(100);
    for (let i = 0; i < 10; i++) {
      client.sendAudio(silence);
      await new Promise((r) => setTimeout(r, 100));
    }

    const partials = client.getPartials();
    expect(partials.length).toBeGreaterThan(0);
  });

  it('works with 200ms chunks', async () => {
    const chunks = loadWavAsChunks(JFK_WAV, 200);  // Larger chunks

    await client.sendChunks(chunks, 20);

    // Drain messages
    const silence = createSilence(100);
    for (let i = 0; i < 10; i++) {
      client.sendAudio(silence);
      await new Promise((r) => setTimeout(r, 100));
    }

    const partials = client.getPartials();
    expect(partials.length).toBeGreaterThan(0);
  });

  it('handles rapid streaming without crashing', async () => {
    const chunks = loadWavAsChunks(JFK_WAV, 100);

    // Stream rapidly (no delay between chunks)
    await client.sendChunks(chunks, 0);

    // The main test is that we're still connected after rapid streaming
    expect(client.isConnected()).toBe(true);

    // Note: Partials may or may not be received immediately since
    // message draining happens on audio receive and rapid streaming
    // doesn't give the inference thread time to process.
    // This test verifies the server doesn't crash under load.
  });

  it('streams at real-time speed', async () => {
    const chunks = loadWavAsChunks(JFK_WAV, 100);  // 100ms chunks

    // Stream at 1x speed (100ms between chunks)
    const startTime = Date.now();
    await client.sendChunks(chunks, 100);
    const elapsed = Date.now() - startTime;

    // Should take approximately the audio duration (11 seconds)
    // Allow some variance
    expect(elapsed).toBeGreaterThan(10000);
    expect(elapsed).toBeLessThan(15000);

    // Wait a bit more for final processing
    await new Promise((r) => setTimeout(r, 1000));

    const partials = client.getPartials();
    expect(partials.length).toBeGreaterThan(0);
  });
});
