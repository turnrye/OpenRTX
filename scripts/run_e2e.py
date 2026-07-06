#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

"""
Golden-image (snapshot) test runner for the ortx UI toolkit.

Takes a test script (tests/e2e/*.txt) of emulator shell commands (key,
sleep, screenshot, nop, quit) and runs it against the linux emulator built
with -Dui=ortx. Every "screenshot <name>.bmp" line becomes an assertion:
the captured frame is compared pixel-for-pixel against a golden reference at
tests/e2e/golden/<base_test>/<variant>/<name>.bmp.

Determinism is enforced by the harness, not the test:
  * faketime freezes the wall clock (stable top-bar clock),
  * TZ=UTC pins the timezone,
  * XDG_STATE_HOME points at a throwaway dir so persisted settings never
    leak between runs (each test starts from firmware defaults),
  * SDL runs offscreen with a dummy audio device.

The script is fed to the emulator all at once; the "sleep <ms>" commands in
the DSL pace it (the emulator's CLI thread blocks in usleep while the UI and
SDL threads render and write the screenshot), so an async screenshot is
reliably flushed before the following command runs.

Modelled on OpenRTX PR #445 (github.com/OpenRTX/OpenRTX/pull/445); adapted
for this branch's single ortx linux binary and its sleep-paced DSL.

Run every test:
    python scripts/run_e2e.py
    python scripts/run_e2e.py --build-dir build_ortx -j 4

Single test:
    python scripts/run_e2e.py tests/e2e/vfo.txt

Common flags:
    --update-golden    overwrite golden images with current output
    --tolerance N      max differing pixels before failure (default: 0)
"""

import argparse
import io
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from PIL import Image, ImageChops

try:
    from tqdm import tqdm
except ImportError:  # tqdm is only needed for the parallel progress bar
    tqdm = None

PROJECT_ROOT = Path(__file__).resolve().parent.parent
E2E_DIR = PROJECT_ROOT / "tests" / "e2e"

# Variant name -> binary filename (relative to build dir). The ortx UI is a
# build-time flag (-Dui=ortx), so there is one linux binary today; the map is
# kept so a second variant (e.g. a 1bpp mono target) can be added later.
VARIANTS = {
    "ortx": "openrtx_linux",
}


def load_normalized(path):
    """Load an image as RGB so byte comparisons are consistent regardless of
    the source mode (RGB vs RGBA, palette)."""
    return Image.open(path).convert("RGB")


def compare_images(actual_img, golden_img):
    """Return the count of differing pixels; raise ValueError on size
    mismatch."""
    if actual_img.size != golden_img.size:
        raise ValueError(
            f"Size mismatch: actual {actual_img.size}"
            f" vs golden {golden_img.size}"
        )

    diff = ImageChops.difference(actual_img, golden_img)
    if diff.getbbox() is None:
        return 0

    mask = diff.convert("L").point(lambda v: 255 if v else 0, mode="L")
    return sum(1 for px in mask.getdata() if px)


def generate_diff_image(actual_img, golden_img, diff_path):
    """Write a diff image highlighting differing pixels in red."""
    diff = ImageChops.difference(actual_img, golden_img)
    mask = diff.convert("L").point(lambda v: 255 if v else 0, mode="L")
    red = Image.new("RGB", actual_img.size, (255, 0, 0))
    out = Image.composite(red, Image.new("RGB", actual_img.size), mask)
    out.save(diff_path)


def resolve_base_name(script_name, variant):
    """Strip a trailing _<variant> suffix from the script name."""
    suffix = f"_{variant}"
    if script_name.endswith(suffix):
        return script_name[: -len(suffix)]
    return script_name


def is_safe_screenshot_name(name):
    """Reject anything that is not a plain basename in the tmpdir."""
    if not name or name != Path(name).name:
        return False
    if name.startswith(".") or name in ("", ".", ".."):
        return False
    return True


