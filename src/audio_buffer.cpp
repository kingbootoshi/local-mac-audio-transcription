#include "audio_buffer.hpp"

AudioBuffer::AudioBuffer(float max_seconds, int sample_rate)
    : max_samples_(static_cast<size_t>(max_seconds * sample_rate))
    , sample_rate_(sample_rate) {
}

void AudioBuffer::push(const int16_t* samples, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (size_t i = 0; i < count; ++i) {
        buffer_.push_back(int16ToFloat(samples[i]));
    }

    // Trim to max size
    while (buffer_.size() > max_samples_) {
        buffer_.pop_front();
    }
}

void AudioBuffer::pushFloat(const float* samples, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (size_t i = 0; i < count; ++i) {
        buffer_.push_back(samples[i]);
    }

    // Trim to max size
    while (buffer_.size() > max_samples_) {
        buffer_.pop_front();
    }
}

size_t AudioBuffer::get(float* out, size_t max_samples, bool clear_retrieved) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t count = std::min(max_samples, buffer_.size());
    for (size_t i = 0; i < count; ++i) {
        out[i] = buffer_[i];
    }

    if (clear_retrieved) {
        for (size_t i = 0; i < count; ++i) {
            buffer_.pop_front();
        }
    }

    return count;
}

std::vector<float> AudioBuffer::getLastMs(int ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t n_samples = static_cast<size_t>((ms / 1000.0f) * sample_rate_);
    n_samples = std::min(n_samples, buffer_.size());

    std::vector<float> result(n_samples);
    size_t start = buffer_.size() - n_samples;
    for (size_t i = 0; i < n_samples; ++i) {
        result[i] = buffer_[start + i];
    }

    return result;
}

std::vector<float> AudioBuffer::getAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<float>(buffer_.begin(), buffer_.end());
}

void AudioBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.clear();
}

size_t AudioBuffer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_.size();
}

float AudioBuffer::durationMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (buffer_.size() * 1000.0f) / sample_rate_;
}

bool AudioBuffer::hasMinDuration(int min_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t min_samples = static_cast<size_t>((min_ms / 1000.0f) * sample_rate_);
    return buffer_.size() >= min_samples;
}
