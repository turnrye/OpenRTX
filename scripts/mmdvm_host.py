#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
"""
Reference host for an OpenRTX radio in M17 hotspot mode.

The radio speaks the MMDVM modem protocol over its host serial port (USB
virtual com port on hardware, a pseudo-terminal on the Linux emulator). This
tool is the smallest possible host: it runs the handshake, polls the status,
prints every frame the radio demodulates with the link setup data decoded,
and can transmit a test stream. It needs only the Python standard library.

Examples:

    mmdvm_host.py /dev/ttyACM0 probe
    mmdvm_host.py /dev/ttyACM0 monitor --rx 433475000 --tx 433475000
    mmdvm_host.py /dev/ttyACM0 tx --src N0CALL --dst ALL --frames 25
"""

import argparse
import os
import select
import struct
import sys
import termios
import time

# --------------------------------------------------------------------------
# MMDVM protocol constants
# --------------------------------------------------------------------------

FRAME_START = 0xE0
GET_VERSION = 0x00
GET_STATUS = 0x01
SET_CONFIG = 0x02
SET_MODE = 0x03
SET_FREQ = 0x04
M17_LINK_SETUP = 0x45
M17_STREAM = 0x46
M17_PACKET = 0x47
M17_LOST = 0x48
M17_EOT = 0x49
ACK = 0x70
NAK = 0x7F

MODE_IDLE = 0
MODE_M17 = 7

FRAME_NAMES = {
    M17_LINK_SETUP: "LSF",
    M17_STREAM: "STREAM",
    M17_PACKET: "PACKET",
    M17_LOST: "LOST",
    M17_EOT: "EOT",
}

# --------------------------------------------------------------------------
# M17 coding, enough to build and read link setup and stream frames
# --------------------------------------------------------------------------

LSF_SYNC = bytes([0x55, 0xF7])
STREAM_SYNC = bytes([0xFF, 0x5D])
PACKET_SYNC = bytes([0x75, 0xFF])
EOT_SYNC = bytes([0x55, 0x5D])

LSF_PUNCTURE = [1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1,
                1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1,
                1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1,
                1]
DATA_PUNCTURE = [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0]

RANDOMIZER = bytes([
    0xd6, 0xb5, 0xe2, 0x30, 0x82, 0xFF, 0x84, 0x62, 0xba, 0x4e, 0x96, 0x90,
    0xd8, 0x98, 0xdd, 0x5d, 0x0c, 0xc8, 0x52, 0x43, 0x91, 0x1d, 0xf8, 0x6e,
    0x68, 0x2F, 0x35, 0xda, 0x14, 0xea, 0xcd, 0x76, 0x19, 0x8d, 0xd5, 0x80,
    0xd1, 0x33, 0x87, 0x13, 0x57, 0x18, 0x2d, 0x29, 0x78, 0xc3])

GOLAY_MATRIX = [0x8eb, 0x93e, 0xa97, 0xdc6, 0x367, 0x6cd,
                0xd99, 0x3da, 0x7b4, 0xf68, 0x63b, 0xc75]

CALL_CHARS = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/."


def bytes_to_bits(data):
    return [(b >> (7 - i)) & 1 for b in data for i in range(8)]


