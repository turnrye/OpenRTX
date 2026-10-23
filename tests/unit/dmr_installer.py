#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The DMR codec installer (scripts/dmr_installer.py) against a made-up donor
# and fakes of radio_tool and dfu-util: where the codec lands in the MD-UV3x0
# image, how that image comes out and is flashed, and the donors and images it
# turns away. The made-up donor carries the codec's first word followed by a
# pattern; there are no codec bytes here.

import contextlib
import hashlib
import importlib.util
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "..", ".."))
SCRIPT = os.path.join(ROOT, "scripts", "dmr_installer.py")

# Where the firmware runs the codec from, and how long it is.
CODEC_ADDR = 0x0807537C
CODEC_LEN = 0x48BB0

# The MD-UV3x0 image load address, and an initial stack pointer in its RAM.
UV3X0_BASE = 0x0800C000
UV3X0_SP = 0x20020000

# A stack pointer in the CS7000 Plus's RAM, for an image of the wrong radio.
CS7000P_SP = 0x24010000

# The donor application starts at 0x0800C000, like the MD-UV3x0 image.
DONOR_CODEC_OFFSET = CODEC_ADDR - 0x0800C000

# The made-up donor: a TYT header, which the fake radio_tool strips when it
# unwraps, then the application.
DONOR_MAGIC = b"OutSecurityBin"
DONOR_HEADER = 0x100

# What the fake radio_tool puts in front of an image it wraps.
WRAP_HEADER = b"WRAP UV3X0 0x800C000\n"

CODEC = bytes.fromhex("2de9f04f") + bytes((i * 7 + 3) & 0xFF
                                          for i in range(CODEC_LEN - 4))

# The fakes note every call in the log the test hands them. radio_tool unwraps
# the made-up donor into the segment name the installer looks for, and wraps an
# image by putting the model and load address in front of it.
FAKE_RADIO_TOOL = """
import os, sys
args = sys.argv[1:]
with open(os.environ["FAKE_TOOL_LOG"], "a") as log:
    log.write("radio_tool " + " ".join(args) + "\\n")
if "--unwrap" in args:
    with open(args[args.index("--in") + 1], "rb") as f:
        data = f.read()
    with open(args[args.index("--out") + 1] + "_0x0800c000", "wb") as f:
        f.write(data[%d:])
elif "--wrap" in args:
    model = args[args.index("-r") + 1]
    base, raw = args[args.index("-s") + 1].split(":", 1)
    with open(raw, "rb") as f:
        data = f.read()
    with open(args[args.index("-o") + 1], "wb") as f:
        f.write(("WRAP %%s %%s\\n" %% (model, base)).encode() + data)
""" % DONOR_HEADER

FAKE_DFU_UTIL = """
import os, sys
with open(os.environ["FAKE_TOOL_LOG"], "a") as log:
    log.write("dfu-util " + " ".join(sys.argv[1:]) + "\\n")
"""


def donor(codec=CODEC, magic=DONOR_MAGIC):
    app = bytearray(b"\x5A" * DONOR_CODEC_OFFSET) + codec + b"\x5A" * 64
    return magic.ljust(DONOR_HEADER, b"\0") + bytes(app)


def firmware(sp=UV3X0_SP, length=4096):
    return sp.to_bytes(4, "little") + bytes((i * 13 + 1) & 0x7F
                                            for i in range(length - 4))


