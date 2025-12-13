#include "whisper_server.hpp"
#include "json.hpp"

#include <App.h>  // For uWS::Loop and WebSocket types

#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>

using json = nlohmann::json;

// Forward declaration of per-socket data (matches main.cpp)
struct PerSocketData {
    std::string session_id;
};

// Callback to disable whisper internal logging (for VAD spam)
static void whisper_log_disable(enum ggml_log_level, const char*, void*) {}

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

    // Free VAD context
    if (vad_ctx_) {
        whisper_vad_free(vad_ctx_);
        vad_ctx_ = nullptr;
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

    // Load VAD model (optional)
    if (!config_.vad_model_path.empty()) {
        std::cout << "[whisper-server] Loading VAD model: " << config_.vad_model_path << std::endl;

        whisper_vad_context_params vad_params = whisper_vad_default_context_params();
        vad_params.n_threads = 2;
        vad_params.use_gpu = false;  // VAD is lightweight, CPU is fine

        vad_ctx_ = whisper_vad_init_from_file_with_params(
            config_.vad_model_path.c_str(), vad_params);

        if (!vad_ctx_) {
            std::cerr << "[whisper-server] Failed to load VAD model" << std::endl;
            return false;
        }
        std::cout << "[whisper-server] VAD enabled (threshold=" << config_.vad_threshold
                  << ", silence=" << config_.silence_trigger_ms << "ms)" << std::endl;
    }

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

std::shared_ptr<Session> WhisperServer::createSession(const std::string& id) {
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
    session->active = true;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[id] = session;
    }

    std::cout << "[whisper-server] Created session " << id << " on context " << slot->slot_id << std::endl;
    return session;
}

std::deque<std::string> WhisperServer::drainSessionMessages(const std::string& session_id) {
    std::shared_ptr<Session> session;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return {}; // Session doesn't exist
        }
        session = it->second;
    }

    // Drain messages from the session's queue
    return session->drainMessages();
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
    using namespace std::chrono;

    const int vad_interval_ms = config_.vad_check_ms;      // 30ms
    const int whisper_interval_ms = config_.step_ms;       // 500ms

    auto last_vad_time = steady_clock::now();
    auto last_whisper_time = steady_clock::now();

    while (running_) {
        auto now = steady_clock::now();
        int64_t now_ms = duration_cast<milliseconds>(now.time_since_epoch()).count();

        // Get snapshot of active sessions
        std::vector<std::shared_ptr<Session>> sessions;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            for (auto& [id, session] : sessions_) {
                if (session->active) {
                    sessions.push_back(session);
                }
            }
        }

        // === VAD CHECK (every 30ms) ===
        auto vad_elapsed = duration_cast<milliseconds>(now - last_vad_time).count();
        if (vad_ctx_ && vad_elapsed >= vad_interval_ms) {
            for (auto& session : sessions) {
                updateVADState(session, now_ms);
            }
            last_vad_time = now;
        }

        // === WHISPER INFERENCE (every 500ms) ===
        auto whisper_elapsed = duration_cast<milliseconds>(now - last_whisper_time).count();
        if (whisper_elapsed >= whisper_interval_ms) {
            for (auto& session : sessions) {
                // If VAD disabled, always run inference (original behavior)
                if (!vad_ctx_) {
                    if (!session->inference_running && session->audio->hasMinDuration(config_.step_ms)) {
                        session->inference_running = true;
                        runInference(session);
                        session->inference_running = false;
                    }
                }
                // If VAD enabled, only run when SPEAKING
                else if (session->speech_state == SpeechState::SPEAKING) {
                    if (!session->inference_running) {
                        session->inference_running = true;
                        runInference(session);
                        session->inference_running = false;
                    }
                }
                // Handle ENDING state - emit final
                else if (session->speech_state == SpeechState::ENDING) {
                    emitFinal(session);
                }
            }
            last_whisper_time = now;
        }

        // Sleep briefly to avoid busy-wait
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

    // Enqueue result if text changed
    // Messages are flushed via event-driven callback (notifySessionHasMessages)
    if (!text.empty() && text != session->last_text) {
        session->enqueueMessage(makePartialMessage(text));
        notifySessionHasMessages(session->id);
        session->pending_text = text;  // Save for potential final
        session->last_text = text;
    }
}

// === VAD Methods ===

float WhisperServer::detectSpeechProb(const float* samples, int n_samples) {
    if (!vad_ctx_ || n_samples == 0) return 0.0f;

    std::lock_guard<std::mutex> lock(vad_mutex_);

    // Disable whisper internal logging during VAD (too verbose)
    whisper_log_set(whisper_log_disable, nullptr);

    bool success = whisper_vad_detect_speech(vad_ctx_, samples, n_samples);

    // Re-enable logging
    whisper_log_set(nullptr, nullptr);

    if (!success) return 0.0f;

    int n_probs = whisper_vad_n_probs(vad_ctx_);
    if (n_probs == 0) return 0.0f;

    const float* probs = whisper_vad_probs(vad_ctx_);
    return probs[n_probs - 1];
}