def bits_to_bytes(bits):
    out = bytearray(len(bits) // 8)
    for i, bit in enumerate(bits):
        if bit:
            out[i >> 3] |= 0x80 >> (i & 7)
    return bytes(out)


def crc_m17(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x5935) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def encode_callsign(call):
    call = call.upper()
    if call == "ALL" or call == "@ALL":
        return b"\xFF" * 6
    if call == "":
        return b"\x00" * 6
    value = 0
    for ch in reversed(call[:9]):
        value = value * 40 + CALL_CHARS.index(ch)
    return value.to_bytes(6, "big")


def decode_callsign(data):
    value = int.from_bytes(data, "big")
    if value == 0xFFFFFFFFFFFF:
        return "ALL"
    if value == 0:
        return ""
    out = ""
    while value and len(out) < 9:
        out += CALL_CHARS[value % 40]
        value //= 40
    return out


def conv_encode(data):
    """K=5 rate-1/2 convolutional code, G1=0x19 G2=0x17, four flush bits."""
    bits = bytes_to_bits(data) + [0, 0, 0, 0]
    memory = 0
    out = []
    for bit in bits:
        memory = ((memory << 1) | bit) & 0x1F
        out.append(bin(memory & 0x19).count("1") & 1)
        out.append(bin(memory & 0x17).count("1") & 1)
    return out


def puncture(bits, pattern):
    return [b for i, b in enumerate(bits) if pattern[i % len(pattern)]]


def depuncture(bits, pattern, out_len):
    """Returns (bits, erasure mask) of out_len coded bits."""
    out, mask = [], []
    idx = 0
    for i in range(out_len):
        if pattern[i % len(pattern)]:
            out.append(bits[idx] if idx < len(bits) else 0)
            mask.append(1)
            idx += 1
        else:
            out.append(0)
            mask.append(0)
    return out, mask


def viterbi_decode(coded, mask, n_in):
    """Hard-decision Viterbi for the K=5 code; erased bits carry no cost."""
    n_states = 16
    inf = 1 << 30
    metric = [inf] * n_states
    metric[0] = 0
    history = []
    for t in range(n_in):
        c0, c1 = coded[2 * t], coded[2 * t + 1]
        m0, m1 = mask[2 * t], mask[2 * t + 1]
        new = [inf] * n_states
        prev = [0] * n_states
        for state in range(n_states):
            if metric[state] >= inf:
                continue
            for bit in (0, 1):
                mem = ((state << 1) | bit) & 0x1F
                e0 = bin(mem & 0x19).count("1") & 1
                e1 = bin(mem & 0x17).count("1") & 1
                cost = metric[state]
                cost += m0 * (e0 != c0) + m1 * (e1 != c1)
                nxt = mem & 0x0F
                if cost < new[nxt]:
                    new[nxt] = cost
                    prev[nxt] = state
        metric = new
        history.append(prev)
    # Flush bits bring the encoder back to state 0.
    state = 0
    bits = []
    for t in range(n_in - 1, -1, -1):
        p = history[t][state]
        bits.append(state & 1)
        state = p
    bits.reverse()
    return bits[:n_in - 4], min(metric)


def interleave(data):
    bits = bytes_to_bits(data)
    n = len(bits)
    out = [0] * n
    for i in range(n):
        out[(45 * i + 92 * i * i) % n] = bits[i]
    return bits_to_bytes(out)


def deinterleave(data):
    bits = bytes_to_bits(data)
    n = len(bits)
    out = [0] * n
    for i in range(n):
        out[i] = bits[(45 * i + 92 * i * i) % n]
    return bits_to_bytes(out)


def randomize(data):
    return bytes(a ^ b for a, b in zip(data, RANDOMIZER))


def golay_encode(value):
    checksum = 0
    for i in range(12):
        if value & (1 << i):
            checksum ^= GOLAY_MATRIX[i]
    return (value << 12) | checksum


def build_lsf(src, dst, stream_type=0x0005, meta=b"\x00" * 14):
    lsf = encode_callsign(dst) + encode_callsign(src)
    lsf += struct.pack(">H", stream_type) + meta
    lsf += struct.pack(">H", crc_m17(lsf))
    return lsf


def encode_lsf_frame(lsf):
    coded = puncture(conv_encode(lsf), LSF_PUNCTURE)
    return LSF_SYNC + randomize(interleave(bits_to_bytes(coded)))


def lich_segment(lsf, num):
    chunk = lsf[num * 5:num * 5 + 5]
    blocks = [
        (chunk[0] << 4) | (chunk[1] >> 4),
        ((chunk[1] & 0x0F) << 8) | chunk[2],
        (chunk[3] << 4) | (chunk[4] >> 4),
        ((chunk[4] & 0x0F) << 8) | (num << 5),
    ]
    out = b""
    for block in blocks:
        out += golay_encode(block).to_bytes(3, "big")
    return out


def encode_stream_frame(lsf, frame_number, payload, lich_num, last=False):
    fn = frame_number & 0x7FFF
    if last:
        fn |= 0x8000
    data = struct.pack(">H", fn) + payload
    coded = puncture(conv_encode(data), DATA_PUNCTURE)
    body = lich_segment(lsf, lich_num) + bits_to_bytes(coded)
    return STREAM_SYNC + randomize(interleave(body))


def eot_frame():
    return EOT_SYNC * 24


def preamble_frame():
    return bytes([0x77]) * 48


def decode_lsf(payload):
    """payload: 46 bytes after the sync word. Returns (lsf, errors)."""
    data = deinterleave(randomize(payload))
    coded, mask = depuncture(bytes_to_bits(data), LSF_PUNCTURE, 488)
    bits, errors = viterbi_decode(coded, mask, 244)
    return bits_to_bytes(bits), errors


def decode_stream(payload):
    """Returns (lich chunk, lich number, frame number, payload, errors)."""
    data = deinterleave(randomize(payload))
    lich = data[:12]
    blocks = [int.from_bytes(lich[3 * i:3 * i + 3], "big") >> 12
              for i in range(4)]
    chunk = bytes([
        blocks[0] >> 4,
        ((blocks[0] & 0x0F) << 4) | (blocks[1] >> 8),
        blocks[1] & 0xFF,
        blocks[2] >> 4,
        ((blocks[2] & 0x0F) << 4) | (blocks[3] >> 8),
    ])
    num = (blocks[3] >> 5) & 0x07
    coded, mask = depuncture(bytes_to_bits(data[12:]), DATA_PUNCTURE, 296)
    bits, errors = viterbi_decode(coded, mask, 148)
    frame = bits_to_bytes(bits)
    fn = struct.unpack(">H", frame[:2])[0]
    return chunk, num, fn, frame[2:18], errors


def describe_lsf(lsf):
    dst = decode_callsign(lsf[0:6])
    src = decode_callsign(lsf[6:12])
    stype = struct.unpack(">H", lsf[12:14])[0]
    ok = struct.unpack(">H", lsf[28:30])[0] == crc_m17(lsf[:28])
    kind = "stream" if stype & 1 else "packet"
    can = (stype >> 7) & 0x0F
    return "%s > %s  type=%s/%d CAN=%d crc=%s" % (
        src, dst, kind, (stype >> 1) & 3, can, "ok" if ok else "BAD")


# --------------------------------------------------------------------------
# Serial link
# --------------------------------------------------------------------------

class Link:
    def __init__(self, path, speed=115200, verbose=False):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.verbose = verbose
        self.buf = bytearray()
        try:
            attr = termios.tcgetattr(self.fd)
            attr[0] = 0          # iflag
            attr[1] = 0          # oflag
            attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            attr[3] = 0          # lflag
            rate = getattr(termios, "B%d" % speed, termios.B115200)
            attr[4] = rate
            attr[5] = rate
            attr[6][termios.VMIN] = 0
            attr[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, attr)
            termios.tcflush(self.fd, termios.TCIOFLUSH)
        except termios.error:
            pass

    def write(self, frame):
        if self.verbose:
            print("-> " + frame.hex(" "))
        os.write(self.fd, frame)

    def read_frame(self, timeout):
        """Return the next complete MMDVM frame or None on timeout."""
        deadline = time.monotonic() + timeout
        while True:
            frame = self._pop_frame()
            if frame is not None:
                if self.verbose:
                    print("<- " + frame.hex(" "))
                return frame
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            r, _, _ = select.select([self.fd], [], [], remaining)
            if r:
                try:
                    self.buf += os.read(self.fd, 4096)
                except BlockingIOError:
                    pass

    def _pop_frame(self):
        while self.buf and self.buf[0] != FRAME_START:
            del self.buf[0]
        if len(self.buf) < 3:
            return None
        length = self.buf[1]
        if length == 0:
            length = self.buf[2] + 255
        if length < 3:
            del self.buf[0]
            return None
        if len(self.buf) < length:
            return None
        frame = bytes(self.buf[:length])
        del self.buf[:length]
        return frame


class Modem:
    def __init__(self, link):
        self.link = link
        self.space = 0
        self.status = None
        self.pending = []

    def command(self, frame, expect, timeout=1.0):
        self.link.write(frame)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            reply = self.link.read_frame(deadline - time.monotonic())
            if reply is None:
                break
            if reply[2] == expect:
                return reply
            if reply[2] == NAK and len(reply) >= 5 and reply[3] == frame[2]:
                raise RuntimeError("NAK for command 0x%02X, reason %d"
                                   % (reply[3], reply[4]))
            self.pending.append(reply)
        raise RuntimeError("no reply to command 0x%02X" % frame[2])

    def get_version(self):
        reply = self.command(bytes([FRAME_START, 3, GET_VERSION]),
                             GET_VERSION)
        return reply[3], reply[4:].decode("ascii", "replace")

    def get_status(self):
        reply = self.command(bytes([FRAME_START, 3, GET_STATUS]), GET_STATUS)
        self.status = {
            "modes": reply[3],
            "mode": reply[4],
            "tx": bool(reply[5] & 0x01),
            "cd": bool(reply[5] & 0x40),
            "m17_space": reply[13] if len(reply) > 13 else 0,
        }
        self.space = self.status["m17_space"]
        return self.status

    def set_config(self, tx_delay_ms=100):
        cfg = bytearray(26)
        cfg[0] = FRAME_START
        cfg[1] = 26
        cfg[2] = SET_CONFIG
        cfg[3] = 0x80           # simplex
        cfg[4] = 0x40           # M17 enabled
        cfg[5] = tx_delay_ms // 10
        cfg[6] = MODE_IDLE
        cfg[7] = 128
        cfg[24] = 128           # M17 TX level
        cfg[25] = 5             # M17 TX hang
        self.command(bytes(cfg), ACK)

    def set_freq(self, rx, tx):
        frame = struct.pack("<BBBBII", FRAME_START, 12, SET_FREQ, 0, rx, tx)
        self.command(frame, ACK)

    def set_mode(self, mode):
        self.command(bytes([FRAME_START, 4, SET_MODE, mode]), ACK)

    def send_frame(self, kind, frame):
        self.link.write(bytes([FRAME_START, 4 + len(frame), kind, 0]) + frame)
        self.space = max(0, self.space - 1)

    def poll(self, timeout):
        if self.pending:
            return self.pending.pop(0)
        return self.link.read_frame(timeout)


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------

def handshake(modem, args):
    version, desc = modem.get_version()
    print("modem: protocol %d, %s" % (version, desc))
    modem.set_config()
    if args.rx or args.tx:
        modem.set_freq(args.rx or args.tx, args.tx or args.rx)
    modem.set_mode(MODE_M17)
    status = modem.get_status()
    print("status: mode=%d tx=%s cd=%s m17_space=%d" % (
        status["mode"], status["tx"], status["cd"], status["m17_space"]))
    return status


def print_frame(frame):
    kind = frame[2]
    name = FRAME_NAMES.get(kind, "0x%02X" % kind)
    stamp = time.strftime("%H:%M:%S")
    if kind in (M17_LOST, M17_EOT) and len(frame) < 52:
        print("%s %-6s" % (stamp, name))
        return
    if len(frame) < 52:
        print("%s %-6s short frame (%d bytes)" % (stamp, name, len(frame)))
        return
    first = "first " if frame[3] & 0x01 else ""
    payload = frame[6:52]
    if kind == M17_LINK_SETUP:
        lsf, errors = decode_lsf(payload)
        print("%s %-6s %s%s  viterbi_err=%d" % (
            stamp, name, first, describe_lsf(lsf), errors))
    elif kind == M17_STREAM:
        chunk, num, fn, _, errors = decode_stream(payload)
        print("%s %-6s %sfn=%d%s lich=%d viterbi_err=%d" % (
            stamp, name, first, fn & 0x7FFF, " EOS" if fn & 0x8000 else "",
            num, errors))
    else:
        print("%s %-6s %s%s" % (stamp, name, first, frame[4:52].hex()))


def cmd_probe(modem, args):
    handshake(modem, args)


def cmd_monitor(modem, args):
    handshake(modem, args)
    end = time.monotonic() + args.duration if args.duration else None
    last_status = time.monotonic()
    while end is None or time.monotonic() < end:
        frame = modem.poll(0.25)
        if frame is not None and frame[2] in FRAME_NAMES:
            print_frame(frame)
        if time.monotonic() - last_status >= 0.25:
            status = modem.get_status()
            last_status = time.monotonic()
            if args.show_status:
                print("status: mode=%d tx=%s cd=%s space=%d" % (
                    status["mode"], status["tx"], status["cd"],
                    status["m17_space"]))


def cmd_tx(modem, args):
    handshake(modem, args)
    lsf = build_lsf(args.src, args.dst)
    frames = [(M17_LINK_SETUP, preamble_frame()),
              (M17_LINK_SETUP, encode_lsf_frame(lsf))]
    for i in range(args.frames):
        payload = bytes([i & 0xFF]) * 16
        frames.append((M17_STREAM, encode_stream_frame(
            lsf, i, payload, i % 6, last=(i == args.frames - 1))))
    frames.append((M17_EOT, eot_frame()))

    print("transmitting %d frames as %s > %s" % (len(frames), args.src,
                                                  args.dst))
    sent = 0
    while sent < len(frames):
        if modem.space <= 1:
            modem.get_status()
            if modem.space <= 1:
                time.sleep(0.02)
                continue
        modem.send_frame(*frames[sent])
        sent += 1
        time.sleep(0.04 if sent > 4 else 0)
        frame = modem.poll(0)
        if frame is not None and frame[2] == NAK:
            print("NAK: command 0x%02X reason %d" % (frame[3], frame[4]))

    # Wait for the transmitter to go off.
    for _ in range(50):
        if not modem.get_status()["tx"]:
            break
        time.sleep(0.05)
    print("done, tx=%s" % modem.status["tx"])


def main():
    parser = argparse.ArgumentParser(description=__doc__.strip(),
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port", help="serial port or pseudo-terminal")
    parser.add_argument("--speed", type=int, default=115200)
    parser.add_argument("--rx", type=int, default=0, help="RX frequency, Hz")
    parser.add_argument("--tx", type=int, default=0, help="TX frequency, Hz")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="dump every frame on the wire")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("probe", help="handshake and print the status")

    mon = sub.add_parser("monitor", help="print the frames the radio hears")
    mon.add_argument("--duration", type=float, default=0,
                     help="seconds to run, 0 = forever")
    mon.add_argument("--show-status", action="store_true")

    tx = sub.add_parser("tx", help="transmit a test voice stream")
    tx.add_argument("--src", default="N0CALL")
    tx.add_argument("--dst", default="ALL")
    tx.add_argument("--frames", type=int, default=25,
                    help="number of 40 ms stream frames")

    args = parser.parse_args()
    link = Link(args.port, args.speed, args.verbose)
    modem = Modem(link)

    {"probe": cmd_probe, "monitor": cmd_monitor, "tx": cmd_tx}[args.command](
        modem, args)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
    except RuntimeError as err:
        print("error: %s" % err, file=sys.stderr)
        sys.exit(1)
