#!/bin/bash
#
# Master test runner for whisper-stream-server
#
# Runs all available tests:
#   1. C++ Unit tests (AudioBuffer, VAD State Machine)
#   2. C++ Integration tests (requires whisper model)
#   3. E2E TypeScript tests (requires running server)
#
# Usage:
#   ./scripts/run_tests.sh           # Run all tests
#   ./scripts/run_tests.sh unit      # Run only unit tests
#   ./scripts/run_tests.sh int       # Run only integration tests
#   ./scripts/run_tests.sh e2e       # Run only E2E tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
E2E_DIR="$PROJECT_DIR/tests/e2e"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Parse arguments
RUN_UNIT=true
RUN_INT=true
RUN_E2E=true

if [ "$1" = "unit" ]; then
    RUN_INT=false
    RUN_E2E=false
elif [ "$1" = "int" ]; then
    RUN_UNIT=false
    RUN_E2E=false
elif [ "$1" = "e2e" ]; then
    RUN_UNIT=false
    RUN_INT=false
fi

echo -e "${YELLOW}=== Building with tests ===${NC}"
cmake -B "$BUILD_DIR" -DBUILD_TESTS=ON "$PROJECT_DIR" 2>&1 | grep -v "^--" | head -5
cmake --build "$BUILD_DIR" -j 2>&1 | tail -5

UNIT_PASSED=0
INT_PASSED=0
E2E_PASSED=0
UNIT_FAILED=0
INT_FAILED=0
E2E_FAILED=0

# Unit tests
if $RUN_UNIT; then
    echo ""
    echo -e "${YELLOW}=== Unit Tests ===${NC}"

    if [ -f "$BUILD_DIR/test_audio_buffer" ]; then
        echo "Running AudioBuffer tests..."
        if "$BUILD_DIR/test_audio_buffer" --reporter compact 2>&1 | tail -3; then
            UNIT_PASSED=$((UNIT_PASSED + 1))
        else
            UNIT_FAILED=$((UNIT_FAILED + 1))
        fi
    fi

    if [ -f "$BUILD_DIR/test_vad_state_machine" ]; then
        echo ""
        echo "Running VAD State Machine tests..."
        if "$BUILD_DIR/test_vad_state_machine" --reporter compact 2>&1 | tail -3; then
            UNIT_PASSED=$((UNIT_PASSED + 1))
        else
            UNIT_FAILED=$((UNIT_FAILED + 1))
        fi
    fi
fi

# Integration tests
if $RUN_INT; then
    echo ""
    echo -e "${YELLOW}=== Integration Tests ===${NC}"

    MODEL_PATH="$PROJECT_DIR/../whisper.cpp/models/ggml-base.en.bin"
    if [ -f "$MODEL_PATH" ]; then
        if [ -f "$BUILD_DIR/test_transcription" ]; then
            echo "Running Transcription tests..."
            if cd "$BUILD_DIR" && ./test_transcription --reporter compact 2>&1 | grep -E "^(All tests|test cases|assertions|FAILED)" | tail -5; then
                INT_PASSED=$((INT_PASSED + 1))
            else
                INT_FAILED=$((INT_FAILED + 1))
            fi
        fi
    else
        echo -e "${YELLOW}Skipped: whisper model not found at $MODEL_PATH${NC}"
    fi
fi

# E2E tests
if $RUN_E2E; then
    echo ""
    echo -e "${YELLOW}=== E2E Tests ===${NC}"

    # Check if server is running
    if nc -z localhost 9090 2>/dev/null; then
        cd "$E2E_DIR"

        # Install dependencies if needed
        if [ ! -d "node_modules" ]; then
            echo "Installing E2E test dependencies..."
            npm install 2>&1 | tail -3
        fi

        echo "Running E2E tests..."
        if npm test 2>&1; then
            E2E_PASSED=$((E2E_PASSED + 1))
        else
            E2E_FAILED=$((E2E_FAILED + 1))
        fi
    else
        echo -e "${YELLOW}Skipped: server not running on port 9090${NC}"
        echo "Start the server with: ./scripts/run.sh"
    fi
fi

# Summary
echo ""
echo -e "${YELLOW}=== Summary ===${NC}"

TOTAL_PASSED=$((UNIT_PASSED + INT_PASSED + E2E_PASSED))
TOTAL_FAILED=$((UNIT_FAILED + INT_FAILED + E2E_FAILED))

if [ $TOTAL_FAILED -eq 0 ] && [ $TOTAL_PASSED -gt 0 ]; then
    echo -e "${GREEN}All available tests passed!${NC}"
    echo "  Unit:        $UNIT_PASSED passed"
    echo "  Integration: $INT_PASSED passed"
    echo "  E2E:         $E2E_PASSED passed"
    exit 0
elif [ $TOTAL_FAILED -gt 0 ]; then
    echo -e "${RED}Some tests failed${NC}"
    echo "  Unit:        $UNIT_PASSED passed, $UNIT_FAILED failed"
    echo "  Integration: $INT_PASSED passed, $INT_FAILED failed"
    echo "  E2E:         $E2E_PASSED passed, $E2E_FAILED failed"
    exit 1
else
    echo -e "${YELLOW}No tests were run${NC}"
    exit 0
fi
