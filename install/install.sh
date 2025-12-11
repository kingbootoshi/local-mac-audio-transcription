#!/bin/bash
#
# Whisper Stream Server - Installation Script
# Installs the server as a launchd service on macOS
#
# Usage: sudo ./install.sh [options]
#
# Options:
#   --model PATH    Path to whisper model (default: downloads base.en)
#   --port PORT     Server port (default: 9090)
#   --contexts N    Number of parallel contexts (default: 2)
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Default values
MODEL_PATH=""
PORT="9090"
CONTEXTS="2"

# Installation paths
INSTALL_BIN="/usr/local/bin"
INSTALL_SHARE="/usr/local/share/whisper"
INSTALL_LOG="/usr/local/var/log"
PLIST_PATH="/Library/LaunchDaemons/com.whisper.stream-server.plist"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --model)
            MODEL_PATH="$2"
            shift 2
            ;;
        --port)
            PORT="$2"
            shift 2
            ;;
        --contexts)
            CONTEXTS="$2"
            shift 2
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Check for root
if [[ $EUID -ne 0 ]]; then
    echo -e "${RED}Error: This script must be run as root (use sudo)${NC}"
    exit 1
fi

echo -e "${GREEN}=== Whisper Stream Server Installer ===${NC}"
echo ""

# Find the server directory (assuming script is in install/)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SERVER_DIR="$PROJECT_DIR/server"

# Check if binary exists
BINARY_PATH="$SERVER_DIR/build/whisper-stream-server"
if [[ ! -f "$BINARY_PATH" ]]; then
    echo -e "${YELLOW}Binary not found. Building...${NC}"
    cd "$SERVER_DIR"

    if [[ ! -f "scripts/build.sh" ]]; then
        echo -e "${RED}Error: build.sh not found. Please build manually first.${NC}"
        exit 1
    fi

    ./scripts/build.sh

    if [[ ! -f "$BINARY_PATH" ]]; then
        echo -e "${RED}Error: Build failed.${NC}"
        exit 1
    fi
fi

echo -e "${GREEN}[1/5] Creating directories...${NC}"
mkdir -p "$INSTALL_BIN"
mkdir -p "$INSTALL_SHARE"
mkdir -p "$INSTALL_LOG"

echo -e "${GREEN}[2/5] Installing binary...${NC}"
cp "$BINARY_PATH" "$INSTALL_BIN/whisper-stream-server"
chmod 755 "$INSTALL_BIN/whisper-stream-server"
echo "  Installed: $INSTALL_BIN/whisper-stream-server"

echo -e "${GREEN}[3/5] Installing model...${NC}"
if [[ -n "$MODEL_PATH" ]]; then
    # User provided model
    if [[ ! -f "$MODEL_PATH" ]]; then
        echo -e "${RED}Error: Model not found at $MODEL_PATH${NC}"
        exit 1
    fi
    cp "$MODEL_PATH" "$INSTALL_SHARE/ggml-base.en.bin"
else
    # Check for existing model
    WHISPER_CPP_DIR="$PROJECT_DIR/../whisper.cpp"
    DEFAULT_MODEL="$WHISPER_CPP_DIR/models/ggml-base.en.bin"

    if [[ -f "$DEFAULT_MODEL" ]]; then
        cp "$DEFAULT_MODEL" "$INSTALL_SHARE/ggml-base.en.bin"
        echo "  Copied from: $DEFAULT_MODEL"
    elif [[ -f "$INSTALL_SHARE/ggml-base.en.bin" ]]; then
        echo "  Using existing model at $INSTALL_SHARE/ggml-base.en.bin"
    else
        echo -e "${YELLOW}  Downloading base.en model...${NC}"
        curl -L "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin" \
            -o "$INSTALL_SHARE/ggml-base.en.bin"
    fi
fi
echo "  Model: $INSTALL_SHARE/ggml-base.en.bin"

echo -e "${GREEN}[4/5] Installing launchd plist...${NC}"

# Generate plist with custom settings
cat > "$PLIST_PATH" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.whisper.stream-server</string>

    <key>ProgramArguments</key>
    <array>
        <string>/usr/local/bin/whisper-stream-server</string>
        <string>--model</string>
        <string>/usr/local/share/whisper/ggml-base.en.bin</string>
        <string>--port</string>
        <string>${PORT}</string>
        <string>--contexts</string>
        <string>${CONTEXTS}</string>
        <string>--threads</string>
        <string>4</string>
    </array>

    <key>RunAtLoad</key>
    <true/>

    <key>KeepAlive</key>
    <dict>
        <key>SuccessfulExit</key>
        <false/>
    </dict>

    <key>ThrottleInterval</key>
    <integer>10</integer>

    <key>WorkingDirectory</key>
    <string>/usr/local/share/whisper</string>

    <key>StandardOutPath</key>
    <string>/usr/local/var/log/whisper-stream-server.log</string>

    <key>StandardErrorPath</key>
    <string>/usr/local/var/log/whisper-stream-server.error.log</string>

    <key>EnvironmentVariables</key>
    <dict>
        <key>PATH</key>
        <string>/usr/local/bin:/usr/bin:/bin</string>
    </dict>

    <key>Nice</key>
    <integer>-5</integer>
</dict>
</plist>
EOF

chmod 644 "$PLIST_PATH"
echo "  Installed: $PLIST_PATH"

echo -e "${GREEN}[5/5] Starting service...${NC}"

# Unload if already loaded
launchctl unload "$PLIST_PATH" 2>/dev/null || true

# Load the service
launchctl load "$PLIST_PATH"

# Wait a moment for startup
sleep 2

# Check if running
if launchctl list | grep -q "com.whisper.stream-server"; then
    echo -e "${GREEN}  Service started successfully!${NC}"
else
    echo -e "${YELLOW}  Service may not have started. Check logs:${NC}"
    echo "    tail -f $INSTALL_LOG/whisper-stream-server.log"
    echo "    tail -f $INSTALL_LOG/whisper-stream-server.error.log"
fi

echo ""
echo -e "${GREEN}=== Installation Complete ===${NC}"
echo ""
echo "Service Details:"
echo "  Binary:  $INSTALL_BIN/whisper-stream-server"
echo "  Model:   $INSTALL_SHARE/ggml-base.en.bin"
echo "  Port:    $PORT"
echo "  Contexts: $CONTEXTS"
echo "  Logs:    $INSTALL_LOG/whisper-stream-server.log"
echo ""
echo "Commands:"
echo "  Status:  sudo launchctl list | grep whisper"
echo "  Stop:    sudo launchctl unload $PLIST_PATH"
echo "  Start:   sudo launchctl load $PLIST_PATH"
echo "  Logs:    tail -f $INSTALL_LOG/whisper-stream-server.log"
echo ""
echo "Test with:"
echo "  wscat -c ws://localhost:$PORT"
echo ""
