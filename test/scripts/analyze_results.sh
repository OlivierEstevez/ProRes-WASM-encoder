#!/usr/bin/env bash
#
# analyze_results.sh - Decode MOVs, compute PSNR/SSIM, generate diff images
#
# For each encoder (native, ffmpeg, wasm) and profile, decodes MOVs back to PNGs,
# computes quality metrics against the original reference frames, and generates
# visual diff images for key frames (first, middle, last).
#
# Usage: ./test/scripts/analyze_results.sh [SEQUENCE_NAME]

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

# Profile filenames
PROFILES_422=("proxy" "lt" "standard" "hq" "4444" "4444xq")
PROFILES_ALPHA=("4444" "4444xq")

# Compute PSNR for a MOV against reference PNG sequence
# Args: ref_dir ref_pattern fps mov_path num_frames
# Outputs: avg_psnr to stdout
compute_psnr() {
    local REF_DIR="$1"
    local REF_PATTERN="$2"
    local FPS="$3"
    local MOV_PATH="$4"
    local NUM_FRAMES="$5"
    local STATS_FILE
    STATS_FILE=$(mktemp)

    ffmpeg -nostdin -y \
        -framerate "$FPS" -i "$REF_DIR/$REF_PATTERN" \
        -i "$MOV_PATH" \
        -frames:v "$NUM_FRAMES" \
        -lavfi "[0:v]format=rgb24[ref];[1:v]format=rgb24[enc];[ref][enc]psnr=stats_file=$STATS_FILE" \
        -f null - 2>/dev/null

    # Parse average PSNR from stats file
    if [[ -f "$STATS_FILE" && -s "$STATS_FILE" ]]; then
        # Extract average from last line or compute from all lines
        local AVG
        AVG=$(awk -F'[ :]' '
            /psnr_avg/ {
                for (i=1; i<=NF; i++) {
                    if ($i == "psnr_avg") { sum += $(i+1); count++ }
                }
            }
            END { if (count > 0) printf "%.2f", sum/count; else print "0" }
        ' "$STATS_FILE")
        rm -f "$STATS_FILE"
        echo "$AVG"
    else
        rm -f "$STATS_FILE"
        echo "0"
    fi
}

# Compute SSIM for a MOV against reference PNG sequence
compute_ssim() {
    local REF_DIR="$1"
    local REF_PATTERN="$2"
    local FPS="$3"
    local MOV_PATH="$4"
    local NUM_FRAMES="$5"
    local STATS_FILE
    STATS_FILE=$(mktemp)

    ffmpeg -nostdin -y \
        -framerate "$FPS" -i "$REF_DIR/$REF_PATTERN" \
        -i "$MOV_PATH" \
        -frames:v "$NUM_FRAMES" \
        -lavfi "[0:v]format=rgb24[ref];[1:v]format=rgb24[enc];[ref][enc]ssim=stats_file=$STATS_FILE" \
        -f null - 2>/dev/null

    if [[ -f "$STATS_FILE" && -s "$STATS_FILE" ]]; then
        local AVG
        AVG=$(awk -F'[ :]' '
            /All/ {
                for (i=1; i<=NF; i++) {
                    if ($i == "All") { sum += $(i+1); count++ }
                }
            }
            END { if (count > 0) printf "%.6f", sum/count; else print "0" }
        ' "$STATS_FILE")
        rm -f "$STATS_FILE"
        echo "$AVG"
    else
        rm -f "$STATS_FILE"
        echo "0"
    fi
}

# Generate diff images for key frames
# Args: ref_dir ref_pattern mov_path diff_dir num_frames fps
generate_diffs() {
    local REF_DIR="$1"
    local REF_PATTERN="$2"
    local MOV_PATH="$3"
    local DIFF_DIR="$4"
    local NUM_FRAMES="$5"
    local FPS="$6"

    mkdir -p "$DIFF_DIR"

    # Decode MOV to temp dir
    local DECODED_DIR
    DECODED_DIR=$(mktemp -d)
    ffmpeg -nostdin -y -i "$MOV_PATH" -pix_fmt rgba -start_number 0 \
        "$DECODED_DIR/frame_%04d.png" 2>/dev/null

    # Key frames: first (0), middle, last
    local MIDDLE=$(( NUM_FRAMES / 2 ))
    local LAST=$(( NUM_FRAMES - 1 ))

    for FRAME_IDX in 0 "$MIDDLE" "$LAST"; do
        local REF_FILE
        REF_FILE=$(printf "$REF_DIR/$REF_PATTERN" "$FRAME_IDX")
        local DEC_FILE
        DEC_FILE=$(printf "$DECODED_DIR/frame_%04d.png" "$FRAME_IDX")
        local DIFF_FILE="$DIFF_DIR/frame_$(printf '%04d' "$FRAME_IDX")_diff.png"

        if [[ -f "$REF_FILE" && -f "$DEC_FILE" ]]; then
            ffmpeg -nostdin -y \
                -i "$REF_FILE" -i "$DEC_FILE" \
                -filter_complex "[0:v]format=rgb24[a];[1:v]format=rgb24[b];[a][b]blend=all_mode=difference,eq=contrast=10" \
                "$DIFF_FILE" 2>/dev/null || true
        fi
    done

    rm -rf "$DECODED_DIR"
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
    SEQ_RESULTS="$RESULTS_DIR/$NAME"
    FPS="$FPS_NUM/$FPS_DEN"

    echo "=== Analyzing: $NAME ==="

    # Choose profiles based on alpha
    if [[ "$HAS_ALPHA" == "1" ]]; then
        PROFILES=("${PROFILES_ALPHA[@]}")
    else
        PROFILES=("${PROFILES_422[@]}")
    fi

    # Start JSON metrics
    METRICS_FILE="$SEQ_RESULTS/metrics.json"
    echo "{" > "$METRICS_FILE"
    echo "  \"sequence\": \"$NAME\"," >> "$METRICS_FILE"
    echo "  \"width\": $WIDTH," >> "$METRICS_FILE"
    echo "  \"height\": $HEIGHT," >> "$METRICS_FILE"
    echo "  \"fps\": \"$FPS_NUM/$FPS_DEN\"," >> "$METRICS_FILE"
    echo "  \"num_frames\": $NUM_FRAMES," >> "$METRICS_FILE"
    echo "  \"has_alpha\": $HAS_ALPHA," >> "$METRICS_FILE"
    echo "  \"results\": {" >> "$METRICS_FILE"

    FIRST_ENCODER=true
    for ENCODER in native ffmpeg wasm; do
        ENCODER_DIR="$SEQ_RESULTS/$ENCODER"
        [[ -d "$ENCODER_DIR" ]] || continue

        echo "  Encoder: $ENCODER"

        if $FIRST_ENCODER; then
            FIRST_ENCODER=false
        else
            echo "," >> "$METRICS_FILE"
        fi
        echo "    \"$ENCODER\": {" >> "$METRICS_FILE"

        FIRST_PROFILE=true
        for PROF in "${PROFILES[@]}"; do
            MOV_FILE="$ENCODER_DIR/$PROF.mov"
            [[ -f "$MOV_FILE" ]] || continue

            echo "    Profile: $PROF"

            # File size
            SIZE=$(stat -f%z "$MOV_FILE" 2>/dev/null || stat --printf="%s" "$MOV_FILE" 2>/dev/null)

            # PSNR
            echo "      Computing PSNR..."
            PSNR=$(compute_psnr "$INPUT_DIR" "$PATTERN" "$FPS" "$MOV_FILE" "$NUM_FRAMES")

            # SSIM
            echo "      Computing SSIM..."
            SSIM=$(compute_ssim "$INPUT_DIR" "$PATTERN" "$FPS" "$MOV_FILE" "$NUM_FRAMES")

            # Diff images
            DIFF_DIR="$SEQ_RESULTS/diffs/${ENCODER}-vs-ref/$PROF"
            echo "      Generating diff images..."
            generate_diffs "$INPUT_DIR" "$PATTERN" "$MOV_FILE" "$DIFF_DIR" "$NUM_FRAMES" "$FPS"

            echo "      PSNR: $PSNR dB, SSIM: $SSIM, Size: $SIZE bytes"

            if $FIRST_PROFILE; then
                FIRST_PROFILE=false
            else
                echo "," >> "$METRICS_FILE"
            fi
            echo "      \"$PROF\": {" >> "$METRICS_FILE"
            echo "        \"size_bytes\": $SIZE," >> "$METRICS_FILE"
            echo "        \"psnr_avg\": $PSNR," >> "$METRICS_FILE"
            echo "        \"ssim_avg\": $SSIM" >> "$METRICS_FILE"
            echo -n "      }" >> "$METRICS_FILE"
        done

        echo "" >> "$METRICS_FILE"
        echo -n "    }" >> "$METRICS_FILE"
    done

    echo "" >> "$METRICS_FILE"
    echo "  }" >> "$METRICS_FILE"
    echo "}" >> "$METRICS_FILE"

    echo "  Metrics written to: $METRICS_FILE"
    echo ""
done < "$CONF"

echo "=== Analysis complete ==="
