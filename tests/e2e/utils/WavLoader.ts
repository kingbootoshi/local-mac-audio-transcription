/**
 * WAV file loader for E2E tests
 *
 * Loads WAV files and splits them into chunks for streaming to the server.
 * Handles WAV files with extra chunks (LIST, INFO, etc.).
 */

import * as fs from 'fs';
import * as path from 'path';

interface WavHeader {
  sampleRate: number;
  numChannels: number;
  bitsPerSample: number;
  dataSize: number;
}

/**
 * Parse WAV file header, handling extra chunks
 */
function parseWavHeader(buffer: Buffer): { header: WavHeader; dataOffset: number } {
  // Verify RIFF header
  if (buffer.toString('utf8', 0, 4) !== 'RIFF' || buffer.toString('utf8', 8, 12) !== 'WAVE') {
    throw new Error('Not a valid WAV file');
  }

  let offset = 12;  // After RIFF header
  let header: WavHeader | null = null;
  let dataOffset = 0;

  // Parse chunks
  while (offset < buffer.length - 8) {
    const chunkId = buffer.toString('utf8', offset, offset + 4);
    const chunkSize = buffer.readUInt32LE(offset + 4);
    offset += 8;

    if (chunkId === 'fmt ') {
      header = {
        sampleRate: buffer.readUInt32LE(offset + 4),
        numChannels: buffer.readUInt16LE(offset + 2),
        bitsPerSample: buffer.readUInt16LE(offset + 14),
        dataSize: 0,  // Will be set later
      };
      offset += chunkSize;
    } else if (chunkId === 'data') {
      if (header) {
        header.dataSize = chunkSize;
      }
      dataOffset = offset;
      break;  // Found data, we're done
    } else {
      // Skip unknown chunks
      offset += chunkSize;
    }
  }

  if (!header || dataOffset === 0) {
    throw new Error('WAV file missing fmt or data chunk');
  }

  return { header, dataOffset };
}

/**
 * Load a WAV file and return Int16Array samples
 *
 * @param filePath Path to WAV file
 * @returns Int16Array of audio samples
 */
export function loadWav(filePath: string): Int16Array {
  const absolutePath = path.resolve(filePath);
  const buffer = fs.readFileSync(absolutePath);

  const { header, dataOffset } = parseWavHeader(buffer);

  // Verify format
  if (header.numChannels !== 1) {
    throw new Error(`WAV must be mono (got ${header.numChannels} channels)`);
  }
  if (header.bitsPerSample !== 16) {
    throw new Error(`WAV must be 16-bit (got ${header.bitsPerSample}-bit)`);
  }

  // Extract samples
  const numSamples = header.dataSize / 2;  // 16-bit = 2 bytes per sample
  const samples = new Int16Array(numSamples);

  for (let i = 0; i < numSamples; i++) {
    samples[i] = buffer.readInt16LE(dataOffset + i * 2);
  }

  console.log(`[WavLoader] Loaded ${numSamples} samples from ${filePath} (${header.sampleRate}Hz)`);

  return samples;
}

/**
 * Split audio samples into chunks of specified duration
 *
 * @param samples Int16Array of audio samples
 * @param chunkMs Chunk duration in milliseconds
 * @param sampleRate Sample rate (default 16000)
 * @returns Array of Int16Array chunks
 */
export function splitIntoChunks(
  samples: Int16Array,
  chunkMs: number = 100,
  sampleRate: number = 16000
): Int16Array[] {
  const samplesPerChunk = Math.floor((sampleRate * chunkMs) / 1000);
  const chunks: Int16Array[] = [];

  for (let i = 0; i < samples.length; i += samplesPerChunk) {
    const end = Math.min(i + samplesPerChunk, samples.length);
    chunks.push(samples.slice(i, end));
  }

  return chunks;
}

/**
 * Load WAV file and split into chunks
 *
 * @param filePath Path to WAV file
 * @param chunkMs Chunk duration in milliseconds (default 100ms)
 * @returns Array of Int16Array chunks
 */
export function loadWavAsChunks(filePath: string, chunkMs: number = 100): Int16Array[] {
  const samples = loadWav(filePath);
  return splitIntoChunks(samples, chunkMs);
}

/**
 * Create silence of specified duration
 *
 * @param durationMs Duration in milliseconds
 * @param sampleRate Sample rate (default 16000)
 * @returns Int16Array of silence
 */
export function createSilence(durationMs: number, sampleRate: number = 16000): Int16Array {
  const numSamples = Math.floor((sampleRate * durationMs) / 1000);
  return new Int16Array(numSamples);  // Zeros = silence
}

/**
 * Get fixture path relative to this file
 */
export function getFixturePath(filename: string): string {
  return path.join(__dirname, '..', '..', 'fixtures', filename);
}
