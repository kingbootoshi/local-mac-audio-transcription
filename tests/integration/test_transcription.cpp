/**
 * Integration tests for Whisper transcription
 *
 * Tests the full transcription pipeline with actual whisper models.
 * Uses jfk.wav as a known audio fixture to verify output quality.
 *
 * REQUIRES: whisper model at ../whisper.cpp/models/ggml-base.en.bin
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "whisper_server.hpp"

#include <fstream>
#include <vector>
#include <cstring>
#include <iostream>

using Catch::Matchers::ContainsSubstring;

// Path to test fixture (relative to build directory)
static const char* JFK_WAV_PATH = "../tests/fixtures/jfk.wav";
static const char* MODEL_PATH = "../../whisper.cpp/models/ggml-base.en.bin";

/**
 * Load a WAV file and return float32 samples.
 * Handles WAV files with extra chunks (LIST, INFO, etc.).
 * Assumes 16-bit PCM, mono, 16kHz (whisper's expected format).
 */
std::vector<float> loadWav(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open WAV file: " + path);
    }

    // Read RIFF header (12 bytes)
    char riff_header[12];
    file.read(riff_header, 12);

    if (file.gcount() < 12) {
        throw std::runtime_error("Invalid WAV header");
    }

    // Verify RIFF header
    if (std::memcmp(riff_header, "RIFF", 4) != 0 || std::memcmp(riff_header + 8, "WAVE", 4) != 0) {
        throw std::runtime_error("Not a valid WAV file");
    }

    // Parse chunks to find fmt and data
    int16_t audio_format = 0;
    int16_t num_channels = 0;
    int32_t sample_rate = 0;
    int16_t bits_per_sample = 0;
    int32_t data_size = 0;
    bool found_fmt = false;
    bool found_data = false;

    while (!file.eof() && (!found_fmt || !found_data)) {
        char chunk_id[4];
        uint32_t chunk_size;

        file.read(chunk_id, 4);
        file.read(reinterpret_cast<char*>(&chunk_size), 4);

        if (file.gcount() < 4) break;

        if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
            // Read format chunk
            char fmt_data[16];
            file.read(fmt_data, std::min(chunk_size, 16u));

            audio_format = *reinterpret_cast<int16_t*>(fmt_data);
            num_channels = *reinterpret_cast<int16_t*>(fmt_data + 2);
            sample_rate = *reinterpret_cast<int32_t*>(fmt_data + 4);
            bits_per_sample = *reinterpret_cast<int16_t*>(fmt_data + 14);
            found_fmt = true;

            // Skip any remaining format data
            if (chunk_size > 16) {
                file.seekg(chunk_size - 16, std::ios::cur);
            }
        } else if (std::memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            found_data = true;
            // Don't skip - we'll read the data next
        } else {
            // Skip unknown chunks
            file.seekg(chunk_size, std::ios::cur);
        }
    }

    if (!found_fmt || !found_data) {
        throw std::runtime_error("WAV file missing fmt or data chunk");
    }

    // Verify format (PCM, mono, 16-bit)
    if (audio_format != 1) {
        throw std::runtime_error("WAV must be PCM format (got " + std::to_string(audio_format) + ")");
    }
    if (num_channels != 1) {
        throw std::runtime_error("WAV must be mono (got " + std::to_string(num_channels) + " channels)");
    }
    if (bits_per_sample != 16) {
        throw std::runtime_error("WAV must be 16-bit (got " + std::to_string(bits_per_sample) + "-bit)");
    }

    // Read audio data
    int num_samples = data_size / sizeof(int16_t);
    std::vector<int16_t> samples(num_samples);
    file.read(reinterpret_cast<char*>(samples.data()), data_size);

    // Convert to float32
    std::vector<float> result(num_samples);
    for (int i = 0; i < num_samples; i++) {
        result[i] = static_cast<float>(samples[i]) / 32768.0f;
    }

    std::cout << "[loadWav] Loaded " << num_samples << " samples from " << path
              << " (sample_rate=" << sample_rate << ")" << std::endl;

    return result;
}

/**
 * Check if test dependencies are available
 */
bool testDependenciesAvailable() {
    std::ifstream model(MODEL_PATH);
    std::ifstream wav(JFK_WAV_PATH);
    return model.is_open() && wav.is_open();
}