void WhisperServer::updateVADState(std::shared_ptr<Session> session, int64_t now_ms) {
    // Get last 30ms of audio for VAD
    std::vector<float> recent_audio = session->audio->getLastMs(config_.vad_check_ms);
    if (recent_audio.empty()) return;

    float speech_prob = detectSpeechProb(recent_audio.data(), recent_audio.size());
    bool is_speech = speech_prob > config_.vad_threshold;

    switch (session->speech_state) {
        case SpeechState::IDLE:
            if (is_speech) {
                session->speech_state = SpeechState::SPEAKING;
                session->speech_start_ms = now_ms;
                session->last_speech_ms = now_ms;
                session->pending_text.clear();
                std::cout << "[VAD:" << session->id << "] === SPEECH STARTED ===" << std::endl;
            }
            break;

        case SpeechState::SPEAKING:
            if (is_speech) {
                session->last_speech_ms = now_ms;
            } else {
                int silence_ms = now_ms - session->last_speech_ms;
                if (silence_ms >= config_.silence_trigger_ms) {
                    int speech_duration = now_ms - session->speech_start_ms;
                    float audio_duration_ms = (session->pcmf32_old.size() * 1000.0f) / WHISPER_SAMPLE_RATE;

                    if (speech_duration >= config_.min_speech_ms) {
                        session->speech_state = SpeechState::ENDING;
                        std::cout << "[VAD:" << session->id << "] === SPEECH ENDED ===" << std::endl;
                        std::cout << "[VAD:" << session->id << "]   Speech duration: " << speech_duration << "ms" << std::endl;
                        std::cout << "[VAD:" << session->id << "]   Audio buffer: " << audio_duration_ms << "ms" << std::endl;
                        std::cout << "[VAD:" << session->id << "]   Last partial: \"" << session->pending_text << "\"" << std::endl;
                    } else {
                        // Too short, ignore
                        session->speech_state = SpeechState::IDLE;
                        std::cout << "[VAD:" << session->id << "] Ignored short utterance (" << speech_duration << "ms)" << std::endl;
                    }
                }
            }
            break;

        case SpeechState::ENDING:
            // Check if user started speaking again before final was emitted
            if (is_speech) {
                session->speech_state = SpeechState::SPEAKING;
                session->last_speech_ms = now_ms;
                std::cout << "[VAD:" << session->id << "] Speech resumed (user interrupted)" << std::endl;
            }
            break;
    }
}

void WhisperServer::emitFinal(std::shared_ptr<Session> session) {
    if (session->speech_state != SpeechState::ENDING) return;

    std::string final_text;

    // Use accumulated audio from pcmf32_old (runInference clears audio buffer)
    std::vector<float> pcmf32 = session->pcmf32_old;
    float duration_ms = (pcmf32.size() * 1000.0f) / WHISPER_SAMPLE_RATE;

    std::cout << "[VAD:" << session->id << "] Running final inference..." << std::endl;
    std::cout << "[VAD:" << session->id << "]   Audio samples: " << pcmf32.size()
              << " (" << duration_ms << "ms)" << std::endl;

    if (!pcmf32.empty() && session->context_slot && session->context_slot->ctx) {
        whisper_context* ctx = session->context_slot->ctx;
        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.print_progress = false;
        wparams.print_special = false;
        wparams.print_realtime = false;
        wparams.print_timestamps = false;
        wparams.translate = config_.translate;
        wparams.single_segment = false;
        wparams.language = config_.language.c_str();
        wparams.n_threads = config_.n_threads;

        if (whisper_full(ctx, wparams, pcmf32.data(), pcmf32.size()) == 0) {
            for (int i = 0; i < whisper_full_n_segments(ctx); ++i) {
                const char* seg = whisper_full_get_segment_text(ctx, i);
                if (seg) final_text += seg;
            }
            // Trim
            size_t start = final_text.find_first_not_of(" \t\n\r");
            size_t end = final_text.find_last_not_of(" \t\n\r");
            if (start != std::string::npos && end != std::string::npos) {
                final_text = final_text.substr(start, end - start + 1);
            } else {
                final_text.clear();
            }
        }
    }

    if (!final_text.empty()) {
        session->enqueueMessage(makeFinalMessage(final_text));
        notifySessionHasMessages(session->id);
        std::cout << "[VAD:" << session->id << "] === FINAL TRANSCRIPT ===" << std::endl;
        std::cout << "[VAD:" << session->id << "]   \"" << final_text << "\"" << std::endl;
    } else {
        std::cout << "[VAD:" << session->id << "] Final inference returned empty/blank" << std::endl;
    }

    // Reset state
    session->speech_state = SpeechState::IDLE;
    session->pending_text.clear();
    session->pcmf32_old.clear();
    session->last_text.clear();
    session->audio->clear();
}

// === Event Loop Integration ===

void WhisperServer::setEventLoop(void* loop) {
    loop_ = loop;
}

void WhisperServer::attachWebSocket(const std::string& session_id, void* ws_handle) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second->ws_handle = ws_handle;
    }
}

void WhisperServer::detachWebSocket(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second->ws_handle = nullptr;
        it->second->flush_pending.store(false);
    }
}

void WhisperServer::notifySessionHasMessages(const std::string& session_id) {
    if (!loop_) return;

    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end() || !it->second->active) return;
        session = it->second;
    }

    // If a flush is already pending, don't schedule another
    if (session->flush_pending.exchange(true)) {
        return;
    }

    // Schedule flush on the event loop thread
    auto* uws_loop = static_cast<uWS::Loop*>(loop_);
    uws_loop->defer([this, session_id]() {
        this->flushSessionMessagesOnEventLoop(session_id);
    });
}

void WhisperServer::flushSessionMessagesOnEventLoop(const std::string& session_id) {
    std::shared_ptr<Session> session;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return;
        session = it->second;
    }

    // Reset flush_pending for future messages
    session->flush_pending.store(false);

    // If socket is gone, discard messages
    if (!session->ws_handle) {
        session->drainMessages();
        return;
    }

    // Cast and send
    auto* ws = static_cast<uWS::WebSocket<false, true, PerSocketData>*>(session->ws_handle);

    std::deque<std::string> pending = session->drainMessages();
    for (const auto& msg : pending) {
        ws->send(msg, uWS::OpCode::TEXT);
    }
}

// === JSON Message Helpers ===

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
