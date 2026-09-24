#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# DMR AMBE codec installer.
#
# OpenRTX ships without the DMR voice codec: it is a proprietary AMBE+2 vocoder
# that cannot be redistributed. This tool takes an OpenRTX firmware image and a
# user-supplied "donor" firmware, extracts the codec from the donor, and merges
# it into the image at the fixed address the firmware expects. The resulting
# image can be flashed to get DMR voice; without it, DMR runs with voice
# disabled (header-only RX, no TX audio).
#
# This script contains NO codec bytes. It only knows *where* the codec lives and
# *how* to move it out of the donor the user provides. The donor is decrypted
# with radio_tool, the same tool OpenRTX uses to wrap and flash its own images.
#
# The reference donor is TYT's MD9600 firmware V26.45 ("MD9600-CSV(2571V5)-
# V26.45.bin"), the same one the OpenGD77 project uses; one donor serves the
# whole MD-9600 / MD-UV380 / RT-3S family because they share the STM32F405 and
# the codec. Obtain it from TYT/Retevis. The codec offsets below are specific to
# that build, so a different donor version is rejected unless --any-donor is
# given (and then it must still carry the codec at the expected offset).

import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

# --- Layout facts (not codec data) --------------------------------------------

DONOR_BASE   = 0x0800C000            # donor (TYT) application load address
CODEC_ADDR   = 0x0807537C            # where the codec runs
CODEC_OFFSET = CODEC_ADDR - DONOR_BASE   # 0x6937C, offset within the donor
CODEC_LEN    = 0x48BB0               # 298928 bytes
CODEC_MAGIC  = bytes.fromhex("2de9f04f")  # first word, "push {r4-r11,lr}"
WRAP_MODEL   = "UV3X0"               # radio_tool wrap type for MD-UV380/390
OUTSEC_MAGIC = b"OutSecurityBin"     # TYT wrapped-firmware header

# Targets: OpenRTX image load address, the RAM an initial stack pointer may
# point into, and how the result is flashed. The CS7000 Plus entry is for a
# radio without the OEM bootloader, running OpenRTX from the start of bank 1.
TARGETS = {
    "mduv3x0": dict(base=0x0800C000,
                    sp=((0x10000000, 0x10010000), (0x20000000, 0x20040000)),
                    wrap=True),
    "cs7000p": dict(base=0x08000000,
                    sp=((0x24000000, 0x24080000),),
                    wrap=False),
}

# The reference donor. A matching SHA-256 means the codec is at CODEC_OFFSET.
DONOR_NAME   = "MD9600-CSV(2571V5)-V26.45.bin"
DONOR_SHA256 = "d8a653307222e576ee416ab6d4704d14758fa71c1b0e826ffd840f26266cf11f"


def die(msg):
    print("error: " + msg, file=sys.stderr)
    sys.exit(1)


def find_radio_tool(explicit):
    if explicit:
        if not os.path.isfile(explicit):
            die("radio_tool not found at " + explicit)
        return explicit
    onpath = shutil.which("radio_tool")
    if onpath:
        return onpath
    # The OpenRTX build clones radio_tool as a meson subproject.
    here = os.path.dirname(os.path.abspath(__file__))
    cand = os.path.join(here, "..", "subprojects", "radio_tool", "radio_tool")
    if os.path.isfile(cand):
        return os.path.abspath(cand)
    die("radio_tool not found; build OpenRTX first or pass --radio-tool")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def read_firmware(path, target):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    # Guard against pointing at a wrapped image or the donor by mistake: the raw
    # OpenRTX binary starts with an STM32 vector table (initial stack pointer in
    # RAM), never with the TYT wrapper header.
    if data[:len(OUTSEC_MAGIC)] == OUTSEC_MAGIC:
        die("--firmware looks like a wrapped/donor image; pass the raw "
            "openrtx_mduv3x0_bin, not a .sgl/wrapped file")
    if len(data) < 8:
        die("--firmware is too small to be a firmware image")
    sp = int.from_bytes(data[0:4], "little")
    if not any(lo <= sp < hi for lo, hi in target["sp"]):
        die("--firmware does not start with a valid STM32 stack pointer; is it "
            "the raw OpenRTX binary?")
    return data


