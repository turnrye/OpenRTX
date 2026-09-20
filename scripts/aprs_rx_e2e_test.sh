#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# TEST: APRS end-to-end RX integration test (experimental).
#
# This is a developer/e2e smoke test, framed the same way as
# sms_loopback_test.sh: it depends on the Linux emulator binary, on NVM
# state, and on timing assumptions that vary across machines, so it is not
# suitable for unattended CI.  Use it manually to verify the whole RX path.
#
# Unlike the SMS test there is no TX phase to record, because APRS reception
# here is receive-only.  The baseband is synthesised instead, which is better
# anyway: the packets are written by this script, so it can assert on exact
# callsigns and message text rather than on whatever a capture happened to
# contain.  One packet per data type identifier the source classifies means
# the test pins the classification too, not just transport.
#
#   Phase 1: generate AFSK1200 baseband for three packets and put it where
#     the file_source audio driver reads it.
#
#   Phase 2: run the emulator, switch it to APRS mode, and let the recording
#     play through.
#
#   Phase 3: check stderr for one APRS_RECEIVED line per packet, with the
#     expected sender, type, and text.
#
# Prerequisites:
#   - build_linux/openrtx_linux already built (script assumes this).
#   - NVM (~/.local/state/OpenRTX/state.bin) has M17 opmode selected, so a
#     single macro-menu step reaches APRS.  Run the radio once, switch to
#     M17, and it persists.
#   - python3 (standard library only; no sox, no Dire Wolf).
#
# Usage:
#   bash scripts/aprs_rx_e2e_test.sh              # uses build_linux/
#   bash scripts/aprs_rx_e2e_test.sh /path/to/build

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${1:-${REPO_ROOT}/build_linux}"
BINARY="${BUILD_DIR}/openrtx_linux"
GENERATOR="${SCRIPT_DIR}/aprs_gen_baseband.py"
RX_SCRIPT="${REPO_ROOT}/meta/aprs_rx_test.txt"
RX_INPUT="/tmp/baseband.raw"
# meta/aprs_rx_test.txt writes this next to wherever the emulator was started.
SCREENSHOT="aprs_rx_done.bmp"

export SDL_VIDEODRIVER=dummy

cleanup()
{
    rm -f "${RX_INPUT}" "${SCREENSHOT}"
}
trap cleanup EXIT

# One packet per data type identifier the source classifies, so a
# classification regression fails here and not only in the unit tests.
MSG_PACKET='N0CALL-7>APRS,WIDE1-1::W1AW     :meet on 146.52{001'
POS_PACKET='N0CALL-7>APRS:!4903.50N/07201.75W-test position'
STATUS_PACKET='N0CALL-7>APRS:>monitoring 146.52'

# enum aprsType, from openrtx/include/protocols/APRS/packet.h.
TYPE_MESSAGE=1
TYPE_POSITION=2
TYPE_STATUS=3

# ── Phase 0: preflight ───────────────────────────────────────────────────────
if [[ ! -x "${BINARY}" ]]; then
    echo "FAIL: ${BINARY} not found — build it first." >&2
    exit 1
fi

python3 "${GENERATOR}" --self-check

# ── Phase 1: generate the baseband ───────────────────────────────────────────
echo "==> [Phase 1] Generating AFSK1200 baseband for three packets"
python3 "${GENERATOR}" -o "${RX_INPUT}" \
    -p "${MSG_PACKET}" \
    -p "${POS_PACKET}" \
    -p "${STATUS_PACKET}"

if [[ ! -s "${RX_INPUT}" ]]; then
    echo "FAIL [Phase 1]: ${RX_INPUT} was not produced or is empty." >&2
    exit 1
fi
echo "PASS [Phase 1]: wrote $(wc -c < "${RX_INPUT}") bytes."

# ── Phase 2: run the emulator ────────────────────────────────────────────────
echo "==> [Phase 2] RX: launching emulator in APRS mode"
RX_STDERR="$(mktemp)"
"${BINARY}" < "${RX_SCRIPT}" 2>"${RX_STDERR}" || true

echo "--- APRS_RECEIVED lines ---"
grep "APRS_RECEIVED" "${RX_STDERR}" || true
echo "---------------------------"

# ── Phase 3: check what arrived ──────────────────────────────────────────────
echo "==> [Phase 3] Checking the inbox received all three packets"
status=0

expect()
{
    local label="$1" pattern="$2"
    if grep -qF "${pattern}" "${RX_STDERR}"; then
        echo "PASS [Phase 3]: ${label}"
    else
        echo "FAIL [Phase 3]: ${label} — expected a line containing:" >&2
        echo "    ${pattern}" >&2
        status=1
    fi
}

# The addressed message is unwrapped: no ":ADDRESSEE:" wrapper, no "{001".
expect "addressed message" \
    "APRS_RECEIVED from 'N0CALL-7' type ${TYPE_MESSAGE}: 'meet on 146.52'"
expect "position report" \
    "APRS_RECEIVED from 'N0CALL-7' type ${TYPE_POSITION}: '!4903.50N/07201.75W-test position'"
expect "status report" \
    "APRS_RECEIVED from 'N0CALL-7' type ${TYPE_STATUS}: '>monitoring 146.52'"

rm -f "${RX_STDERR}"

if [[ ${status} -eq 0 ]]; then
    echo "PASS: all three packets reached the inbox."
else
    echo "FAIL: see above." >&2
fi
exit ${status}
