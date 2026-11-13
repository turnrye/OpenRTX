#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
"""Flash an OpenRTX image into a TYT MD-9600 through its bootloader, then restart the radio into it.

    md9600_flash.py openrtx_md9600_bin [--key FILE] [--dry-run]

Takes the plain image the build produces (openrtx_md9600_bin) and encrypts it for the bootloader with the MD-9600
key, read from radio_tool's cipher table in subprojects/ (checked by SHA-256) unless --key gives a 1024-byte key
file. Put the radio in its bootloader first: Settings > Flash Mode, or hold P1 and the orange key while connecting
power. Unlike radio_tool, this sends the bootloader's exit command at the end, so the radio restarts without a power
cycle.

Writes only the flash sectors the image covers, from 0x0800C000 up and below the settings sector; the bootloader
(sectors 0-2) is never erased. The vector table is written last, so an interrupted transfer leaves no image the
bootloader would start. Needs pyusb and libusb.

The bootloader protocol is as described by RadioFoundry (MIT), docs/spec/hardware-dm1701.md section 15.2.
"""
import argparse
import hashlib
import re
import struct
import sys
import time
from pathlib import Path

APP_BASE = 0x0800C000
APP_END = 0x080E0000        # settings sector (11) starts here
EXIT_ADDR = 0x08000000
SECTORS = [(0x0800C000, 0x4000), (0x08010000, 0x10000)] + \
          [(0x08020000 + i * 0x20000, 0x20000) for i in range(6)]
BLOCK = 1024
FIRST_BLOCK = 2
LOADER_IF = "@Internal Flash /0x0800C000/01*016Kg,01*064Kg,07*128Kg"
KEY_SHA256 = "c3d665b297651b52310ec098e87440b147136241359c1154d474e517b048c9ae"
KEY_HEADER = Path(__file__).resolve().parent.parent / \
    "subprojects/radio_tool/include/radio_tool/fw/cipher/md9600.hpp"
DNLOAD, GETSTATUS, CLRSTATUS, ABORT = 1, 3, 4, 6
ST_IDLE, ST_DNBUSY, ST_DNIDLE, ST_ERROR = 2, 4, 5, 10


class Refused(Exception):
    """A request, device or image this script won't use."""


def norm(s):
    """Collapse whitespace, as interface strings are compared."""
    return " ".join(str(s or "").split())


def load_key(path=None):
    """The 1024-byte MD-9600 key, from a key file or radio_tool's cipher table, checked by SHA-256."""
    if path:
        key = Path(path).read_bytes()
    else:
        if not KEY_HEADER.exists():
            raise Refused(f"{KEY_HEADER} not found: fetch the subprojects or pass --key")
        text = KEY_HEADER.read_text()
        body = text[text.index("{", text.index("md9600[")) + 1:]
        key = bytes(int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]{2}", body[:body.index("}")]))
    if len(key) != 1024 or hashlib.sha256(key).hexdigest() != KEY_SHA256:
        raise Refused("the key doesn't match the MD-9600 key's SHA-256")
    return key


def read_image(path, key):
    """The plain image, checked for the MD-9600 bootloader, encrypted from 0x0800C000."""
    plain = Path(path).read_bytes()
    if plain[:14] == b"OutSecurityBin":
        raise Refused(f"{path} is a wrapped image: pass the plain openrtx_md9600_bin")
    if APP_BASE + len(plain) > APP_END:
        raise Refused(f"image ends at {APP_BASE + len(plain):#x}, past the settings sector at {APP_END:#x}")
    sp, reset = struct.unpack("<II", plain[:8])
    if (sp & 0x2FFE0000) != 0x20000000:
        raise Refused(f"initial SP {sp:#x} fails the bootloader's check: the radio would stay in its bootloader")
    if not (reset & 1) or not (APP_BASE <= (reset & ~1) < APP_BASE + len(plain)):
        raise Refused(f"reset vector {reset:#x} is not inside an image linked for {APP_BASE:#x}")
    return bytes(b ^ key[i % 1024] for i, b in enumerate(plain))


