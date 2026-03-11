#!/usr/bin/env bash
#
# Generic E2E test runner for OpenRTX.
#
# Takes a test script file containing emulator shell commands (key, sleep,
# screenshot, quit, etc.) and runs it against the linux emulator. Any
# "screenshot <name>.bmp" commands in the script automatically become
# assertions: the captured image is compared pixel-for-pixel against a
# golden reference in tests/e2e/golden/<test_name>/<name>.bmp.
#
# Usage:
#   bash tests/e2e/run_e2e.sh <test_script> [path/to/openrtx_linux] [variant]
#
# The optional variant (default: "default") selects which set of golden
# images to compare against: tests/e2e/golden/<test_name>/<variant>/
# This lets the same test script run against binaries with different
# screen sizes (e.g. the 128x64 Module17 variant).
#
# Environment:
#   UPDATE_GOLDEN=1  — overwrite golden images with current output
#   TOLERANCE=0      — max differing pixels before failure (default: 0)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

TEST_SCRIPT="${1:?Usage: run_e2e.sh <test_script> [openrtx_binary] [variant]}"
OPENRTX="${2:-$PROJECT_ROOT/build_linux/openrtx_linux}"
VARIANT="${3:-default}"
TOLERANCE="${TOLERANCE:-0}"

# Derive test name and golden dir from the script filename
TEST_NAME="$(basename "$TEST_SCRIPT" .txt)"
GOLDEN_DIR="$SCRIPT_DIR/golden/$TEST_NAME/$VARIANT"

# ── helpers ──────────────────────────────────────────────────────────────────

die()  { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }

check_prerequisites() {
    [[ -f "$TEST_SCRIPT" ]] || die "test script not found: $TEST_SCRIPT"
    [[ -x "$OPENRTX" ]]    || die "openrtx_linux not found at $OPENRTX — build it first"
    command -v compare >/dev/null 2>&1 || die "'compare' (ImageMagick) not found on PATH"
    command -v faketime >/dev/null 2>&1 || die "'faketime' not found on PATH"
}

# ── main ─────────────────────────────────────────────────────────────────────

check_prerequisites

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

echo "Running E2E test: $TEST_NAME ($VARIANT)"

# Rewrite screenshot commands to capture into our temp dir, and collect
# the list of screenshot filenames for later assertion.
SCREENSHOTS=()
REWRITTEN_SCRIPT=""

while IFS= read -r line; do
    # Match lines like: screenshot foo.bmp  or  screenshot  some_name.bmp
    if [[ "$line" =~ ^[[:space:]]*screenshot[[:space:]]+(.+)$ ]]; then
        name="${BASH_REMATCH[1]}"
        SCREENSHOTS+=("$name")
        REWRITTEN_SCRIPT+="screenshot $TMPDIR/$name"$'\n'
    else
        REWRITTEN_SCRIPT+="$line"$'\n'
    fi
done < "$TEST_SCRIPT"

# Give each test its own codeplug file so parallel runs don't share state
cp "$PROJECT_ROOT/default.rtxc" "$TMPDIR/default.rtxc"

# Run the emulator with a fresh NVM state and frozen clock for determinism
TZ=UTC faketime -f '2000-01-01 00:00:00' env \
    XDG_STATE_HOME="$TMPDIR/state" SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
TZ=UTC faketime -f '2000-01-01 00:00:00' env \
    XDG_STATE_HOME="$TMPDIR/state" SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
    timeout 30 bash -c "cd \"$TMPDIR\" && exec \"$OPENRTX\"" <<< "$REWRITTEN_SCRIPT" >/dev/null 2>&1

# Directory for persisting failed snapshots across runs
FAIL_DIR="$PROJECT_ROOT/build_linux/e2e_failures/$TEST_NAME/$VARIANT"

# Assert each screenshot
FAILURES=0
for name in "${SCREENSHOTS[@]}"; do
    actual="$TMPDIR/$name"
    golden="$GOLDEN_DIR/$name"

    if [[ ! -f "$actual" ]]; then
        echo "FAIL: screenshot not produced: $name"
        FAILURES=$((FAILURES + 1))
        continue
    fi

    if [[ "${UPDATE_GOLDEN:-}" == "1" ]]; then
        mkdir -p "$GOLDEN_DIR"
        cp "$actual" "$golden"
        echo "  updated golden: $name"
        continue
    fi

    if [[ ! -f "$golden" ]]; then
        echo "FAIL: golden image missing: $golden (run with UPDATE_GOLDEN=1 to create)"
        FAILURES=$((FAILURES + 1))
        continue
    fi

    diff_pixels=$(compare -metric AE "$actual" "$golden" /dev/null 2>&1) || true

    if [[ "$diff_pixels" -le "$TOLERANCE" ]]; then
        pass "$name (${diff_pixels} pixels differ)"
    else
        echo "FAIL: $name — ${diff_pixels} pixels differ (tolerance: ${TOLERANCE})"
        mkdir -p "$FAIL_DIR"
        cp "$actual" "$FAIL_DIR/actual_$name"
        cp "$golden" "$FAIL_DIR/expected_$name"
        compare "$actual" "$golden" "$FAIL_DIR/diff_$name" 2>/dev/null || true
        echo "  actual:   $FAIL_DIR/actual_$name"
        echo "  expected: $FAIL_DIR/expected_$name"
        echo "  diff:     $FAIL_DIR/diff_$name"
        FAILURES=$((FAILURES + 1))
    fi
done

if [[ "$FAILURES" -gt 0 ]]; then
    echo "Snapshot failures saved to: $FAIL_DIR"
    die "$FAILURES screenshot(s) failed"
fi

echo "All assertions passed."
