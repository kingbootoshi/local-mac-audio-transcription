#include "whisper_server.hpp"
#include "json.hpp"

#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>

using json = nlohmann::json;

// Generate a random session ID
static std::string generateSessionId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static const char* hex = "0123456789abcdef";

    std::string id;
    id.reserve(16);
    for (int i = 0; i < 16; ++i) {
        id += hex[dis(gen)];
    }
    return id;
}

WhisperServer::WhisperServer(const ServerConfig& config)
    : config_(config) {
}

WhisperServer::~WhisperServer() {
    stop();

    // Free all whisper contexts
    for (auto& slot : context_pool_) {
        if (slot && slot->ctx) {
            whisper_free(slot->ctx);
            slot->ctx = nullptr;
        }
    }
}

bool WhisperServer::init() {
    std::cout << "[whisper-server] Initializing with " << config_.n_contexts
              << " context(s)..." << std::endl;
    std::cout << "[whisper-server] Model: " << config_.model_path << std::endl;
    std::cout << "[whisper-server] GPU: " << (config_.use_gpu ? "enabled" : "disabled") << std::endl;

    // Load the backend
    ggml_backend_load_all();

    // Initialize context pool
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = config_.use_gpu;
    cparams.flash_attn = config_.flash_attn;

    for (int i = 0; i < config_.n_contexts; ++i) {
        std::cout << "[whisper-server] Loading context " << (i + 1) << "/" << config_.n_contexts << "..." << std::endl;

        auto slot = std::make_unique<ContextSlot>();
        slot->ctx = whisper_init_from_file_with_params(
            config_.model_path.c_str(), cparams
        );

        if (!slot->ctx) {
            std::cerr << "[whisper-server] Failed to load model for context " << i << std::endl;
            return false;
        }

        slot->slot_id = i;
        slot->in_use = false;
        context_pool_.push_back(std::move(slot));
    }

    std::cout << "[whisper-server] All contexts loaded successfully" << std::endl;
    return true;
}

void WhisperServer::run() {
    running_ = true;

    // Start inference loop thread
    inference_thread_ = std::thread(&WhisperServer::inferenceLoop, this);

    std::cout << "[whisper-server] Server running on port " << config_.port << std::endl;
    std::cout << "[whisper-server] Inference: step=" << config_.step_ms << "ms, length="
              << config_.length_ms << "ms, keep=" << config_.keep_ms << "ms" << std::endl;
}

void WhisperServer::stop() {
    if (!running_) return;

    running_ = false;

    if (inference_thread_.joinable()) {
        inference_thread_.join();
    }

    // Clean up sessions
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [id, session] : sessions_) {
        session->active = false;
        if (session->context_slot) {
            releaseContext(session->context_slot);
        }
    }
    sessions_.clear();
}

ContextSlot* WhisperServer::acquireContext() {
    std::lock_guard<std::mutex> lock(context_pool_mutex_);
    for (auto& slot : context_pool_) {
        if (!slot->in_use) {
            slot->in_use = true;
            std::cout << "[whisper-server] Acquired context slot " << slot->slot_id << std::endl;
            return slot.get();
        }
    }
    return nullptr; // All contexts busy
}

void WhisperServer::releaseContext(ContextSlot* slot) {
    if (slot) {
        std::lock_guard<std::mutex> lock(context_pool_mutex_);
        std::cout << "[whisper-server] Released context slot " << slot->slot_id << std::endl;
        slot->in_use = false;
    }
}

std::shared_ptr<Session> WhisperServer::createSession(
    const std::string& id,
    std::function<void(const std::string&)> send_fn
) {
    // Try to acquire a context
    ContextSlot* slot = acquireContext();
    if (!slot) {
        std::cerr << "[whisper-server] No available contexts for new session" << std::endl;
        return nullptr;
    }

    auto session = std::make_shared<Session>();
    session->id = id;
    session->audio = std::make_unique<AudioBuffer>(30.0f, WHISPER_SAMPLE_RATE);
    session->context_slot = slot;
    session->send_message = std::move(send_fn);
    session->active = true;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[id] = session;
    }

    std::cout << "[whisper-server] Created session " << id << " on context " << slot->slot_id << std::endl;
    return session;
}

void WhisperServer::destroySession(const std::string& id) {
    std::shared_ptr<Session> session;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(id);
        if (it != sessions_.end()) {
            session = it->second;
            sessions_.erase(it);
        }
    }

    if (session) {
        session->active = false;

        // Wait for any ongoing inference to complete
        while (session->inference_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        releaseContext(session->context_slot);
        std::cout << "[whisper-server] Destroyed session " << id << std::endl;
    }
}

void WhisperServer::onAudioReceived(const std::string& session_id, const int16_t* data, size_t len) {
    std::shared_ptr<Session> session;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            session = it->second;
        }
    }

    if (session && session->active) {
        session->audio->push(data, len);
    }
}

