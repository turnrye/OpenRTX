#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate Bell 202 AFSK1200 baseband carrying APRS packets.

Takes packets written in TNC2 monitor form::

    N0CALL-7>APRS,WIDE1-1::W1AW     :hello{001

builds the AX.25 UI frame each one describes, and writes the modulated
result as raw signed 16-bit little-endian samples — the format the Linux
emulator's file_source audio driver reads.

Nothing outside the standard library is needed: no sox, no Dire Wolf, and
no recording.  Because the packets are written here rather than captured,
a test using this output can assert on exact callsigns and message text.

Output is deterministic: the same arguments always produce the same bytes.
"""

import argparse
import math
import struct
import sys

# Bell 202 tones and rates.  APRS_SAMPLE_RATE in
# openrtx/include/protocols/APRS/constants.h must match SAMPLE_RATE.
SAMPLE_RATE = 9600
BAUD = 1200
MARK_HZ = 1200
SPACE_HZ = 2200

FLAG = 0x7E
CONTROL_UI = 0x03
PID_NO_L3 = 0xF0


def ax25_address(call, ssid, last, command=False):
    """Encode one AX.25 address: six shifted-ASCII characters plus flags."""
    if len(call) > 6:
        raise ValueError(f"callsign too long: {call}")
    if not 0 <= ssid <= 15:
        raise ValueError(f"SSID out of range: {ssid}")

    out = bytearray((ord(c) << 1) & 0xFF for c in call.ljust(6))
    # CRRSSSSL: command/response, two reserved bits sent as ones, SSID, and
    # the last-address marker.
    flags = 0x60 | (ssid << 1)
    if command:
        flags |= 0x80
    if last:
        flags |= 0x01
    out.append(flags)
    return bytes(out)


def split_call(text):
    """Split "CALL-SSID" into its parts; a missing SSID means zero."""
    if "-" in text:
        call, _, ssid = text.partition("-")
        return call.upper(), int(ssid)
    return text.upper(), 0


def parse_tnc2(line):
    """Build the AX.25 frame described by one TNC2 monitor line."""
    header, _, info = line.partition(":")
    if not info:
        raise ValueError(f"no info field in: {line}")

    source, _, rest = header.partition(">")
    if not rest:
        raise ValueError(f"no destination in: {line}")
    path = rest.split(",")
    dest = path[0]
    digis = path[1:]

    addresses = [dest, source] + digis
    frame = bytearray()
    for i, addr in enumerate(addresses):
        call, ssid = split_call(addr.rstrip("*"))
        # The destination carries the command bit; only the final address
        # carries the last-address marker.
        frame += ax25_address(call, ssid, last=(i == len(addresses) - 1),
                              command=(i == 0))

    frame.append(CONTROL_UI)
    frame.append(PID_NO_L3)
    frame += info.encode("ascii")
    return bytes(frame)


def _crc_register(data):
    """CRC-16/X.25 register value, before the final inversion."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc


def fcs(data):
    """CRC-16/X.25 frame check sequence, sent least significant byte first."""
    return _crc_register(data) ^ 0xFFFF


def to_bits(frame, flags_before, flags_after):
    """Frame to a stuffed NRZI-ready bit list, least significant bit first.

    Flags delimit the frame and are never stuffed; everything between them
    gets a zero inserted after five consecutive ones so no flag pattern can
    appear inside the data.
    """
    bits = []

    def emit(byte, stuff):
        nonlocal ones
        for i in range(8):
            bit = (byte >> i) & 1
            bits.append(bit)
            if not stuff:
                continue
            ones = ones + 1 if bit else 0
            if ones == 5:
                bits.append(0)
                ones = 0

    ones = 0
    for _ in range(flags_before):
        ones = 0
        emit(FLAG, stuff=False)

    ones = 0
    payload = frame + struct.pack("<H", fcs(frame))
    for byte in payload:
        emit(byte, stuff=True)

    for _ in range(flags_after):
        ones = 0
        emit(FLAG, stuff=False)

    return bits


def nrzi(bits):
    """NRZI encode: a zero flips the output level, a one holds it."""
    level = 1
    out = []
    for bit in bits:
        if bit == 0:
            level ^= 1
        out.append(level)
    return out


def modulate(levels, samples, amplitude):
    """Continuous-phase FSK: level 1 is the mark tone, level 0 the space."""
    phase = 0.0
    out = bytearray()
    for level in levels:
        freq = MARK_HZ if level else SPACE_HZ
        step = 2.0 * math.pi * freq / SAMPLE_RATE
        for _ in range(samples):
            out += struct.pack("<h", int(amplitude * math.sin(phase)))
            phase += step
            if phase >= 2.0 * math.pi:
                phase -= 2.0 * math.pi
    return bytes(out)


def silence(seconds):
    return bytes(2 * int(SAMPLE_RATE * seconds))


def self_check():
    """Round-trip the pieces that are easy to get subtly wrong."""
    # The CRC-16/X.25 check value: "123456789" must produce 0x906E.
    value = fcs(b"123456789")
    if value != 0x906E:
        print(f"FAIL: FCS check value is {value:#06x}, expected 0x906e")
        return 1

    # Running the CRC over a frame followed by its own FCS must leave the
    # register holding the residue — this is exactly the check a receiver
    # does, so getting it right here means the decoder will accept the
    # frames this script produces.
    frame = parse_tnc2("N0CALL-7>APRS::W1AW     :hello{001")
    codeword = frame + struct.pack("<H", fcs(frame))
    if _crc_register(codeword) != 0xF0B8:
        print("FAIL: FCS residue is wrong")
        return 1

    # Addresses are shifted ASCII, and the last one sets the low bit.
    addr = ax25_address("N0CALL", 7, last=True)
    if addr[:6] != bytes((ord(c) << 1) for c in "N0CALL") or addr[6] & 1 != 1:
        print("FAIL: address encoding is wrong")
        return 1

    # Bit stuffing must break up any run of six ones outside the flags.
    bits = to_bits(b"\xff\xff\xff", flags_before=1, flags_after=1)
    run = best = 0
    for bit in bits[8:-8]:
        run = run + 1 if bit else 0
        best = max(best, run)
    if best > 5:
        print(f"FAIL: bit stuffing left a run of {best} ones")
        return 1

    print("self-check passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output",
                        help="file to write, or '-' for stdout")
    parser.add_argument("-p", "--packet", action="append", default=[],
                        metavar="TNC2",
                        help="packet in TNC2 form; repeatable")
    parser.add_argument("--lead", type=float, default=0.5,
                        metavar="SECONDS",
                        help="silence before the first packet (default 0.5)")
    parser.add_argument("--gap", type=float, default=0.5, metavar="SECONDS",
                        help="silence between packets (default 0.5)")
    parser.add_argument("--tail", type=float, default=0.5, metavar="SECONDS",
                        help="silence after the last packet (default 0.5)")
    parser.add_argument("--preamble", type=int, default=32, metavar="FLAGS",
                        help="opening flags per packet (default 32)")
    parser.add_argument("--amplitude", type=int, default=300,
                        metavar="PEAK",
                        help="peak sample value (default 300).  The "
                             "demodulator's correlators run in int16 "
                             "arithmetic and saturate on a strong signal: a "
                             "pure tone above roughly 500 peak stops "
                             "decoding.  Anything from about 100 to 500 "
                             "decodes identically")
    parser.add_argument("--self-check", action="store_true",
                        help="verify the encoders and exit")
    args = parser.parse_args()

    if args.self_check:
        return self_check()

    if not args.packet:
        parser.error("at least one --packet is required")
    if not args.output:
        parser.error("--output is required")

    if SAMPLE_RATE % BAUD:
        raise SystemExit("sample rate must be a whole number of samples/symbol")
    samples_per_symbol = SAMPLE_RATE // BAUD

    audio = bytearray(silence(args.lead))
    for i, text in enumerate(args.packet):
        if i:
            audio += silence(args.gap)
        frame = parse_tnc2(text)
        bits = to_bits(frame, flags_before=args.preamble, flags_after=4)
        audio += modulate(nrzi(bits), samples_per_symbol, args.amplitude)
    audio += silence(args.tail)

    if args.output == "-":
        sys.stdout.buffer.write(audio)
    else:
        with open(args.output, "wb") as handle:
            handle.write(audio)
        seconds = len(audio) / 2 / SAMPLE_RATE
        print(f"wrote {args.output}: {len(audio)} bytes, "
              f"{seconds:.2f} s at {SAMPLE_RATE} S/s")

    return 0


if __name__ == "__main__":
    sys.exit(main())