def plan(image):
    """(sector address, sector size, [(block number, 1024 bytes)]) for each sector the image covers."""
    out = []
    for addr, size in SECTORS:
        off = addr - APP_BASE
        if off >= len(image):
            break
        chunk = image[off:off + size]
        blocks = []
        for i in range(0, len(chunk), BLOCK):
            blk = chunk[i:i + BLOCK]
            blocks.append((FIRST_BLOCK + i // BLOCK, blk + b"\xFF" * (BLOCK - len(blk))))
        out.append((addr, size, blocks))
    return out


class Guard:
    """Allowlist over a pyusb-like device: only the planned sectors and the exit reach it."""

    def __init__(self, dev, intf, sectors):
        self.dev, self.intf = dev, intf
        self.sectors = {addr: len(blocks) for addr, _, blocks in sectors}
        self.addr = None
        self.written = set()

    def done(self):
        """Whether every planned block has been written."""
        return len(self.written) == sum(self.sectors.values())

    def out(self, req, value, data=b""):
        """Class OUT request, after the allowlist check."""
        data = bytes(data or b"")
        if req in (CLRSTATUS, ABORT):
            ok = value == 0 and not data
        elif req != DNLOAD:
            ok = False
        elif value == 0 and data in (b"\x91\x01", b"\x91\x31"):
            ok = True
        elif value == 0 and len(data) == 5 and data[0] == 0x41:
            ok = struct.unpack("<I", data[1:])[0] in self.sectors
        elif value == 0 and len(data) == 5 and data[0] == 0x21:
            a = struct.unpack("<I", data[1:])[0]
            ok = a in self.sectors or (a == EXIT_ADDR and self.done())
            if ok:
                self.addr = a
        elif value == 0 and not data:
            ok = self.addr == EXIT_ADDR and self.done()
        else:
            key = (self.addr, value)
            ok = (self.addr in self.sectors and FIRST_BLOCK <= value < FIRST_BLOCK + self.sectors[self.addr]
                  and len(data) == BLOCK and key not in self.written)
            if ok:
                self.written.add(key)
        if not ok:
            raise Refused(f"refused: request {req}, wValue {value}, {len(data)} bytes [{data[:5].hex(' ')}]")
        return self.dev.ctrl_transfer(0x21, req, value, self.intf, data, 5000)

    def status(self):
        """GETSTATUS: (status, poll_ms, state)."""
        r = bytes(self.dev.ctrl_transfer(0xA1, GETSTATUS, 0, self.intf, 6, 5000))
        return r[0], r[1] | (r[2] << 8) | (r[3] << 16), r[4]


class Loader:
    """The bootloader's write sequence."""

    def __init__(self, guard, log=print, sleep=time.sleep):
        self.g, self.log, self.sleep = guard, log, sleep

    def wait_idle(self):
        """Until dfuIDLE: CLRSTATUS, then ABORT as a fallback."""
        for attempt in range(20):
            _, _, state = self.g.status()
            if state == ST_IDLE:
                return
            self.g.out(CLRSTATUS if attempt < 10 else ABORT, 0)
        raise RuntimeError(f"device stays in state {state}, not dfuIDLE")

    def wait_done(self, what):
        """After a DNLOAD: dnBUSY (maybe skipped), then dnIDLE."""
        for _ in range(4000):
            status, poll, state = self.g.status()
            if state == ST_ERROR or status:
                raise RuntimeError(f"{what}: status {status}, state {state}")
            if state == ST_DNIDLE:
                return
            if state != ST_DNBUSY:
                raise RuntimeError(f"{what}: unexpected state {state}")
            self.sleep(max(poll, 1) / 1000)
        raise RuntimeError(f"{what}: stayed busy")

    def vendor(self, b1):
        """Vendor command 91 xx."""
        self.g.out(DNLOAD, 0, bytes([0x91, b1]))
        self.g.status()
        self.sleep(0.1)
        self.g.status()
        self.wait_idle()

    def command(self, cmd, addr, what):
        """A 5-byte command: 41 (erase) or 21 (set address)."""
        self.g.out(DNLOAD, 0, bytes([cmd]) + struct.pack("<I", addr))
        self.wait_done(what)

    def write(self, sectors):
        """Erase the image's sectors, write them with the vector table last, exit."""
        self.wait_idle()
        self.vendor(0x01)
        self.vendor(0x31)
        for addr, _, _ in sectors:
            self.log(f"erase {addr:#010x}")
            self.command(0x41, addr, f"erase {addr:#x}")
        for addr, _, blocks in sectors:
            self.command(0x21, addr, f"set address {addr:#x}")
            for num, blk in blocks:
                if addr == APP_BASE and num == FIRST_BLOCK:
                    continue
                self.g.out(DNLOAD, num, blk)
                self.wait_done(f"block {num} at {addr:#x}")
            self.log(f"wrote {addr:#010x}")
        first = sectors[0][2][0][1]
        self.command(0x21, APP_BASE, "set address 0x0800C000")
        self.g.out(DNLOAD, FIRST_BLOCK, first)
        self.wait_done("vector table")
        self.command(0x21, EXIT_ADDR, "set address 0x08000000")
        self.g.out(DNLOAD, 0, b"")
        try:
            self.g.status()
        except Exception:
            pass
        self.log("done: the radio restarts into the new firmware")


def open_bootloader():
    """The radio's bootloader as a pyusb device, or Refused."""
    import usb.backend.libusb1
    import usb.core
    import usb.util
    backend = usb.backend.libusb1.get_backend()
    if backend is None:
        backend = usb.backend.libusb1.get_backend(find_library=lambda x: "/opt/homebrew/lib/libusb-1.0.dylib")
    devs = list(usb.core.find(find_all=True, idVendor=0x0483, idProduct=0xDF11, backend=backend))
    if len(devs) != 1:
        raise Refused(f"expected one 0483:DF11 device, found {len(devs)}: is the radio in its bootloader?")
    dev = devs[0]
    maker = usb.util.get_string(dev, dev.iManufacturer) if dev.iManufacturer else ""
    if "AnyRoad" not in (maker or ""):
        raise Refused(f"manufacturer {maker!r}: not the MD-9600 bootloader")
    strings = [norm(usb.util.get_string(dev, i.iInterface)) for i in dev.get_active_configuration() if i.iInterface]
    if norm(LOADER_IF) not in strings:
        raise Refused("the radio isn't in its bootloader: use Settings > Flash Mode, or hold P1 and the orange key "
                      "while connecting power")
    intf = dev.get_active_configuration()[(0, 0)].bInterfaceNumber
    usb.util.claim_interface(dev, intf)
    return dev, intf


def main():
    """Command line entry point."""
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="openrtx_md9600_bin (the plain image, not the _wrap file)")
    ap.add_argument("--key", help="1024-byte MD-9600 key file (default: radio_tool's cipher table)")
    ap.add_argument("--dry-run", action="store_true", help="check the image and print the plan; no USB")
    a = ap.parse_args()
    sectors = plan(read_image(a.image, load_key(a.key)))
    print("sectors: " + ", ".join(f"{addr:#010x} ({len(b)} KB)" for addr, _, b in sectors))
    if a.dry_run:
        return
    dev, intf = open_bootloader()
    Loader(Guard(dev, intf, sectors)).write(sectors)


if __name__ == "__main__":
    try:
        main()
    except Refused as e:
        sys.exit(f"refused: {e}")