def extract_codec(radio_tool, donor, any_donor):
    with open(donor, "rb") as f:
        head = f.read(len(OUTSEC_MAGIC))
    if head != OUTSEC_MAGIC:
        die("donor is not a TYT firmware file (missing OutSecurityBin header)")

    digest = sha256(donor)
    if digest != DONOR_SHA256:
        if not any_donor:
            die("donor SHA-256 does not match the reference " + DONOR_NAME +
                "\n  expected " + DONOR_SHA256 + "\n  got      " + digest +
                "\n  use --any-donor to try it anyway (codec offset may differ)")
        print("warning: donor is not the reference " + DONOR_NAME +
              "; proceeding because --any-donor was given")

    tmp = tempfile.mkdtemp(prefix="dmr_donor_")
    try:
        prefix = os.path.join(tmp, "donor")
        subprocess.run([radio_tool, "--unwrap", "--in", donor, "--out", prefix],
                       check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        segs = glob.glob(prefix + "*0x0800c000") + glob.glob(prefix + "*0x0800C000")
        if not segs:
            die("radio_tool did not produce a 0x0800c000 application segment")
        with open(segs[0], "rb") as f:
            app = f.read()
        end = CODEC_OFFSET + CODEC_LEN
        if len(app) < end:
            die("donor application is too short to hold the codec")
        if app[CODEC_OFFSET:CODEC_OFFSET + 4] != CODEC_MAGIC:
            die("codec not found at the expected offset in the donor "
                "(wrong donor version?)")
        return app[CODEC_OFFSET:end]
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def merge(firmware, codec, base):
    offset = CODEC_ADDR - base
    end = offset + CODEC_LEN
    merged = bytearray(firmware)
    if len(merged) > offset:
        # The firmware must not already occupy the codec region (the linker
        # reserves it); only trailing erased padding is acceptable.
        if any(b != 0xFF for b in merged[offset:end]):
            die("firmware image overlaps the codec region; wrong image?")
    merged += b"\xFF" * (offset - len(merged))
    merged[offset:end] = codec
    assert merged[offset:offset + 4] == CODEC_MAGIC
    return merged


def wrap(radio_tool, raw_path, out_path, base):
    subprocess.run([radio_tool, "--wrap", "-o", out_path, "-r", WRAP_MODEL,
                    "-s", "0x%X:%s" % (base, raw_path)], check=True)


def main():
    ap = argparse.ArgumentParser(description="Merge the DMR AMBE codec from a "
                                 "donor firmware into an OpenRTX image.")
    ap.add_argument("--target", choices=sorted(TARGETS), default="mduv3x0",
                    help="radio the image is for (default mduv3x0)")
    ap.add_argument("--firmware", required=True,
                    help="raw OpenRTX binary (e.g. build_cm4/openrtx_mduv3x0_bin)")
    ap.add_argument("--donor",
                    help="TYT donor firmware (%s). Omit to build a voice-less "
                         "image." % DONOR_NAME)
    ap.add_argument("--output", required=True,
                    help="flashable output image (radio_tool-wrapped for "
                         "mduv3x0, raw for dfu-util on cs7000p)")
    ap.add_argument("--radio-tool", help="path to the radio_tool binary")
    ap.add_argument("--any-donor", action="store_true",
                    help="accept a donor whose SHA-256 does not match the "
                         "reference (codec offset must still line up)")
    ap.add_argument("--flash", action="store_true",
                    help="flash the result after building it")
    args = ap.parse_args()

    target = TARGETS[args.target]
    radio_tool = find_radio_tool(args.radio_tool)
    firmware = read_firmware(args.firmware, target)

    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tf:
        raw_out = tf.name
    try:
        if args.donor:
            codec = extract_codec(radio_tool, args.donor, args.any_donor)
            merged = merge(firmware, codec, target["base"])
            print("codec %d bytes merged at 0x%X (image address 0x%X)"
                  % (len(codec), CODEC_ADDR - target["base"], CODEC_ADDR))
        else:
            merged = firmware
            print("no donor given: building a voice-less DMR image")
        with open(raw_out, "wb") as f:
            f.write(merged)
        if target["wrap"]:
            wrap(radio_tool, raw_out, args.output, target["base"])
        else:
            shutil.copyfile(raw_out, args.output)
        print("wrote " + args.output)
    finally:
        os.unlink(raw_out)

    if target["wrap"]:
        flash = [radio_tool, "-d", "0", "-f", "-i", args.output]
    else:
        flash = ["dfu-util", "-d", "0483:df11", "-a", "0",
                 "-s", "0x%08X" % target["base"], "-D", args.output]
    if args.flash:
        print("flashing " + args.output + " ...")
        subprocess.run(flash, check=True)
    else:
        print("flash with: " + " ".join(flash))


if __name__ == "__main__":
    main()
