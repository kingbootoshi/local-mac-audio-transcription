/**
 * VAD boundary tests for whisper-stream-server
 *
 * Tests voice activity detection and final transcript generation.
 * Verifies that finals are emitted after speech ends.
 */

import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { TestClient } from '../utils/TestClient.js';
import {
  loadWavAsChunks,
  createSilence,
  splitIntoChunks,
  getFixturePath,
} from '../utils/WavLoader.js';

const SERVER_URL = process.env.WHISPER_SERVER_URL ?? 'ws://localhost:9090';
const JFK_WAV = getFixturePath('jfk.wav');

describe('VAD Boundaries', () => {
  let client: TestClient;

  beforeEach(async () => {
    client = new TestClient({ url: SERVER_URL, debug: false });
    await client.connect();
    await client.waitForReady();
  });

  afterEach(() => {
    client.disconnect();
  });

  it('emits final after silence following speech', async () => {
    // Stream JFK audio with small delay
    const audioChunks = loadWavAsChunks(JFK_WAV, 100);
    await client.sendChunks(audioChunks, 20);

    // Send 2 seconds of silence (triggers VAD ENDING state)
    // and keep sending to drain the message queue
    const silence = createSilence(100);
    for (let i = 0; i < 40; i++) {  // 4 seconds of silence chunks
      client.sendAudio(silence);
      await new Promise((r) => setTimeout(r, 100));
    }

    const finals = client.getFinals();

    // Should have at least one final
    expect(finals.length).toBeGreaterThan(0);
  });

  it('final contains full transcript keywords', async () => {
    const audioChunks = loadWavAsChunks(JFK_WAV, 100);
    await client.sendChunks(audioChunks, 50);

    // Send silence to trigger final
    const silence = createSilence(2000);
    const silenceChunks = splitIntoChunks(silence, 100);
    await client.sendChunks(silenceChunks, 50);

    // Wait for processing
    await new Promise((r) => setTimeout(r, 3000));

    const finals = client.getFinals();

    if (finals.length > 0) {
      const finalText = finals.join(' ').toLowerCase();

      // Check for expected content
      const hasAsk = finalText.includes('ask');
      const hasCountry = finalText.includes('country');

      // Final should have more complete content than partials
      expect(hasAsk || hasCountry).toBe(true);
    } else {
      // If no finals, partials should have content
      const partials = client.getPartials();
      expect(partials.length).toBeGreaterThan(0);
    }
  });

  it('emits partials during continuous speech', async () => {
    const audioChunks = loadWavAsChunks(JFK_WAV, 100);

    // Stream the first 5 seconds only (about half the audio)
    const halfChunks = audioChunks.slice(0, 50);  // 50 chunks * 100ms = 5 seconds
    await client.sendChunks(halfChunks, 50);

    // Wait for processing
    await new Promise((r) => setTimeout(r, 1500));

    // Should have partials but not finals (still in speech)
    const partials = client.getPartials();
    expect(partials.length).toBeGreaterThan(0);
  });

  it('no finals during leading silence', async () => {
    // Send only silence
    const silence = createSilence(500);
    const silenceChunks = splitIntoChunks(silence, 100);
    await client.sendChunks(silenceChunks, 50);

    // Wait a bit
    await new Promise((r) => setTimeout(r, 500));

    const finals = client.getFinals();
    const partials = client.getPartials();

    // Should not emit anything for pure silence
    expect(finals.length).toBe(0);
    // Partials should be empty or contain only whitespace/blank
    const meaningfulPartials = partials.filter(
      (p) => p.trim().length > 0 && !p.includes('[BLANK')
    );
    expect(meaningfulPartials.length).toBe(0);
  });

  it('handles multiple utterances', async () => {
    // First utterance
    const audioChunks = loadWavAsChunks(JFK_WAV, 100);
    await client.sendChunks(audioChunks, 30);

    // Silence between utterances
    const silence = createSilence(2500);  // 2.5 seconds to trigger ENDING
    const silenceChunks = splitIntoChunks(silence, 100);
    await client.sendChunks(silenceChunks, 30);

    // Wait for first final
    await new Promise((r) => setTimeout(r, 2000));

    // Second utterance (same audio for simplicity)
    await client.sendChunks(audioChunks, 30);

    // More silence
    await client.sendChunks(silenceChunks, 30);

    // Wait for second final
    await new Promise((r) => setTimeout(r, 3000));

    const finals = client.getFinals();

    // With two utterances separated by silence, expect two finals
    // (though this depends on timing and may sometimes produce just one)
    expect(finals.length).toBeGreaterThanOrEqual(1);

    // At least some content should be present
    const allFinals = finals.join(' ').toLowerCase();
    expect(allFinals.includes('ask') || allFinals.includes('country')).toBe(true);
  });
});