// ============================================================================
// Test Cases
// ============================================================================

TEST_CASE("Integration: Load WAV file", "[integration][wav]") {
    std::ifstream f(JFK_WAV_PATH);
    if (!f.is_open()) {
        WARN("Skipping test: jfk.wav not found at " << JFK_WAV_PATH);
        SUCCEED("Test skipped - fixture not found");
        return;
    }

    std::vector<float> samples = loadWav(JFK_WAV_PATH);

    // JFK clip is about 11 seconds at 16kHz = ~176000 samples
    REQUIRE(samples.size() > 100000);
    REQUIRE(samples.size() < 300000);

    // Values should be in normalized range
    for (const auto& s : samples) {
        REQUIRE(s >= -1.0f);
        REQUIRE(s <= 1.0f);
    }
}

TEST_CASE("Integration: Transcribe JFK clip contains expected keywords", "[integration][transcribe]") {
    if (!testDependenciesAvailable()) {
        WARN("Skipping test: model or fixture not found");
        SUCCEED("Test skipped - dependencies not found");
        return;
    }

    // Load audio
    std::vector<float> samples = loadWav(JFK_WAV_PATH);

    // Create minimal server config
    ServerConfig config;
    config.model_path = MODEL_PATH;
    config.n_contexts = 1;
    config.n_threads = 4;
    config.use_gpu = true;

    // Initialize whisper context directly (not full server)
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = config.use_gpu;
    cparams.flash_attn = true;

    whisper_context* ctx = whisper_init_from_file_with_params(config.model_path.c_str(), cparams);
    REQUIRE(ctx != nullptr);

    // Run inference
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_progress = false;
    wparams.print_special = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.single_segment = false;
    wparams.n_threads = config.n_threads;
    wparams.language = "en";

    int result = whisper_full(ctx, wparams, samples.data(), samples.size());
    REQUIRE(result == 0);

    // Extract text
    std::string text;
    int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; i++) {
        text += whisper_full_get_segment_text(ctx, i);
    }

    // Convert to lowercase for matching
    std::string lower_text;
    for (char c : text) {
        lower_text += std::tolower(c);
    }

    std::cout << "[Integration Test] Transcribed: " << text << std::endl;

    // Verify expected content
    REQUIRE_THAT(lower_text, ContainsSubstring("ask"));
    REQUIRE_THAT(lower_text, ContainsSubstring("country"));

    whisper_free(ctx);
}

TEST_CASE("Integration: Empty audio returns empty/blank result", "[integration][transcribe]") {
    std::ifstream f(MODEL_PATH);
    if (!f.is_open()) {
        WARN("Skipping test: model not found");
        SUCCEED("Test skipped - model not found");
        return;
    }

    // Create 1 second of silence
    std::vector<float> silence(16000, 0.0f);

    // Initialize context
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true;

    whisper_context* ctx = whisper_init_from_file_with_params(MODEL_PATH, cparams);
    REQUIRE(ctx != nullptr);

    // Run inference
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_progress = false;
    wparams.print_special = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.n_threads = 4;
    wparams.language = "en";

    int result = whisper_full(ctx, wparams, silence.data(), silence.size());
    REQUIRE(result == 0);

    // Extract text
    std::string text;
    int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; i++) {
        const char* segment_text = whisper_full_get_segment_text(ctx, i);
        if (segment_text) text += segment_text;
    }

    // Should be empty or contain only whitespace/blank markers
    // Note: Whisper sometimes outputs "[BLANK_AUDIO]" or similar
    bool is_essentially_empty = text.empty() ||
        text.find_first_not_of(" \t\n\r") == std::string::npos ||
        text.find("[BLANK") != std::string::npos;

    REQUIRE(is_essentially_empty);

    whisper_free(ctx);
}

TEST_CASE("Integration: Context initialization and cleanup", "[integration][context]") {
    std::ifstream f(MODEL_PATH);
    if (!f.is_open()) {
        WARN("Skipping test: model not found");
        SUCCEED("Test skipped - model not found");
        return;
    }

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true;

    // Create and destroy context multiple times
    for (int i = 0; i < 3; i++) {
        whisper_context* ctx = whisper_init_from_file_with_params(MODEL_PATH, cparams);
        REQUIRE(ctx != nullptr);
        whisper_free(ctx);
    }
}