void WhisperServer::inferenceLoop() {
    const int step_samples = (config_.step_ms * WHISPER_SAMPLE_RATE) / 1000;
    const int length_samples = (config_.length_ms * WHISPER_SAMPLE_RATE) / 1000;
    const int keep_samples = (config_.keep_ms * WHISPER_SAMPLE_RATE) / 1000;

    while (running_) {
        auto start_time = std::chrono::steady_clock::now();

        // Get snapshot of active sessions
        std::vector<std::shared_ptr<Session>> active_sessions;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            for (auto& [id, session] : sessions_) {
                if (session->active && !session->inference_running) {
                    // Check if we have enough audio
                    if (session->audio->hasMinDuration(config_.step_ms)) {
                        active_sessions.push_back(session);
                    }
                }
            }
        }

        // Run inference on each session (in their respective context)
        for (auto& session : active_sessions) {
            session->inference_running = true;
            runInference(session);
            session->inference_running = false;
        }

        // Sleep to maintain step interval
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        auto sleep_time = std::chrono::milliseconds(config_.step_ms) - elapsed;
        if (sleep_time > std::chrono::milliseconds(0)) {
            std::this_thread::sleep_for(sleep_time);
        }
    }
}

void WhisperServer::runInference(std::shared_ptr<Session> session) {
    if (!session || !session->context_slot || !session->context_slot->ctx) {
        return;
    }

    whisper_context* ctx = session->context_slot->ctx;

    const int n_samples_step = (config_.step_ms * WHISPER_SAMPLE_RATE) / 1000;
    const int n_samples_len = (config_.length_ms * WHISPER_SAMPLE_RATE) / 1000;
    const int n_samples_keep = (config_.keep_ms * WHISPER_SAMPLE_RATE) / 1000;

    // Get new audio
    std::vector<float> pcmf32_new = session->audio->getAll();
    session->audio->clear();

    if (pcmf32_new.empty()) {
        return;
    }

    // Build sliding window: [keep from old] + [new audio]
    std::vector<float> pcmf32;

    int n_samples_take = 0;
    if (!session->pcmf32_old.empty()) {
        n_samples_take = std::min(
            static_cast<int>(session->pcmf32_old.size()),
            std::max(0, n_samples_keep + n_samples_len - static_cast<int>(pcmf32_new.size()))
        );
    }

    pcmf32.resize(pcmf32_new.size() + n_samples_take);

    // Copy tail of old audio
    if (n_samples_take > 0) {
        size_t old_start = session->pcmf32_old.size() - n_samples_take;
        std::memcpy(pcmf32.data(), session->pcmf32_old.data() + old_start,
                    n_samples_take * sizeof(float));
    }

    // Copy new audio
    std::memcpy(pcmf32.data() + n_samples_take, pcmf32_new.data(),
                pcmf32_new.size() * sizeof(float));

    // Save for next iteration
    session->pcmf32_old = pcmf32;

    // Run whisper inference
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    wparams.print_progress = false;
    wparams.print_special = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.translate = config_.translate;
    wparams.single_segment = true;
    wparams.max_tokens = 0;
    wparams.language = config_.language.c_str();
    wparams.n_threads = config_.n_threads;
    wparams.no_context = true;
    wparams.no_timestamps = true;

    if (whisper_full(ctx, wparams, pcmf32.data(), pcmf32.size()) != 0) {
        std::cerr << "[whisper-server] Inference failed for session " << session->id << std::endl;
        return;
    }

    // Extract text from segments
    std::string text;
    const int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
        const char* segment_text = whisper_full_get_segment_text(ctx, i);
        if (segment_text) {
            text += segment_text;
        }
    }

    // Trim whitespace
    size_t start = text.find_first_not_of(" \t\n\r");
    size_t end = text.find_last_not_of(" \t\n\r");
    if (start != std::string::npos && end != std::string::npos) {
        text = text.substr(start, end - start + 1);
    } else {
        text.clear();
    }

    // Send result if text changed
    if (!text.empty() && text != session->last_text) {
        // Determine if this is a partial or final result
        // For now, send as partial (final detection would require silence detection)
        session->send_message(makePartialMessage(text));
        session->last_text = text;
    }
}

std::string WhisperServer::makeReadyMessage() {
    json msg;
    msg["type"] = "ready";
    msg["model"] = config_.model_path;
    msg["contexts"] = config_.n_contexts;
    return msg.dump();
}

std::string WhisperServer::makePartialMessage(const std::string& text) {
    json msg;
    msg["type"] = "partial";
    msg["text"] = text;
    return msg.dump();
}

std::string WhisperServer::makeFinalMessage(const std::string& text) {
    json msg;
    msg["type"] = "final";
    msg["text"] = text;
    return msg.dump();
}

std::string WhisperServer::makeErrorMessage(const std::string& error) {
    json msg;
    msg["type"] = "error";
    msg["message"] = error;
    return msg.dump();
}
