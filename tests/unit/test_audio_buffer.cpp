/**
 * Unit tests for AudioBuffer class
 *
 * Tests the thread-safe ring buffer that accumulates incoming PCM audio
 * and converts int16 to float32 for whisper inference.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "audio_buffer.hpp"

#include <thread>
#include <vector>
#include <cmath>
#include <atomic>

using Catch::Matchers::WithinAbs;

// ============================================================================
// Basic Push/Size Tests
// ============================================================================

TEST_CASE("AudioBuffer: Initial state is empty", "[audio][buffer]") {
    AudioBuffer buffer(1.0f, 16000);  // 1 second max, 16kHz

    REQUIRE(buffer.size() == 0);
    REQUIRE(buffer.durationMs() == 0.0f);
    REQUIRE(buffer.hasMinDuration(0) == true);
    REQUIRE(buffer.hasMinDuration(1) == false);
}

TEST_CASE("AudioBuffer: push increments size", "[audio][buffer]") {
    AudioBuffer buffer(1.0f, 16000);

    int16_t samples[] = {1000, 2000, 3000, 4000, 5000};
    buffer.push(samples, 5);

    REQUIRE(buffer.size() == 5);
}

TEST_CASE("AudioBuffer: multiple pushes accumulate", "[audio][buffer]") {
    AudioBuffer buffer(1.0f, 16000);

    int16_t samples1[] = {1000, 2000, 3000};
    int16_t samples2[] = {4000, 5000};
    buffer.push(samples1, 3);
    buffer.push(samples2, 2);

    REQUIRE(buffer.size() == 5);
}

// ============================================================================
// Int16 to Float32 Conversion Tests
// ============================================================================

TEST_CASE("AudioBuffer: int16 to float32 conversion - positive max", "[audio][conversion]") {
    AudioBuffer buffer(1.0f, 16000);

    int16_t samples[] = {32767};  // Max positive
    buffer.push(samples, 1);

    std::vector<float> result = buffer.getAll();
    REQUIRE(result.size() == 1);
    REQUIRE_THAT(result[0], WithinAbs(1.0f, 0.0001f));
}

TEST_CASE("AudioBuffer: int16 to float32 conversion - negative max", "[audio][conversion]") {
    AudioBuffer buffer(1.0f, 16000);

    int16_t samples[] = {-32768};  // Max negative
    buffer.push(samples, 1);

    std::vector<float> result = buffer.getAll();
    REQUIRE(result.size() == 1);
    REQUIRE_THAT(result[0], WithinAbs(-1.0f, 0.0001f));
}

TEST_CASE("AudioBuffer: int16 to float32 conversion - zero", "[audio][conversion]") {
    AudioBuffer buffer(1.0f, 16000);

    int16_t samples[] = {0};
    buffer.push(samples, 1);

    std::vector<float> result = buffer.getAll();
    REQUIRE(result.size() == 1);
    REQUIRE_THAT(result[0], WithinAbs(0.0f, 0.0001f));
}

TEST_CASE("AudioBuffer: int16 to float32 conversion - mid values", "[audio][conversion]") {
    AudioBuffer buffer(1.0f, 16000);

    int16_t samples[] = {16384, -16384};  // ~0.5 and ~-0.5
    buffer.push(samples, 2);

    std::vector<float> result = buffer.getAll();
    REQUIRE(result.size() == 2);
    REQUIRE_THAT(result[0], WithinAbs(0.5f, 0.001f));
    REQUIRE_THAT(result[1], WithinAbs(-0.5f, 0.001f));
}

// ============================================================================
// getAll() Tests
// ============================================================================

TEST_CASE("AudioBuffer: getAll returns complete buffer", "[audio][buffer]") {
    AudioBuffer buffer(1.0f, 16000);

    int16_t samples[] = {1000, 2000, 3000, 4000, 5000};
    buffer.push(samples, 5);

    std::vector<float> result = buffer.getAll();
    REQUIRE(result.size() == 5);

    // Verify values are converted correctly
    REQUIRE_THAT(result[0], WithinAbs(1000.0f / 32768.0f, 0.0001f));
    REQUIRE_THAT(result[4], WithinAbs(5000.0f / 32768.0f, 0.0001f));
}

TEST_CASE("AudioBuffer: getAll on empty buffer", "[audio][buffer]") {
    AudioBuffer buffer(1.0f, 16000);

    std::vector<float> result = buffer.getAll();
    REQUIRE(result.empty());
}

// ============================================================================
// getLastMs() Tests
// ============================================================================

TEST_CASE("AudioBuffer: getLastMs returns correct slice", "[audio][buffer]") {
    AudioBuffer buffer(1.0f, 16000);

    // Push 1600 samples (100ms at 16kHz)
    std::vector<int16_t> samples(1600, 1000);
    buffer.push(samples.data(), samples.size());

    // Get last 50ms (800 samples)
    std::vector<float> result = buffer.getLastMs(50);
    REQUIRE(result.size() == 800);
}

TEST_CASE("AudioBuffer: getLastMs returns all if requested more than available", "[audio][buffer]") {
    AudioBuffer buffer(1.0f, 16000);

    // Push 800 samples (50ms)
    std::vector<int16_t> samples(800, 1000);
    buffer.push(samples.data(), samples.size());

    // Request last 100ms (1600 samples) but only 800 available
    std::vector<float> result = buffer.getLastMs(100);
    REQUIRE(result.size() == 800);
}

TEST_CASE("AudioBuffer: getLastMs on empty buffer", "[audio][buffer]") {
    AudioBuffer buffer(1.0f, 16000);

    std::vector<float> result = buffer.getLastMs(100);
    REQUIRE(result.empty());
}

// ============================================================================
// Duration Tests
// ============================================================================

TEST_CASE("AudioBuffer: durationMs calculation", "[audio][duration]") {
    AudioBuffer buffer(1.0f, 16000);

    // Push 16000 samples (1000ms at 16kHz)
    std::vector<int16_t> samples(16000, 1000);
    buffer.push(samples.data(), samples.size());

    REQUIRE_THAT(buffer.durationMs(), WithinAbs(1000.0f, 0.1f));
}

TEST_CASE("AudioBuffer: hasMinDuration", "[audio][duration]") {
    AudioBuffer buffer(1.0f, 16000);

    // Push 800 samples (50ms)
    std::vector<int16_t> samples(800, 1000);
    buffer.push(samples.data(), samples.size());

    REQUIRE(buffer.hasMinDuration(50) == true);
    REQUIRE(buffer.hasMinDuration(51) == false);
    REQUIRE(buffer.hasMinDuration(100) == false);
}

// ============================================================================
// Clear Tests
// ============================================================================

TEST_CASE("AudioBuffer: clear empties buffer", "[audio][buffer]") {
    AudioBuffer buffer(1.0f, 16000);

    int16_t samples[] = {1000, 2000, 3000};
    buffer.push(samples, 3);

    REQUIRE(buffer.size() == 3);

    buffer.clear();

    REQUIRE(buffer.size() == 0);
    REQUIRE(buffer.durationMs() == 0.0f);
    REQUIRE(buffer.getAll().empty());
}

// ============================================================================
// Max Capacity Tests
// ============================================================================

TEST_CASE("AudioBuffer: max capacity enforced", "[audio][capacity]") {
    // 0.5 second max = 8000 samples at 16kHz
    AudioBuffer buffer(0.5f, 16000);

    // Push 10000 samples (more than max)
    std::vector<int16_t> samples(10000, 1000);
    buffer.push(samples.data(), samples.size());

    // Should be capped at 8000
    REQUIRE(buffer.size() == 8000);
}

TEST_CASE("AudioBuffer: oldest samples dropped when capacity exceeded", "[audio][capacity]") {
    // 0.1 second max = 1600 samples at 16kHz
    AudioBuffer buffer(0.1f, 16000);

    // Push first batch of 1000 samples with value 1
    std::vector<int16_t> first(1000, 1);
    buffer.push(first.data(), first.size());

    // Push second batch of 1000 samples with value 2
    std::vector<int16_t> second(1000, 2);
    buffer.push(second.data(), second.size());

    // Should have 1600 samples, some from first batch dropped
    REQUIRE(buffer.size() == 1600);

    // The buffer should contain more of the second batch
    std::vector<float> result = buffer.getAll();

    // First 600 should be from first batch (value ~1/32768)
    // Last 1000 should be from second batch (value ~2/32768)
    float expected_first = 1.0f / 32768.0f;
    float expected_second = 2.0f / 32768.0f;

    // Check beginning and end to verify FIFO behavior
    REQUIRE_THAT(result[0], WithinAbs(expected_first, 0.0001f));
    REQUIRE_THAT(result[1599], WithinAbs(expected_second, 0.0001f));
}

// ============================================================================
// get() with partial retrieval
// ============================================================================

TEST_CASE("AudioBuffer: get retrieves up to max samples", "[audio][buffer]") {
    AudioBuffer buffer(1.0f, 16000);

    int16_t samples[] = {1000, 2000, 3000, 4000, 5000};
    buffer.push(samples, 5);

    float out[3];
    size_t retrieved = buffer.get(out, 3, false);

    REQUIRE(retrieved == 3);
    REQUIRE_THAT(out[0], WithinAbs(1000.0f / 32768.0f, 0.0001f));
    REQUIRE_THAT(out[2], WithinAbs(3000.0f / 32768.0f, 0.0001f));

    // Buffer should still have all 5 samples (clear_retrieved=false)
    REQUIRE(buffer.size() == 5);
}

TEST_CASE("AudioBuffer: get with clear_retrieved", "[audio][buffer]") {
    AudioBuffer buffer(1.0f, 16000);

    int16_t samples[] = {1000, 2000, 3000, 4000, 5000};
    buffer.push(samples, 5);

    float out[3];
    size_t retrieved = buffer.get(out, 3, true);

    REQUIRE(retrieved == 3);
    // Buffer should now have only 2 samples
    REQUIRE(buffer.size() == 2);

    // Remaining samples should be the last two
    std::vector<float> remaining = buffer.getAll();
    REQUIRE(remaining.size() == 2);
    REQUIRE_THAT(remaining[0], WithinAbs(4000.0f / 32768.0f, 0.0001f));
    REQUIRE_THAT(remaining[1], WithinAbs(5000.0f / 32768.0f, 0.0001f));
}

// ============================================================================
// pushFloat Tests
// ============================================================================

TEST_CASE("AudioBuffer: pushFloat adds float samples directly", "[audio][buffer]") {
    AudioBuffer buffer(1.0f, 16000);

    float samples[] = {0.5f, -0.5f, 0.0f, 1.0f, -1.0f};
    buffer.pushFloat(samples, 5);

    std::vector<float> result = buffer.getAll();
    REQUIRE(result.size() == 5);
    REQUIRE_THAT(result[0], WithinAbs(0.5f, 0.0001f));
    REQUIRE_THAT(result[1], WithinAbs(-0.5f, 0.0001f));
    REQUIRE_THAT(result[4], WithinAbs(-1.0f, 0.0001f));
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST_CASE("AudioBuffer: concurrent push and getAll", "[audio][thread]") {
    AudioBuffer buffer(1.0f, 16000);
    std::atomic<bool> stop{false};
    std::atomic<int> push_count{0};
    std::atomic<int> read_count{0};

    // Writer thread
    std::thread writer([&]() {
        int16_t samples[] = {1000, 2000, 3000, 4000};
        while (!stop) {
            buffer.push(samples, 4);
            push_count++;
        }
    });

    // Reader thread
    std::thread reader([&]() {
        while (!stop) {
            std::vector<float> result = buffer.getAll();
            read_count++;
        }
    });

    // Let them run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop = true;

    writer.join();
    reader.join();

    // If we got here without crashing, the test passes
    REQUIRE(push_count > 0);
    REQUIRE(read_count > 0);
}

TEST_CASE("AudioBuffer: concurrent push and clear", "[audio][thread]") {
    AudioBuffer buffer(1.0f, 16000);
    std::atomic<bool> stop{false};
    std::atomic<int> operations{0};

    // Writer thread
    std::thread writer([&]() {
        int16_t samples[] = {1000, 2000, 3000, 4000};
        while (!stop) {
            buffer.push(samples, 4);
            operations++;
        }
    });

    // Clearer thread
    std::thread clearer([&]() {
        while (!stop) {
            buffer.clear();
            operations++;
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });

    // Let them run
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop = true;

    writer.join();
    clearer.join();

    REQUIRE(operations > 0);
}

TEST_CASE("AudioBuffer: concurrent push and getLastMs", "[audio][thread]") {
    AudioBuffer buffer(1.0f, 16000);
    std::atomic<bool> stop{false};
    std::atomic<int> operations{0};

    // Writer thread
    std::thread writer([&]() {
        int16_t samples[160];  // 10ms of audio
        for (int i = 0; i < 160; i++) samples[i] = i;
        while (!stop) {
            buffer.push(samples, 160);
            operations++;
        }
    });

    // Reader thread
    std::thread reader([&]() {
        while (!stop) {
            std::vector<float> result = buffer.getLastMs(30);
            operations++;
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop = true;

    writer.join();
    reader.join();

    REQUIRE(operations > 0);
}
