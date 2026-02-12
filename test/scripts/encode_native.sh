#!/usr/bin/env bash
#
# encode_native.sh - Encode PNG sequences using the native C encoder
#
# Reads sequences.conf and runs test_png_sequences for each entry.
# Usage: ./test/scripts/encode_native.sh [SEQUENCE_NAME]
#   If SEQUENCE_NAME is provided, only encode that sequence.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
CONF="$SCRIPT_DIR/sequences.conf"
BINARY="$PROJECT_DIR/test/binaries/test_png_sequences"
REF_DIR="$PROJECT_DIR/test/reference"
RESULTS_DIR="$PROJECT_DIR/test/suite-results"

FILTER="${1:-}"

# Build the test binary
echo "=== Building native test binary ==="
gcc -O2 -o "$BINARY" \
    "$PROJECT_DIR/test/src/test_png_sequences.c" \
    "$PROJECT_DIR"/src/encoder/*.c \
    "$PROJECT_DIR"/src/muxer/*.c \
    -I "$PROJECT_DIR/src" \
    -I "$PROJECT_DIR/third_party" \
    -lm
echo "Built: $BINARY"
echo ""

# Process each sequence
while IFS='|' read -r NAME DIR WIDTH HEIGHT FPS_NUM FPS_DEN NUM_FRAMES HAS_ALPHA PATTERN; do
    # Skip comments and empty lines
    [[ "$NAME" =~ ^#.*$ || -z "$NAME" ]] && continue

    # Filter if requested
    if [[ -n "$FILTER" && "$NAME" != "$FILTER" ]]; then
        continue
    fi

    INPUT_DIR="$REF_DIR/$DIR"
    OUTPUT_DIR="$RESULTS_DIR/$NAME/native"

    echo "=== Encoding: $NAME ==="
    echo "  Input:  $INPUT_DIR"
    echo "  Output: $OUTPUT_DIR"

    mkdir -p "$OUTPUT_DIR"

    # Run the encoder, capture JSON timing data from stderr
    "$BINARY" \
        "$INPUT_DIR" \
        "$OUTPUT_DIR" \
        "$WIDTH" "$HEIGHT" \
        "$FPS_NUM" "$FPS_DEN" \
        "$NUM_FRAMES" \
        "$HAS_ALPHA" \
        "$PATTERN" \
        2>"$RESULTS_DIR/$NAME/native_timing.json" || {
            echo "  WARNING: Encoding failed for $NAME"
            continue
        }

    echo ""
done < "$CONF"

echo "=== Native encoding complete ==="
