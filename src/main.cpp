#include "whisper_server.hpp"

#include <App.h>  // uWebSockets

#include <iostream>
#include <string>
#include <csignal>
#include <cstring>

// Global pointers for signal handling
static WhisperServer* g_server = nullptr;
static us_listen_socket_t* g_listen_socket = nullptr;
static uWS::Loop* g_loop = nullptr;

void signalHandler(int signum) {
    std::cout << "\n[whisper-server] Received signal " << signum << ", shutting down..." << std::endl;

    // Close the listen socket to stop accepting new connections
    // and cause the event loop to exit
    if (g_loop && g_listen_socket) {
        g_loop->defer([]{
            if (g_listen_socket) {
                us_listen_socket_close(0, g_listen_socket);
                g_listen_socket = nullptr;
            }
        });
    }

    if (g_server) {
        g_server->stop();
    }
}

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n\n"
              << "Options:\n"
              << "  -m, --model PATH      Path to whisper model (default: models/ggml-base.en.bin)\n"
              << "  -p, --port PORT       Port to listen on (default: 9090)\n"
              << "  -c, --contexts N      Number of parallel contexts (default: 2)\n"
              << "  -t, --threads N       Threads per inference (default: 4)\n"
              << "  -l, --language LANG   Language code (default: en)\n"
              << "      --step MS         Inference step interval in ms (default: 500)\n"
              << "      --length MS       Audio context length in ms (default: 5000)\n"
              << "      --keep MS         Audio overlap in ms (default: 200)\n"
              << "      --no-gpu          Disable GPU acceleration\n"
              << "      --translate       Translate to English\n"
              << "      --vad-model PATH  Path to VAD model (enables VAD)\n"
              << "      --vad-threshold N Speech probability threshold 0.0-1.0 (default: 0.5)\n"
              << "      --vad-silence MS  Silence duration to trigger final (default: 500)\n"
              << "  -h, --help            Show this help\n"
              << std::endl;
}

bool parseArgs(int argc, char** argv, ServerConfig& config) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return false;
        }
        else if ((arg == "-m" || arg == "--model") && i + 1 < argc) {
            config.model_path = argv[++i];
        }
        else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            config.port = std::stoi(argv[++i]);
        }
        else if ((arg == "-c" || arg == "--contexts") && i + 1 < argc) {
            config.n_contexts = std::stoi(argv[++i]);
        }
        else if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
            config.n_threads = std::stoi(argv[++i]);
        }
        else if ((arg == "-l" || arg == "--language") && i + 1 < argc) {
            config.language = argv[++i];
        }
        else if (arg == "--step" && i + 1 < argc) {
            config.step_ms = std::stoi(argv[++i]);
        }
        else if (arg == "--length" && i + 1 < argc) {
            config.length_ms = std::stoi(argv[++i]);
        }
        else if (arg == "--keep" && i + 1 < argc) {
            config.keep_ms = std::stoi(argv[++i]);
        }
        else if (arg == "--no-gpu") {
            config.use_gpu = false;
        }
        else if (arg == "--translate") {
            config.translate = true;
        }
        else if (arg == "--vad-model" && i + 1 < argc) {
            config.vad_model_path = argv[++i];
        }
        else if (arg == "--vad-threshold" && i + 1 < argc) {
            config.vad_threshold = std::stof(argv[++i]);
        }
        else if (arg == "--vad-silence" && i + 1 < argc) {
            config.silence_trigger_ms = std::stoi(argv[++i]);
        }
        else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            printUsage(argv[0]);
            return false;
        }
    }
    return true;
}

// Per-socket user data
struct PerSocketData {
    std::string session_id;
};

int main(int argc, char** argv) {
    ServerConfig config;

    if (!parseArgs(argc, argv, config)) {
        return 1;
    }

    // Create and initialize server
    WhisperServer server(config);
    g_server = &server;

    // Set up signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    if (!server.init()) {
        std::cerr << "[whisper-server] Failed to initialize server" << std::endl;
        return 1;
    }

    // Start inference thread
    server.run();

    // Create uWebSockets app
    uWS::App()
        .ws<PerSocketData>("/*", {
            // Settings
            .compression = uWS::DISABLED,
            .maxPayloadLength = 16 * 1024 * 1024,  // 16MB max message
            .idleTimeout = 120,
            .maxBackpressure = 1 * 1024 * 1024,    // 1MB backpressure

            // Handlers
            .upgrade = [](auto* res, auto* req, auto* context) {
                // Accept all upgrades
                res->template upgrade<PerSocketData>(
                    { .session_id = "" },
                    req->getHeader("sec-websocket-key"),
                    req->getHeader("sec-websocket-protocol"),
                    req->getHeader("sec-websocket-extensions"),
                    context
                );
            },

            .open = [&server](auto* ws) {
                // Generate session ID
                static int session_counter = 0;
                std::string session_id = "session_" + std::to_string(++session_counter);

                auto* data = ws->getUserData();
                data->session_id = session_id;

                std::cout << "[whisper-server] WebSocket connected: " << session_id << std::endl;

                // Create the session (no send callback - we use message queue now)
                auto session = server.createSession(session_id);

                if (!session) {
                    ws->send(R"({"type":"error","message":"No available contexts, try again later"})",
                             uWS::OpCode::TEXT);
                    ws->close();
                    return;
                }

                // Send ready message (safe: we're on the uWS event loop thread)
                ws->send(server.makeReadyMessage(), uWS::OpCode::TEXT);
            },

            .message = [&server](auto* ws, std::string_view message, uWS::OpCode opCode) {
                auto* data = ws->getUserData();

                if (opCode == uWS::OpCode::BINARY) {
                    // Binary message = audio data (int16 PCM)
                    const int16_t* audio_data = reinterpret_cast<const int16_t*>(message.data());
                    size_t sample_count = message.size() / sizeof(int16_t);

                    server.onAudioReceived(data->session_id, audio_data, sample_count);
                }
                else if (opCode == uWS::OpCode::TEXT) {
                    // Text message = control command (JSON)
                    // For now, we don't handle any text commands
                    std::cout << "[whisper-server] Received text message: " << message << std::endl;
                }

                // Drain any pending messages from the inference thread
                // This is safe because we're on the uWS event loop thread
                auto pending = server.drainSessionMessages(data->session_id);
                for (const auto& msg : pending) {
                    ws->send(msg, uWS::OpCode::TEXT);
                }
            },

            .close = [&server](auto* ws, int code, std::string_view message) {
                auto* data = ws->getUserData();
                std::cout << "[whisper-server] WebSocket disconnected: " << data->session_id
                          << " (code=" << code << ")" << std::endl;

                server.destroySession(data->session_id);
            }
        })
        .listen(config.port, [&config](auto* listen_socket) {
            if (listen_socket) {
                g_listen_socket = listen_socket;
                g_loop = uWS::Loop::get();
                std::cout << "[whisper-server] Listening on port " << config.port << std::endl;
            } else {
                std::cerr << "[whisper-server] Failed to listen on port " << config.port << std::endl;
            }
        })
        .run();

    std::cout << "[whisper-server] Server stopped" << std::endl;
    return 0;
}