def run_test(script_path, binary, variant, tolerance, update_golden,
             build_dir, log=print, wrapper=None):
    """Run a single e2e test. Return True on success.

    All progress/diagnostic output goes through ``log`` so parallel runners
    can buffer per-test output. ``wrapper`` is an optional list of argv tokens
    inserted between faketime and the binary (e.g. ["valgrind"])."""
    test_name = script_path.stem
    base_name = resolve_base_name(test_name, variant)
    golden_dir = E2E_DIR / "golden" / base_name / variant
    fail_dir = build_dir / "e2e_failures" / base_name / variant

    def _preserve_partial(tmpdir, screenshots):
        produced = [
            (n, tmpdir / n) for n in screenshots if (tmpdir / n).is_file()
        ]
        if not produced:
            return
        fail_dir.mkdir(parents=True, exist_ok=True)
        for name, path in produced:
            shutil.copy2(path, fail_dir / f"actual_{name}")
        log(f"  Partial screenshots saved to: {fail_dir}")

    with tempfile.TemporaryDirectory() as tmpdir_str:
        tmpdir = Path(tmpdir_str)
        (tmpdir / "state").mkdir()

        # Parse the script, rewriting screenshot paths into the tmpdir.
        screenshots = []
        rewritten_lines = []
        with open(script_path) as f:
            for lineno, line in enumerate(f, 1):
                line = line.rstrip("\n")
                m = re.match(r"^\s*screenshot\s+(.+)$", line)
                if m:
                    name = m.group(1).strip()
                    if not is_safe_screenshot_name(name):
                        log(
                            f"FAIL: {script_path.name}:{lineno}: unsafe"
                            f" screenshot name {name!r} (must be a plain"
                            f" basename)"
                        )
                        return False
                    screenshots.append(name)
                    rewritten_lines.append(f"screenshot {tmpdir / name}")
                else:
                    rewritten_lines.append(line)
        rewritten_script = "\n".join(rewritten_lines) + "\n"

        log(f"Running E2E test: {base_name} ({variant})")

        env = {
            **os.environ,
            "TZ": "UTC",
            "XDG_STATE_HOME": str(tmpdir / "state"),
            "SDL_VIDEODRIVER": "offscreen",
            "SDL_AUDIODRIVER": "dummy",
        }

        cmd = ["faketime", "-f", "2000-01-01 00:00:00"]
        if wrapper:
            cmd += list(wrapper)
        cmd.append(str(binary))

        try:
            result = subprocess.run(
                cmd, input=rewritten_script, capture_output=True, text=True,
                timeout=30, cwd=str(tmpdir), env=env,
            )
        except subprocess.TimeoutExpired as e:
            log("FAIL: emulator timed out after 30s")
            if e.stderr:
                stderr = e.stderr
                if isinstance(stderr, bytes):
                    stderr = stderr.decode(errors="replace")
                log(stderr.rstrip())
            _preserve_partial(tmpdir, screenshots)
            return False

        # A nonzero exit is tolerated (with a warning) rather than an instant
        # failure: this branch predates the emulator's shutdown-race fix
        # (OpenRTX PR #445), which can abort during terminate *after* the
        # screenshots are written. The screenshots are the real assertions, so
        # they are still compared below; a genuine UI regression shows up as an
        # image diff and a hang is caught by the 30s timeout. Once the shutdown
        # fix lands on this branch this tolerance becomes a no-op.
        if result.returncode != 0:
            log(
                f"  WARNING: emulator exited with code {result.returncode}"
                f" (known pre-#445 shutdown race); comparing screenshots anyway"
            )

        failures = 0
        for name in screenshots:
            actual = tmpdir / name
            golden = golden_dir / name

            if not actual.is_file():
                log(f"  FAIL: screenshot not produced: {name}")
                if result.stderr:
                    log(result.stderr.rstrip())
                failures += 1
                continue

            if update_golden:
                golden_dir.mkdir(parents=True, exist_ok=True)
                shutil.copy2(actual, golden)
                log(f"  updated golden: {name}")
                continue

            if not golden.is_file():
                log(f"  FAIL: golden image missing: {golden}")
                log("    (run with --update-golden to create)")
                failures += 1
                continue

            try:
                actual_img = load_normalized(actual)
                golden_img = load_normalized(golden)
                diff_pixels = compare_images(actual_img, golden_img)
            except Exception as e:
                log(f"  FAIL: {name} -- comparison error: {e}")
                failures += 1
                continue

            if diff_pixels <= tolerance:
                log(f"  PASS: {name} ({diff_pixels} pixels differ)")
            else:
                log(
                    f"  FAIL: {name} -- {diff_pixels} pixels differ"
                    f" (tolerance: {tolerance})"
                )
                fail_dir.mkdir(parents=True, exist_ok=True)
                shutil.copy2(actual, fail_dir / f"actual_{name}")
                shutil.copy2(golden, fail_dir / f"expected_{name}")
                try:
                    generate_diff_image(
                        actual_img, golden_img, fail_dir / f"diff_{name}"
                    )
                except Exception as e:
                    log(f"    (diff image generation failed: {e})")
                log(f"    actual:   {fail_dir / f'actual_{name}'}")
                log(f"    expected: {fail_dir / f'expected_{name}'}")
                log(f"    diff:     {fail_dir / f'diff_{name}'}")
                failures += 1

        if failures > 0:
            return False

        log("  All assertions passed.")
        return True


def discover_tests(build_dir):
    """Yield (script_path, binary, variant) for every test script.

    A script ending in _<variant> is variant-specific; otherwise it runs
    against every known variant."""
    for script in sorted(E2E_DIR.glob("*.txt")):
        name = script.stem
        matched = next(
            (v for v in VARIANTS if name.endswith(f"_{v}")), None
        )
        if matched:
            yield script, build_dir / VARIANTS[matched], matched
        else:
            for v, bin_name in VARIANTS.items():
                yield script, build_dir / bin_name, v


