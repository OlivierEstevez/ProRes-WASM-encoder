#!/usr/bin/env bash
#
# encode_ffmpeg.sh - Encode PNG sequences using FFmpeg prores_ks (reference)
#
# Reads sequences.conf and encodes each sequence with all applicable profiles.
# Usage: ./test/scripts/encode_ffmpeg.sh [SEQUENCE_NAME]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
CONF="$SCRIPT_DIR/sequences.conf"
REF_DIR="$PROJECT_DIR/test/reference"
RESULTS_DIR="$PROJECT_DIR/test/suite-results"

FILTER="${1:-}"

# Check ffmpeg
if ! command -v ffmpeg &>/dev/null; then
    echo "ERROR: ffmpeg not found in PATH"
    exit 1
fi

# Profile encoding parameters
# Args: profile_value pix_fmt output_path [qscale] input_args...
# If qscale is provided (non-empty), adds -qscale:v <value>
encode_profile() {
    local PROFILE_VAL="$1"
    local PIX_FMT="$2"
    local OUTPUT="$3"
    local QSCALE="$4"
    shift 4
    # Remaining args are input args

    local START_TIME
    START_TIME=$(python3 -c "import time; print(time.time())")

    local QSCALE_ARGS=()
    if [[ -n "$QSCALE" ]]; then
        QSCALE_ARGS=(-qscale:v "$QSCALE")
    fi

    ffmpeg -nostdin -y "$@" \
        -vcodec prores_ks \
        -profile:v "$PROFILE_VAL" \
        -pix_fmt "$PIX_FMT" \
        ${QSCALE_ARGS[@]+"${QSCALE_ARGS[@]}"} \
        -vendor apl0 \
        "$OUTPUT" 2>/dev/null

    local END_TIME
    END_TIME=$(python3 -c "import time; print(time.time())")

    local ELAPSED
    ELAPSED=$(python3 -c "print(f'{($END_TIME - $START_TIME) * 1000:.1f}')")
    local SIZE
    SIZE=$(stat -f%z "$OUTPUT" 2>/dev/null || stat --printf="%s" "$OUTPUT" 2>/dev/null)

    echo "    Done: $(basename "$OUTPUT") ($(echo "scale=2; $SIZE / 1048576" | bc) MB, ${ELAPSED} ms)"
}

# Process each sequence
while IFS='|' read -r NAME DIR WIDTH HEIGHT FPS_NUM FPS_DEN NUM_FRAMES HAS_ALPHA PATTERN; do
    # Skip comments and empty lines
    [[ "$NAME" =~ ^#.*$ || -z "$NAME" ]] && continue

    # Filter if requested
    if [[ -n "$FILTER" && "$NAME" != "$FILTER" ]]; then
        continue
    fi

    INPUT_DIR="$REF_DIR/$DIR"
    FPS="$FPS_NUM/$FPS_DEN"
    INPUT_ARGS=(-framerate "$FPS" -i "$INPUT_DIR/$PATTERN" -frames:v "$NUM_FRAMES")

    # Encode with both modes: qscale=1 (max quality) and default (adaptive trellis)
    for MODE in ffmpeg-max-quality ffmpeg-default; do
        OUTPUT_DIR="$RESULTS_DIR/$NAME/$MODE"

        if [[ "$MODE" == "ffmpeg-max-quality" ]]; then
            QSCALE="1"
            echo "=== Encoding: $NAME (FFmpeg max quality, qscale=1) ==="
        else
            QSCALE=""
            echo "=== Encoding: $NAME (FFmpeg default/adaptive) ==="
        fi

        echo "  Input:  $INPUT_DIR"
        echo "  Output: $OUTPUT_DIR"

        mkdir -p "$OUTPUT_DIR"

        if [[ "$HAS_ALPHA" == "0" ]]; then
            # Non-alpha: encode all 6 profiles
            echo "  Encoding 422 profiles (Proxy, LT, Standard, HQ)..."

            echo "    Proxy..."
            encode_profile 0 yuv422p10le "$OUTPUT_DIR/proxy.mov" "$QSCALE" "${INPUT_ARGS[@]}"

            echo "    LT..."
            encode_profile 1 yuv422p10le "$OUTPUT_DIR/lt.mov" "$QSCALE" "${INPUT_ARGS[@]}"

            echo "    Standard..."
            encode_profile 2 yuv422p10le "$OUTPUT_DIR/standard.mov" "$QSCALE" "${INPUT_ARGS[@]}"

            echo "    HQ..."
            encode_profile 3 yuv422p10le "$OUTPUT_DIR/hq.mov" "$QSCALE" "${INPUT_ARGS[@]}"

            echo "  Encoding 4444 profiles..."

            echo "    4444..."
            encode_profile 4 yuv444p10le "$OUTPUT_DIR/4444.mov" "$QSCALE" "${INPUT_ARGS[@]}"

            echo "    4444XQ..."
            encode_profile 5 yuv444p10le "$OUTPUT_DIR/4444xq.mov" "$QSCALE" "${INPUT_ARGS[@]}"
        else
            # Alpha: only 4444 profiles with alpha pixel format
            echo "  Encoding 4444 profiles with alpha..."

            echo "    4444..."
            encode_profile 4 yuva444p10le "$OUTPUT_DIR/4444.mov" "$QSCALE" "${INPUT_ARGS[@]}"

            echo "    4444XQ..."
            encode_profile 5 yuva444p10le "$OUTPUT_DIR/4444xq.mov" "$QSCALE" "${INPUT_ARGS[@]}"
        fi

        echo ""
    done
done < "$CONF"

echo "=== FFmpeg encoding complete ==="
