#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# Decimate a 48 kHz S16LE raw baseband file to 24 kHz.
#
# The OpenRTX M17 modulator (TX) outputs baseband at 48 kHz (10 samples per
# symbol), while the demodulator (RX) expects 24 kHz (5 samples per symbol).
# This script performs a simple 2:1 decimation so that a TX capture can be
# fed back into the RX path for loopback testing.
#
# Usage:
#   ./scripts/decimate_baseband.py /tmp/baseband_tx.raw /tmp/baseband.raw
#
# The input and output files are headerless signed 16-bit little-endian (S16LE)
# mono audio, the same format used by file_source and file_sink drivers.

import argparse
import struct
import sys


def decimate(input_path: str, output_path: str, factor: int) -> None:
    """Read an S16LE raw file and write every `factor`-th sample."""

    sample_size = struct.calcsize("<h")

    with open(input_path, "rb") as fin:
        data = fin.read()

    if len(data) % sample_size != 0:
        print(
            f"Warning: input size ({len(data)} bytes) is not a multiple of "
            f"{sample_size}; trailing bytes will be discarded.",
            file=sys.stderr,
        )

    total_samples = len(data) // sample_size
    samples = struct.unpack_from(f"<{total_samples}h", data)

    decimated = samples[::factor]

    out_data = struct.pack(f"<{len(decimated)}h", *decimated)

    with open(output_path, "wb") as fout:
        fout.write(out_data)

    duration_in = total_samples / 48000.0
    duration_out = len(decimated) / 24000.0
    print(f"Input:  {total_samples} samples @ 48 kHz ({duration_in:.3f} s)")
    print(f"Output: {len(decimated)} samples @ 24 kHz ({duration_out:.3f} s)")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Decimate a 48 kHz S16LE raw baseband file to 24 kHz "
        "for OpenRTX M17 RX loopback testing."
    )
    parser.add_argument(
        "input",
        help="Path to the 48 kHz S16LE input file (e.g. /tmp/baseband_tx.raw)",
    )
    parser.add_argument(
        "output",
        help="Path for the 24 kHz S16LE output file (e.g. /tmp/baseband.raw)",
    )
    parser.add_argument(
        "-f",
        "--factor",
        type=int,
        default=2,
        help="Decimation factor (default: 2, i.e. 48 kHz → 24 kHz)",
    )

    args = parser.parse_args()
    decimate(args.input, args.output, args.factor)


if __name__ == "__main__":
    main()