def check_faketime():
    if not shutil.which("faketime"):
        print("FAIL: 'faketime' not found on PATH", file=sys.stderr)
        sys.exit(1)


def check_binary(binary):
    if not os.access(binary, os.X_OK):
        print(
            f"FAIL: binary not found or not executable: {binary}\n"
            f"  Build it with: meson setup build_ortx -Dui=ortx &&"
            f" meson compile -C build_ortx openrtx_linux",
            file=sys.stderr,
        )
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="ortx UI snapshot test runner")
    parser.add_argument(
        "test_script", nargs="?", type=Path,
        help="Path to a test script (.txt); omit to run all discovered tests",
    )
    parser.add_argument("--binary", type=Path, help="Path to openrtx binary")
    parser.add_argument(
        "--variant", default="ortx", help="UI variant (default: ortx)"
    )
    parser.add_argument(
        "--build-dir", type=Path, default=PROJECT_ROOT / "build_ortx",
        help="Build dir with the emulator binary (default: build_ortx)",
    )
    parser.add_argument(
        "-j", "--jobs", type=int, default=os.cpu_count() or 1,
        help="Parallel workers when running all tests (default: #CPUs)",
    )
    parser.add_argument(
        "--tolerance", type=int, default=0,
        help="Max differing pixels before a screenshot fails (default: 0)",
    )
    parser.add_argument(
        "--update-golden", action="store_true",
        help="Overwrite golden images with current output",
    )
    parser.add_argument(
        "--wrapper", default="",
        help="Command wrapping the emulator (e.g. 'valgrind'), inserted"
             " between faketime and the binary",
    )
    args = parser.parse_args()

    wrapper = shlex.split(args.wrapper) if args.wrapper else None

    if not args.test_script:
        return run_all(args, args.tolerance, args.update_golden, wrapper)

    script_path = args.test_script.resolve()
    build_dir = args.build_dir.resolve()
    binary = (
        args.binary.resolve() if args.binary
        else build_dir / VARIANTS.get(args.variant, "openrtx_linux")
    )

    check_faketime()
    check_binary(binary)

    if not script_path.is_file():
        print(f"FAIL: test script not found: {script_path}", file=sys.stderr)
        sys.exit(1)

    ok = run_test(
        script_path, binary, args.variant, args.tolerance, args.update_golden,
        build_dir, wrapper=wrapper,
    )
    sys.exit(0 if ok else 1)


def run_all(args, tolerance, update_golden, wrapper=None):
    """Discover and run every test in parallel via a thread pool."""
    build_dir = args.build_dir.resolve()
    check_faketime()

    tests = list(discover_tests(build_dir))
    if not tests:
        print("No e2e test scripts found.", file=sys.stderr)
        sys.exit(1)

    runnable, skipped = [], []
    for script, binary, variant in tests:
        base = resolve_base_name(script.stem, variant)
        if os.access(binary, os.X_OK):
            runnable.append((script, binary, variant, base))
        else:
            skipped.append((f"{base} ({variant})", str(binary)))

    for name, path in skipped:
        print(f"SKIP: {name} (binary not found: {path})")
    if not runnable:
        print("\nNo runnable tests.")
        sys.exit(1)

    jobs = max(1, min(args.jobs, len(runnable)))
    passed = failed = 0
    failed_names = []

    def _worker(item):
        script, binary, variant, base = item
        buf = io.StringIO()
        ok = run_test(
            script, binary, variant, tolerance, update_golden, build_dir,
            log=lambda msg="": print(msg, file=buf), wrapper=wrapper,
        )
        return base, variant, ok, buf.getvalue()

    with ThreadPoolExecutor(max_workers=jobs) as ex:
        futures = [ex.submit(_worker, item) for item in runnable]
        bar = tqdm(
            total=len(runnable), desc=f"e2e (j={jobs})", unit="test",
            dynamic_ncols=True,
        ) if tqdm else None

        def _write(msg):
            if bar:
                tqdm.write(msg)
            else:
                print(msg)

        for fut in as_completed(futures):
            base, variant, ok, output = fut.result()
            _write(f"[{'PASS' if ok else 'FAIL'}] {base} ({variant})")
            if ok:
                passed += 1
            else:
                if output.strip():
                    _write(output.rstrip())
                failed += 1
                failed_names.append(f"{base} ({variant})")
            if bar:
                bar.update(1)
        if bar:
            bar.close()

    print()
    print(
        f"{passed} passed, {failed} failed, {len(skipped)} skipped,"
        f" {len(tests)} total"
    )
    if failed_names:
        print("\nFailed tests:")
        for name in failed_names:
            print(f"  {name}")

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
