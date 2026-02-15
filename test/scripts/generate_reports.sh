#!/usr/bin/env bash
#
# generate_reports.sh - Generate markdown reports from metrics.json files
#
# Creates per-sequence report.md files and an overall summary.md.
# Usage: ./test/scripts/generate_reports.sh [SEQUENCE_NAME]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
CONF="$SCRIPT_DIR/sequences.conf"
RESULTS_DIR="$PROJECT_DIR/test/suite-results"

FILTER="${1:-}"

# Helper: extract JSON value using python3
json_val() {
    local FILE="$1"
    local QUERY="$2"
    python3 -c "
import json, sys
with open('$FILE') as f:
    data = json.load(f)
val = $QUERY
if val is None:
    print('N/A')
elif isinstance(val, float):
    print(f'{val:.4f}')
else:
    print(val)
" 2>/dev/null || echo "N/A"
}

# Helper: format bytes as MB
bytes_to_mb() {
    python3 -c "print(f'{$1 / 1048576:.2f}')" 2>/dev/null || echo "N/A"
}

# Collect all sequence names for summary
SEQUENCES=()

# Generate per-sequence reports
while IFS='|' read -r NAME DIR WIDTH HEIGHT FPS_NUM FPS_DEN NUM_FRAMES HAS_ALPHA PATTERN; do
    [[ "$NAME" =~ ^#.*$ || -z "$NAME" ]] && continue

    if [[ -n "$FILTER" && "$NAME" != "$FILTER" ]]; then
        continue
    fi

    SEQ_DIR="$RESULTS_DIR/$NAME"
    METRICS="$SEQ_DIR/metrics.json"

    if [[ ! -f "$METRICS" ]]; then
        echo "Skipping $NAME: no metrics.json found"
        continue
    fi

    SEQUENCES+=("$NAME")
    REPORT="$SEQ_DIR/report.md"

    echo "Generating report for $NAME..."

    # Determine profiles
    if [[ "$HAS_ALPHA" == "1" ]]; then
        PROFILES=("4444" "4444xq")
    else
        PROFILES=("proxy" "lt" "standard" "hq" "4444" "4444xq")
    fi

    # Which encoders are present?
    ENCODERS=()
    for ENC in native ffmpeg ffmpeg-default wasm; do
        if python3 -c "
import json
with open('$METRICS') as f:
    data = json.load(f)
assert '$ENC' in data.get('results', {})
" 2>/dev/null; then
            ENCODERS+=("$ENC")
        fi
    done

    cat > "$REPORT" <<EOF
# Test Report: $NAME

| Property | Value |
|----------|-------|
| Resolution | ${WIDTH}x${HEIGHT} |
| Frame Rate | ${FPS_NUM}/${FPS_DEN} fps |
| Frames | ${NUM_FRAMES} |
| Alpha | $([ "$HAS_ALPHA" == "1" ] && echo "Yes" || echo "No") |

## File Size Comparison

| Profile |$(for e in "${ENCODERS[@]}"; do echo -n " $e (MB) |"; done) $(if [[ ${#ENCODERS[@]} -ge 2 ]]; then echo -n "Ratio |"; fi)
|---------|$(for e in "${ENCODERS[@]}"; do echo -n "-----------|"; done) $(if [[ ${#ENCODERS[@]} -ge 2 ]]; then echo -n "------|"; fi)
EOF

    for PROF in "${PROFILES[@]}"; do
        ROW="| $PROF |"
        SIZES=()
        for ENC in "${ENCODERS[@]}"; do
            SIZE=$(json_val "$METRICS" "data['results'].get('$ENC', {}).get('$PROF', {}).get('size_bytes', 0)")
            if [[ "$SIZE" != "N/A" && "$SIZE" != "0" ]]; then
                MB=$(bytes_to_mb "$SIZE")
                ROW+=" $MB |"
                SIZES+=("$SIZE")
            else
                ROW+=" N/A |"
                SIZES+=("0")
            fi
        done

        # Compute ratio (first encoder / ffmpeg, or first / second)
        if [[ ${#ENCODERS[@]} -ge 2 && ${#SIZES[@]} -ge 2 ]]; then
            if [[ "${SIZES[0]}" != "0" && "${SIZES[1]}" != "0" ]]; then
                RATIO=$(python3 -c "print(f'{${SIZES[0]} / ${SIZES[1]}:.2f}x')" 2>/dev/null || echo "N/A")
                ROW+=" $RATIO |"
            else
                ROW+=" N/A |"
            fi
        fi

        echo "$ROW" >> "$REPORT"
    done

    cat >> "$REPORT" <<'EOF'

## Quality Metrics (vs Reference PNGs)

EOF

    echo "| Profile |$(for e in "${ENCODERS[@]}"; do echo -n " $e PSNR | $e SSIM |"; done)" >> "$REPORT"
    echo "|---------|$(for e in "${ENCODERS[@]}"; do echo -n "-----------|-----------|"; done)" >> "$REPORT"

    for PROF in "${PROFILES[@]}"; do
        ROW="| $PROF |"
        for ENC in "${ENCODERS[@]}"; do
            PSNR=$(json_val "$METRICS" "data['results'].get('$ENC', {}).get('$PROF', {}).get('psnr_avg', 'N/A')")
            SSIM=$(json_val "$METRICS" "data['results'].get('$ENC', {}).get('$PROF', {}).get('ssim_avg', 'N/A')")
            ROW+=" $PSNR | $SSIM |"
        done
        echo "$ROW" >> "$REPORT"
    done

    # Diff images section
    if [[ -d "$SEQ_DIR/diffs" ]]; then
        cat >> "$REPORT" <<'EOF'

## Visual Comparisons (Diff Images)

Difference images are amplified 10x for visibility. Brighter = larger difference.

EOF
        for ENC in "${ENCODERS[@]}"; do
            DIFF_BASE="$SEQ_DIR/diffs/${ENC}-vs-ref"
            [[ -d "$DIFF_BASE" ]] || continue
            echo "### $ENC vs Reference" >> "$REPORT"
            echo "" >> "$REPORT"
            for PROF in "${PROFILES[@]}"; do
                PROF_DIFF="$DIFF_BASE/$PROF"
                [[ -d "$PROF_DIFF" ]] || continue
                DIFF_FILES=$(ls "$PROF_DIFF"/*_diff.png 2>/dev/null | head -3)
                if [[ -n "$DIFF_FILES" ]]; then
                    echo "**$PROF:**" >> "$REPORT"
                    for F in $DIFF_FILES; do
                        BASENAME=$(basename "$F")
                        # Use path relative to report file (which is in SEQ_DIR)
                        echo "![${BASENAME}](diffs/${ENC}-vs-ref/${PROF}/${BASENAME})" >> "$REPORT"
                    done
                    echo "" >> "$REPORT"
                fi
            done
        done
    fi

    echo "  Written: $REPORT"
done < "$CONF"

# Generate summary
if [[ ${#SEQUENCES[@]} -gt 0 ]]; then
    SUMMARY="$RESULTS_DIR/summary.md"
    echo "Generating summary..."

    cat > "$SUMMARY" <<EOF
# PNG Sequence Encoding Test Suite - Summary

Generated: $(date -u "+%Y-%m-%d %H:%M:%S UTC")

## Sequences

| Sequence | Resolution | FPS | Frames | Alpha | Report |
|----------|-----------|-----|--------|-------|--------|
EOF

    while IFS='|' read -r NAME DIR WIDTH HEIGHT FPS_NUM FPS_DEN NUM_FRAMES HAS_ALPHA PATTERN; do
        [[ "$NAME" =~ ^#.*$ || -z "$NAME" ]] && continue
        ALPHA_STR=$([ "$HAS_ALPHA" == "1" ] && echo "Yes" || echo "No")

        if [[ -f "$RESULTS_DIR/$NAME/report.md" ]]; then
            echo "| $NAME | ${WIDTH}x${HEIGHT} | ${FPS_NUM}/${FPS_DEN} | $NUM_FRAMES | $ALPHA_STR | [$NAME/report.md]($NAME/report.md) |" >> "$SUMMARY"
        else
            echo "| $NAME | ${WIDTH}x${HEIGHT} | ${FPS_NUM}/${FPS_DEN} | $NUM_FRAMES | $ALPHA_STR | (not available) |" >> "$SUMMARY"
        fi
    done < "$CONF"

    # Add overview comparison table (HQ profile across all sequences)
    cat >> "$SUMMARY" <<'EOF'

## HQ Profile Overview (across sequences)

| Sequence | Native Size (MB) | FFmpeg Size (MB) | Native PSNR | FFmpeg PSNR | Size Ratio |
|----------|------------------|------------------|-------------|-------------|------------|
EOF

    for SEQ in "${SEQUENCES[@]}"; do
        METRICS="$RESULTS_DIR/$SEQ/metrics.json"
        [[ -f "$METRICS" ]] || continue

        # Try HQ first, fall back to 4444
        PROF="hq"
        HAS_HQ=$(python3 -c "
import json
with open('$METRICS') as f:
    d = json.load(f)
print('yes' if 'hq' in d.get('results',{}).get('native',{}) or 'hq' in d.get('results',{}).get('ffmpeg',{}) else 'no')
" 2>/dev/null)

        if [[ "$HAS_HQ" != "yes" ]]; then
            PROF="4444"
        fi

        python3 -c "
import json
with open('$METRICS') as f:
    d = json.load(f)
r = d.get('results', {})
n = r.get('native', {}).get('$PROF', {})
f_data = r.get('ffmpeg', {}).get('$PROF', {})
ns = n.get('size_bytes', 0)
fs = f_data.get('size_bytes', 0)
np = n.get('psnr_avg', 'N/A')
fp = f_data.get('psnr_avg', 'N/A')
ns_mb = f'{ns/1048576:.2f}' if ns else 'N/A'
fs_mb = f'{fs/1048576:.2f}' if fs else 'N/A'
ratio = f'{ns/fs:.2f}x' if ns and fs else 'N/A'
print(f'| $SEQ | {ns_mb} | {fs_mb} | {np} | {fp} | {ratio} |')
" 2>/dev/null >> "$SUMMARY" || echo "| $SEQ | N/A | N/A | N/A | N/A | N/A |" >> "$SUMMARY"
    done

    echo "  Written: $SUMMARY"
fi

echo "=== Report generation complete ==="
