#include "whisper_server.hpp"

#include <App.h>  // uWebSockets

#include <iostream>
#include <string>
#include <csignal>
#include <cstring>

// Global server pointer for signal handling
static WhisperServer* g_server = nullptr;

void signalHandler(int signum) {
    std::cout << "\n[whisper-server] Received signal " << signum << ", shutting down..." << std::endl;
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

            .open = [&server, &config](auto* ws) {
                // Generate session ID
                static int session_counter = 0;
                std::string session_id = "session_" + std::to_string(++session_counter);

                auto* data = ws->getUserData();
                data->session_id = session_id;

                std::cout << "[whisper-server] WebSocket connected: " << session_id << std::endl;

                // Create session with send callback
                // Note: we capture ws but need to be careful about lifetime
                auto send_fn = [ws](const std::string& msg) {
                    ws->send(msg, uWS::OpCode::TEXT);
                };

                // Access server's createSession through a helper
                // Since WhisperServer::createSession is private, we need to expose it
                // For now, we'll do this inline

                // Actually, let's make a simpler approach: store the WebSocket pointer
                // and have the server use it directly. But uWS doesn't support that well.

                // Better approach: use a message queue per session that the WS reads from
                // For MVP, we'll use a direct send which is safe if we're careful

                // Create the session
                auto session = server.createSession(session_id, send_fn);

                if (!session) {
                    ws->send(R"({"type":"error","message":"No available contexts, try again later"})",
                             uWS::OpCode::TEXT);
                    ws->close();
                    return;
                }

                // Send ready message
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
                std::cout << "[whisper-server] Listening on port " << config.port << std::endl;
            } else {
                std::cerr << "[whisper-server] Failed to listen on port " << config.port << std::endl;
            }
        })
        .run();

    std::cout << "[whisper-server] Server stopped" << std::endl;
    return 0;
}
