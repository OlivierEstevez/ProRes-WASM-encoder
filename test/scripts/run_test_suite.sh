#!/usr/bin/env bash
#
# run_test_suite.sh - Master orchestrator for the PNG sequence encoding test suite
#
# Builds the native encoder, encodes all sequences with native + FFmpeg,
# analyzes results, and generates reports.
#
# Usage:
#   ./test/scripts/run_test_suite.sh              # Run all sequences
#   ./test/scripts/run_test_suite.sh TEST-01       # Run single sequence
#   ./test/scripts/run_test_suite.sh --skip-ffmpeg # Skip FFmpeg encoding

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
RESULTS_DIR="$PROJECT_DIR/test/suite-results"

FILTER=""
SKIP_FFMPEG=false
SKIP_NATIVE=false
SKIP_ANALYZE=false

# Parse args
for arg in "$@"; do
    case "$arg" in
        --skip-ffmpeg)  SKIP_FFMPEG=true ;;
        --skip-native)  SKIP_NATIVE=true ;;
        --skip-analyze) SKIP_ANALYZE=true ;;
        --help|-h)
            echo "Usage: $0 [SEQUENCE_NAME] [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --skip-ffmpeg   Skip FFmpeg reference encoding"
            echo "  --skip-native   Skip native encoder"
            echo "  --skip-analyze  Skip analysis and report generation"
            echo ""
            echo "If SEQUENCE_NAME is provided, only process that sequence."
            exit 0
            ;;
        *)
            FILTER="$arg"
            ;;
    esac
done

echo "================================================"
echo "  ProRes WASM Encoder - PNG Sequence Test Suite"
echo "================================================"
echo ""
echo "Results directory: $RESULTS_DIR"
echo "Filter: ${FILTER:-all sequences}"
echo ""

mkdir -p "$RESULTS_DIR"

START_TIME=$(date +%s)

# Step 1: Native encoding
if ! $SKIP_NATIVE; then
    echo ""
    echo "========================================"
    echo "  Step 1/4: Native Encoding"
    echo "========================================"
    echo ""
    bash "$SCRIPT_DIR/encode_native.sh" ${FILTER:+"$FILTER"}
fi

# Step 2: FFmpeg encoding
if ! $SKIP_FFMPEG; then
    echo ""
    echo "========================================"
    echo "  Step 2/4: FFmpeg Reference Encoding"
    echo "========================================"
    echo ""
    bash "$SCRIPT_DIR/encode_ffmpeg.sh" ${FILTER:+"$FILTER"}
fi

# Step 3: Analysis
if ! $SKIP_ANALYZE; then
    echo ""
    echo "========================================"
    echo "  Step 3/4: Analysis (PSNR/SSIM/Diffs)"
    echo "========================================"
    echo ""
    bash "$SCRIPT_DIR/analyze_results.sh" ${FILTER:+"$FILTER"}

    # Step 4: Reports
    echo ""
    echo "========================================"
    echo "  Step 4/4: Report Generation"
    echo "========================================"
    echo ""
    bash "$SCRIPT_DIR/generate_reports.sh" ${FILTER:+"$FILTER"}
fi

END_TIME=$(date +%s)
ELAPSED=$(( END_TIME - START_TIME ))

echo ""
echo "================================================"
echo "  Test Suite Complete"
echo "  Total time: ${ELAPSED}s"
echo "  Results:    $RESULTS_DIR"
if [[ -f "$RESULTS_DIR/summary.md" ]]; then
    echo "  Summary:    $RESULTS_DIR/summary.md"
fi
echo "================================================"
