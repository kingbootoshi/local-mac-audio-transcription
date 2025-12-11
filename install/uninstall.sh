#!/bin/bash
#
# Whisper Stream Server - Uninstallation Script
# Removes the server and launchd service from macOS
#
# Usage: sudo ./uninstall.sh [options]
#
# Options:
#   --keep-model    Don't remove the whisper model file
#   --keep-logs     Don't remove log files
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Options
KEEP_MODEL=false
KEEP_LOGS=false

# Installation paths
INSTALL_BIN="/usr/local/bin"
INSTALL_SHARE="/usr/local/share/whisper"
INSTALL_LOG="/usr/local/var/log"
PLIST_PATH="/Library/LaunchDaemons/com.whisper.stream-server.plist"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --keep-model)
            KEEP_MODEL=true
            shift
            ;;
        --keep-logs)
            KEEP_LOGS=true
            shift
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

echo -e "${GREEN}=== Whisper Stream Server Uninstaller ===${NC}"
echo ""

echo -e "${GREEN}[1/4] Stopping service...${NC}"
if [[ -f "$PLIST_PATH" ]]; then
    launchctl unload "$PLIST_PATH" 2>/dev/null || true
    echo "  Service stopped"
else
    echo "  Service not installed (plist not found)"
fi

echo -e "${GREEN}[2/4] Removing launchd plist...${NC}"
if [[ -f "$PLIST_PATH" ]]; then
    rm -f "$PLIST_PATH"
    echo "  Removed: $PLIST_PATH"
else
    echo "  Already removed"
fi

echo -e "${GREEN}[3/4] Removing binary...${NC}"
BINARY_PATH="$INSTALL_BIN/whisper-stream-server"
if [[ -f "$BINARY_PATH" ]]; then
    rm -f "$BINARY_PATH"
    echo "  Removed: $BINARY_PATH"
else
    echo "  Already removed"
fi

echo -e "${GREEN}[4/4] Cleaning up...${NC}"

# Remove model (unless --keep-model)
if [[ "$KEEP_MODEL" == false ]]; then
    if [[ -f "$INSTALL_SHARE/ggml-base.en.bin" ]]; then
        rm -f "$INSTALL_SHARE/ggml-base.en.bin"
        echo "  Removed model: $INSTALL_SHARE/ggml-base.en.bin"
    fi

    # Remove share directory if empty
    if [[ -d "$INSTALL_SHARE" ]]; then
        rmdir "$INSTALL_SHARE" 2>/dev/null && echo "  Removed directory: $INSTALL_SHARE" || true
    fi
else
    echo "  Kept model: $INSTALL_SHARE/ggml-base.en.bin"
fi

# Remove logs (unless --keep-logs)
if [[ "$KEEP_LOGS" == false ]]; then
    if [[ -f "$INSTALL_LOG/whisper-stream-server.log" ]]; then
        rm -f "$INSTALL_LOG/whisper-stream-server.log"
        echo "  Removed: $INSTALL_LOG/whisper-stream-server.log"
    fi
    if [[ -f "$INSTALL_LOG/whisper-stream-server.error.log" ]]; then
        rm -f "$INSTALL_LOG/whisper-stream-server.error.log"
        echo "  Removed: $INSTALL_LOG/whisper-stream-server.error.log"
    fi
else
    echo "  Kept logs in: $INSTALL_LOG/"
fi

echo ""
echo -e "${GREEN}=== Uninstallation Complete ===${NC}"
echo ""

if [[ "$KEEP_MODEL" == true ]]; then
    echo "Model preserved at: $INSTALL_SHARE/ggml-base.en.bin"
fi
if [[ "$KEEP_LOGS" == true ]]; then
    echo "Logs preserved at: $INSTALL_LOG/whisper-stream-server*.log"
fi
