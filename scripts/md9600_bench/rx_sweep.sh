#!/bin/bash
# FM copy receive sweep: HackRF sends the M17 loop at falling levels, the radio
# captures its HR_C6000 FM copy, the bench decoder (copy_measure.py) measures it.
#   rx_sweep.sh CONSOLE_DIR IQ_BASE FREQ_HZ [invert]
# IQ_BASE.cs8 plus IQ_BASE-m10/-m20/-m30.cs8 (digital back-off) must exist.
set -u
D=$1; BASE=$2; F=$3; INV=${4:-}
TUNE=$((F - 250000))
HERE=$(cd "$(dirname "$0")" && pwd)

run() {   # label file vga
    local label=$1 file=$2 vga=$3
    hackrf_transfer -t "$file" -f $TUNE -s 2000000 -x "$vga" -a 0 -R > /dev/null 2>&1 &
    local hp=$!
    python3 -c "import time; time.sleep(1.2)"
    python3 -I "$HERE/send.py" "$D" "fmcap 3000 $INV" | grep FMCAP | sed 's/ saved.*//'
    kill -INT $hp; wait $hp 2>/dev/null
    # The capture the radio just dumped (take numbers restart at each boot)
    local wav
    wav=$(grep -E "END [0-9]+ -> " "$D/serial.log" | tail -1 | awk '{print $5}')
    echo -n "$label: "
    (cd ~/workspace/openrtx-acceptance-testing && \
     uv run python "$HERE/copy_measure.py" 6.9 "$wav" 2>/dev/null | tail -1)
}

run "vga30"      "$BASE.cs8"     30
run "vga20"      "$BASE.cs8"     20
run "vga10"      "$BASE.cs8"     10
run "vga0"       "$BASE.cs8"     0
run "vga0-10dB"  "$BASE-m10.cs8" 0
run "vga0-20dB"  "$BASE-m20.cs8" 0
run "vga0-30dB"  "$BASE-m30.cs8" 0
