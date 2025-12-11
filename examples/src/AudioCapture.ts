import type { AudioCaptureOptions } from './types';

const WHISPER_SAMPLE_RATE = 16000;

export class AudioCapture {
  private audioContext: AudioContext | null = null;
  private mediaStream: MediaStream | null = null;
  private workletNode: AudioWorkletNode | null = null;
  private options: Required<AudioCaptureOptions>;
  private isCapturing = false;

  constructor(options: AudioCaptureOptions = {}) {
    this.options = {
      sampleRate: options.sampleRate ?? WHISPER_SAMPLE_RATE,
      chunkDurationMs: options.chunkDurationMs ?? 100,
      onAudioChunk: options.onAudioChunk ?? (() => {}),
      onError: options.onError ?? console.error,
    };
  }

  async start(): Promise<void> {
    if (this.isCapturing) {
      return;
    }

    try {
      // Request microphone access
      this.mediaStream = await navigator.mediaDevices.getUserMedia({
        audio: {
          channelCount: 1,
          sampleRate: { ideal: this.options.sampleRate },
          echoCancellation: true,
          noiseSuppression: true,
        },
      });

      // Create audio context
      this.audioContext = new AudioContext({
        sampleRate: this.options.sampleRate,
      });

      // If browser sample rate differs from our target, we'll need to resample
      const actualSampleRate = this.audioContext.sampleRate;
      const needsResampling = actualSampleRate !== this.options.sampleRate;

      console.log(`[AudioCapture] Browser sample rate: ${actualSampleRate}, target: ${this.options.sampleRate}`);

      // Create source from microphone
      const source = this.audioContext.createMediaStreamSource(this.mediaStream);

      // Use ScriptProcessorNode for audio processing (simpler than AudioWorklet for this use case)
      const bufferSize = 4096;
      const scriptNode = this.audioContext.createScriptProcessor(bufferSize, 1, 1);

      // Resampling state
      let resampleBuffer: number[] = [];
      const resampleRatio = needsResampling ? this.options.sampleRate / actualSampleRate : 1;

      scriptNode.onaudioprocess = (event) => {
        const inputData = event.inputBuffer.getChannelData(0);

        let samples: Float32Array;

        if (needsResampling) {
          // Simple linear interpolation resampling
          samples = this.resample(inputData, actualSampleRate, this.options.sampleRate);
        } else {
          samples = inputData;
        }

        // Convert float32 to int16
        const int16Data = this.float32ToInt16(samples);

        // Send to callback
        this.options.onAudioChunk(int16Data);
      };

      // Connect the audio graph
      source.connect(scriptNode);
      scriptNode.connect(this.audioContext.destination);

      this.isCapturing = true;
      console.log('[AudioCapture] Started capturing audio');
    } catch (error) {
      this.options.onError(error as Error);
      throw error;
    }
  }

  stop(): void {
    if (!this.isCapturing) {
      return;
    }

    // Stop media stream tracks
    if (this.mediaStream) {
      this.mediaStream.getTracks().forEach((track) => track.stop());
      this.mediaStream = null;
    }

    // Close audio context
    if (this.audioContext) {
      this.audioContext.close();
      this.audioContext = null;
    }

    this.workletNode = null;
    this.isCapturing = false;
    console.log('[AudioCapture] Stopped capturing audio');
  }

  isActive(): boolean {
    return this.isCapturing;
  }

  // Simple linear interpolation resampling
  private resample(input: Float32Array, fromRate: number, toRate: number): Float32Array {
    if (fromRate === toRate) {
      return input;
    }

    const ratio = fromRate / toRate;
    const outputLength = Math.round(input.length / ratio);
    const output = new Float32Array(outputLength);

    for (let i = 0; i < outputLength; i++) {
      const srcIndex = i * ratio;
      const srcIndexFloor = Math.floor(srcIndex);
      const srcIndexCeil = Math.min(srcIndexFloor + 1, input.length - 1);
      const t = srcIndex - srcIndexFloor;

      // Linear interpolation
      output[i] = input[srcIndexFloor] * (1 - t) + input[srcIndexCeil] * t;
    }

    return output;
  }

  // Convert float32 [-1, 1] to int16 [-32768, 32767]
  private float32ToInt16(float32Array: Float32Array): Int16Array {
    const int16Array = new Int16Array(float32Array.length);

    for (let i = 0; i < float32Array.length; i++) {
      // Clamp to [-1, 1]
      const s = Math.max(-1, Math.min(1, float32Array[i]));
      // Convert to int16
      int16Array[i] = s < 0 ? s * 0x8000 : s * 0x7fff;
    }

    return int16Array;
  }
}
