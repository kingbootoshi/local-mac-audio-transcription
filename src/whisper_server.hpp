#ifndef WHISPER_SERVER_HPP
#define WHISPER_SERVER_HPP

#include "audio_buffer.hpp"
#include "whisper.h"

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>

// Server configuration
struct ServerConfig {
    std::string model_path = "models/ggml-base.en.bin";
    std::string language = "en";
    int port = 9090;
    int n_contexts = 2;       // Number of parallel whisper contexts
    int n_threads = 4;        // Threads per inference
    int step_ms = 500;        // Run inference every N ms
    int length_ms = 5000;     // Audio context window
    int keep_ms = 200;        // Overlap between windows
    bool use_gpu = true;
    bool flash_attn = true;
    bool translate = false;
};

// Forward declarations
struct Session;
class WhisperServer;

// Context slot in the pool
struct ContextSlot {
    whisper_context* ctx = nullptr;
    bool in_use = false;  // Changed from atomic - we'll protect with mutex
    int slot_id = 0;
};

// Per-connection session
struct Session {
    std::string id;
    std::unique_ptr<AudioBuffer> audio;
    std::vector<float> pcmf32_old;    // Previous audio for overlap
    std::string last_text;             // For detecting changes
    ContextSlot* context_slot = nullptr;
    std::atomic<bool> active{true};
    std::atomic<bool> inference_running{false};

    // Callback to send message to client
    std::function<void(const std::string&)> send_message;
};

// Main server class
class WhisperServer {
public:
    explicit WhisperServer(const ServerConfig& config);
    ~WhisperServer();

    // Initialize contexts and start server
    bool init();

    // Run the server (blocking)
    void run();

    // Stop the server
    void stop();

    // Get server status
    bool isRunning() const { return running_.load(); }

    // === Public methods for WebSocket handlers ===

    // Session management
    std::shared_ptr<Session> createSession(
        const std::string& id,
        std::function<void(const std::string&)> send_fn
    );
    void destroySession(const std::string& id);

    // Audio processing
    void onAudioReceived(const std::string& session_id, const int16_t* data, size_t len);

    // JSON message helpers
    std::string makeReadyMessage();
    std::string makePartialMessage(const std::string& text);
    std::string makeFinalMessage(const std::string& text);
    std::string makeErrorMessage(const std::string& error);

private:
    ServerConfig config_;
    std::vector<std::unique_ptr<ContextSlot>> context_pool_;  // Use unique_ptr
    std::mutex context_pool_mutex_;  // Protect context pool access
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    std::mutex sessions_mutex_;

    std::atomic<bool> running_{false};
    std::thread inference_thread_;

    // Context pool management
    ContextSlot* acquireContext();
    void releaseContext(ContextSlot* slot);

    // Inference loop
    void inferenceLoop();
    void runInference(std::shared_ptr<Session> session);
};

#endif // WHISPER_SERVER_HPP
