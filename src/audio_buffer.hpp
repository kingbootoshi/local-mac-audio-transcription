#ifndef AUDIO_BUFFER_HPP
#define AUDIO_BUFFER_HPP

#include <vector>
#include <deque>
#include <mutex>
#include <cstdint>
#include <cstring>

// Thread-safe ring buffer for accumulating incoming PCM audio
// Converts int16 input to float32 (whisper's expected format)
class AudioBuffer {
public:
    // max_seconds: maximum audio to retain (default 30s for whisper's context)
    explicit AudioBuffer(float max_seconds = 30.0f, int sample_rate = 16000);

    // Push int16 PCM samples (from WebSocket). Thread-safe.
    void push(const int16_t* samples, size_t count);

    // Push float32 samples directly. Thread-safe.
    void pushFloat(const float* samples, size_t count);

    // Get up to max_samples of audio. Returns actual count retrieved.
    // If clear_retrieved is true, removes the returned samples from buffer.
    // Thread-safe.
    size_t get(float* out, size_t max_samples, bool clear_retrieved = false);

    // Get the last N milliseconds of audio (for sliding window).
    // Thread-safe.
    std::vector<float> getLastMs(int ms);

    // Get all available audio without removing it. Thread-safe.
    std::vector<float> getAll();

    // Clear all buffered audio. Thread-safe.
    void clear();

    // Get current buffer size in samples. Thread-safe.
    size_t size() const;

    // Get current buffer duration in milliseconds. Thread-safe.
    float durationMs() const;

    // Check if we have at least min_ms of audio. Thread-safe.
    bool hasMinDuration(int min_ms) const;

private:
    std::deque<float> buffer_;
    mutable std::mutex mutex_;
    size_t max_samples_;
    int sample_rate_;

    // Convert int16 to float32 (normalized to [-1, 1])
    static float int16ToFloat(int16_t sample) {
        return static_cast<float>(sample) / 32768.0f;
    }
};

#endif // AUDIO_BUFFER_HPP
