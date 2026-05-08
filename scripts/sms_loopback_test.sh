#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# SMS loopback RX integration test.
#
# Phase 1 (TX): run the SMS send script and capture 48 kHz baseband to
#   /tmp/m17_output.raw, verify the file is non-empty.
#
# Phase 2 (RX): downsample the TX output to 24 kHz, place it at
#   /tmp/baseband.raw so the file_source audio driver feeds it to the
#   M17 demodulator, then launch the emulator in M17 RX mode and wait
#   for an "SMS_RECEIVED" line on stderr.
#
# Prerequisites:
#   - Emulator binary built with SMS_RECEIVED stderr print and file_source fix.
#   - The emulator NVM already has M17 selected as the operating mode
#     (run the radio once and switch to M17, then it persists).
#   - 'sox' installed for 48 → 24 kHz resampling.
#
# Usage:
#   bash scripts/sms_loopback_test.sh                # uses build_linux
#   bash scripts/sms_loopback_test.sh BUILD_DIR

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${1:-${REPO_ROOT}/build_linux}"
BINARY="${BUILD_DIR}/openrtx_linux"
TX_SCRIPT="${REPO_ROOT}/meta/sms_send_test.txt"
RX_SCRIPT="${REPO_ROOT}/meta/sms_rx_test.txt"
TX_RAW="/tmp/m17_output.raw"
RX_INPUT="/tmp/baseband.raw"
GOLDEN="${REPO_ROOT}/tests/unit/assets/sms_a_golden.raw"

cleanup()
{
    rm -f "${TX_RAW}" "${RX_INPUT}" "${GOLDEN}"
}
trap cleanup EXIT

if [[ ! -x "${BINARY}" ]]; then
    echo "ERROR: emulator binary not found at ${BINARY}" >&2
    echo "  Run 'meson compile -C ${BUILD_DIR} openrtx_linux' first." >&2
    exit 1
fi

# ── Phase 1: TX ──────────────────────────────────────────────────────────────
echo "==> [Phase 1] TX: generating SMS baseband"
rm -f "${TX_RAW}"
SDL_VIDEODRIVER=offscreen "${BINARY}" < "${TX_SCRIPT}" 2>/dev/null

if [[ ! -s "${TX_RAW}" ]]; then
    echo "FAIL [Phase 1]: ${TX_RAW} was not produced or is empty — TX did not fire." >&2
    exit 1
fi

TX_BYTES="$(wc -c < "${TX_RAW}")"
echo "    Captured ${TX_BYTES} bytes of 48 kHz baseband (expected 19200)"

if [[ "${TX_BYTES}" -ne 19200 ]]; then
    echo "FAIL [Phase 1]: unexpected TX output size ${TX_BYTES} (expected 19200)." >&2
    exit 1
fi
echo "PASS [Phase 1]: TX produced ${TX_BYTES} bytes."

# ── Phase 2: downsample 48 kHz → 24 kHz ─────────────────────────────────────
echo "==> [Phase 2] Resampling TX output to 24 kHz for RX injection"
sox -r 48000 -e signed-integer -b 16 -c 1 "${TX_RAW}" \
    -r 24000 "${RX_INPUT}"
echo "    Written ${RX_INPUT} ($(wc -c < "${RX_INPUT}") bytes)"

# ── Phase 3: RX ──────────────────────────────────────────────────────────────
echo "==> [Phase 3] RX: launching emulator, waiting for SMS_RECEIVED on stderr"
RX_STDERR="$(mktemp)"
SDL_VIDEODRIVER=offscreen "${BINARY}" < "${RX_SCRIPT}" 2>"${RX_STDERR}" || true

echo "--- stderr output ---"
cat "${RX_STDERR}"
echo "---------------------"

if grep -q "SMS_RECEIVED" "${RX_STDERR}"; then
    echo "PASS [Phase 3]: SMS_RECEIVED detected in stderr."
    rm -f "${RX_STDERR}"
    exit 0
else
    echo "FAIL [Phase 3]: SMS_RECEIVED not found in emulator stderr." >&2
    rm -f "${RX_STDERR}"
    exit 1
fi