def load_installer():
    spec = importlib.util.spec_from_file_location("dmr_installer", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Installer(unittest.TestCase):

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="dmr_installer_test_")
        self.addCleanup(shutil.rmtree, self.dir)
        self.bin = os.path.join(self.dir, "bin")
        os.mkdir(self.bin)
        self.radio_tool = self.tool("radio_tool", FAKE_RADIO_TOOL)
        self.tool("dfu-util", FAKE_DFU_UTIL)
        self.log = os.path.join(self.dir, "tools.log")
        self.out = os.path.join(self.dir, "out.bin")

    def tool(self, name, body):
        path = os.path.join(self.bin, name)
        with open(path, "w") as f:
            f.write("#!" + sys.executable + "\n" + body)
        os.chmod(path, 0o755)
        return path

    def file(self, name, data):
        path = os.path.join(self.dir, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def output(self):
        with open(self.out, "rb") as f:
            return f.read()

    def tool_calls(self):
        if not os.path.exists(self.log):
            return []
        with open(self.log) as f:
            return f.read().splitlines()

    # Only the fakes are on the path, so no real flasher can ever run.
    def install(self, fw, donor_data=None, *extra):
        args = [sys.executable, SCRIPT,
                "--firmware", self.file("fw.bin", fw),
                "--output", self.out, "--radio-tool", self.radio_tool]
        if donor_data is not None:
            args += ["--donor", self.file("donor.bin", donor_data)]
        env = dict(os.environ, PATH=self.bin, FAKE_TOOL_LOG=self.log)
        return subprocess.run(args + list(extra), env=env,
                              capture_output=True, text=True)

    def assertRefused(self, result, message):
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(message, result.stderr)
        self.assertFalse(os.path.exists(self.out))

    # The wrapper, the firmware, erased flash up to the codec, then the codec.
    def assertMerged(self, image, fw):
        self.assertEqual(image[:len(WRAP_HEADER)], WRAP_HEADER)
        image = image[len(WRAP_HEADER):]
        offset = CODEC_ADDR - UV3X0_BASE
        self.assertEqual(len(image), offset + CODEC_LEN)
        self.assertEqual(image[:len(fw)], fw)
        self.assertEqual(image[len(fw):offset], b"\xFF" * (offset - len(fw)))
        self.assertTrue(image[offset:] == CODEC, "codec not at 0x%X" % offset)

    def test_firmware_and_installer_agree_on_the_codec_address(self):
        with open(os.path.join(ROOT, "platform/mcu/STM32F4xx/"
                                     "linker_script_MDx.ld")) as f:
            text = f.read()
        start = int(re.search(r"_ambe_blob_start\s*=\s*(0x[0-9A-Fa-f]+)",
                              text).group(1), 16)
        end = int(re.search(r"_ambe_blob_end\s*=\s*(0x[0-9A-Fa-f]+)",
                            text).group(1), 16)
        self.assertEqual(start, CODEC_ADDR)
        self.assertEqual(end - start, CODEC_LEN)

        # The codec driver has one address table per radio
        with open(os.path.join(ROOT,
                               "platform/drivers/baseband/dmr_codec.c")) as f:
            starts = re.findall(r"#define\s+AMBE_REGION_START\s+(0x[0-9A-Fa-f]+)",
                                f.read())
        self.assertIn(CODEC_ADDR, [int(a, 16) for a in starts])

        installer = load_installer()
        self.assertEqual(installer.CODEC_ADDR, CODEC_ADDR)
        self.assertEqual(installer.CODEC_LEN, CODEC_LEN)

    def test_image_gets_the_codec_and_the_tyt_wrapper(self):
        fw = firmware()
        result = self.install(fw, donor(), "--any-donor")
        self.assertEqual(result.returncode, 0, result.stderr)

        self.assertMerged(self.output(), fw)
        self.assertIn("flash with: %s -d 0 -f -i %s"
                      % (self.radio_tool, self.out), result.stdout)

    def test_without_a_donor_the_image_is_left_voiceless(self):
        fw = firmware()
        result = self.install(fw)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("voice-less", result.stdout)
        self.assertEqual(self.output(), WRAP_HEADER + fw)
        self.assertFalse(any("--unwrap" in c for c in self.tool_calls()))

    def test_only_the_reference_donor_is_taken_without_asking(self):
        fw = firmware()
        self.assertRefused(self.install(fw, donor()),
                           "does not match the reference")

        result = self.install(fw, donor(), "--any-donor")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("warning: donor is not the reference", result.stdout)

        # The reference donor itself goes through without the warning
        installer = load_installer()
        installer.DONOR_SHA256 = hashlib.sha256(donor()).hexdigest()
        said = io.StringIO()
        with mock.patch.dict(os.environ, FAKE_TOOL_LOG=self.log), \
                contextlib.redirect_stdout(said):
            codec = installer.extract_codec(self.radio_tool,
                                            self.file("ref.bin", donor()),
                                            False)
        self.assertTrue(codec == CODEC)
        self.assertNotIn("warning", said.getvalue())

    def test_donors_without_the_codec_where_expected_are_refused(self):
        cases = [
            (donor(magic=b"NotATytFirmware"), "not a TYT firmware file"),
            (donor(codec=bytes(4) + CODEC[4:]), "codec not found"),
            (DONOR_MAGIC.ljust(DONOR_HEADER, b"\0")
             + b"\x5A" * (DONOR_CODEC_OFFSET + 64), "too short"),
        ]
        for data, message in cases:
            with self.subTest(message=message):
                self.assertRefused(self.install(firmware(), data,
                                                "--any-donor"),
                                   message)

    def test_images_that_are_not_an_mduv3x0_image_are_refused(self):
        cases = [
            (donor(), "looks like a wrapped/donor image"),
            (firmware(CS7000P_SP), "valid STM32 stack pointer"),
            (UV3X0_SP.to_bytes(4, "little"), "too small"),
        ]
        for fw, message in cases:
            with self.subTest(message=message):
                self.assertRefused(self.install(fw, donor(), "--any-donor"),
                                   message)

    def test_an_image_reaching_into_the_codec_region_is_refused(self):
        offset = CODEC_ADDR - UV3X0_BASE
        fw = firmware(UV3X0_SP, offset + 16)
        self.assertRefused(self.install(fw, donor(), "--any-donor"),
                           "overlaps the codec region")

        # Erased padding running into the region is fine
        fw = firmware() + b"\xFF" * (offset + 16 - 4096)
        result = self.install(fw, donor(), "--any-donor")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertMerged(self.output(), firmware())

    def test_flash_uses_radio_tool(self):
        result = self.install(firmware(), donor(), "--any-donor", "--flash")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.tool_calls()[-1],
                         "radio_tool -d 0 -f -i " + self.out)
        self.assertFalse(any(c.startswith("dfu-util")
                             for c in self.tool_calls()))


if __name__ == "__main__":
    # Meson hands every test the options it gives the Catch2 ones.
    unittest.main(argv=sys.argv[:1])
