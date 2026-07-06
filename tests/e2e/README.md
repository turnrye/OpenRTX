<!--
SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->

# ortx UI golden-image tests

Snapshot (golden-image) tests for the `ortx` retained-widget UI toolkit. Each
test drives the linux emulator through a short script, captures screenshots,
and compares them pixel-for-pixel against a checked-in golden reference. A
change that alters any pixel of a covered screen fails the test, so the visual
design is locked against accidental regressions.

Modelled on [OpenRTX PR #445](https://github.com/OpenRTX/OpenRTX/pull/445),
adapted for this branch's single ortx linux binary and its `sleep`-paced DSL
(this branch predates the emulator's `wait_stable` / `screenshot_sync`
primitives).

## Requirements

- The linux emulator built with the ortx UI:
  ```
  meson setup build_ortx -Dui=ortx
  meson compile -C build_ortx openrtx_linux
  ```
- `faketime` on `PATH` (`apt install faketime`) — freezes the clock so the top
  bar renders identically every run.
- Python deps: `pip install -r requirements.txt` (Pillow, tqdm).

## Running

```
# every test, in parallel
python scripts/run_e2e.py

# one test
python scripts/run_e2e.py tests/e2e/vfo.txt

# after an intentional UI change, refresh the goldens and eyeball the diff
python scripts/run_e2e.py --update-golden
git diff --stat tests/e2e/golden
```

On failure the actual image, the expected image, and a red diff mask are
written under `build_ortx/e2e_failures/<test>/<variant>/`.

`--tolerance N` allows up to N differing pixels (default 0). Prefer pinning the
nondeterministic input instead (see below).

## Determinism

The runner makes each run reproducible without any per-test setup:

- **`faketime -f "2000-01-01 00:00:00"`** — the top-bar clock is always `00:00`.
- **`TZ=UTC`** — timezone can't shift the clock.
- **`XDG_STATE_HOME=<tmpdir>`** — the linux nvmem backend persists settings
  there, so each test starts from firmware defaults; edits made in one test
  never leak into another (or into your real `~/.local/state`).
- **offscreen SDL + dummy audio**.

Some values still vary. Where the emulator exposes a setter, pin it: `vfo.txt`
issues `rssi -100` so the S-meter can't drift across a level boundary. Where it
does not, tolerate the jittery region with a `#tolerance` directive (below):
the Info screen's RSSI row is left at 0 or -100 dBm depending on emulator
timing (~40px of digits), so `info.txt` allows `#tolerance 80` while still
catching layout or other-row regressions (those differ by hundreds of pixels).

### Directives

Lines beginning with `#` are harness directives, parsed by the runner and
never sent to the emulator:

- `#require <feature>` — skip the test (rather than fail) unless the feature is
  present. The only feature today is `test_version`: the binary must be built
  with `-Dtest_version=e2e-test` so version-bearing screens render a fixed
  string. `about.txt` uses it, so About is skipped on a normal build and runs
  in CI / a `-Dtest_version=e2e-test` build:
  ```
  meson configure build_ortx -Dtest_version=e2e-test
  meson compile -C build_ortx openrtx_linux
  python scripts/run_e2e.py            # now About runs too
  ```
- `#tolerance <N>` — allow up to N differing pixels for this test (never
  tightens; the CLI `--tolerance` can loosen further). Prefer pinning the input
  over tolerating it; use this only for genuinely emulator-nondeterministic
  regions.

> The emulator can abort during shutdown on this branch (a race fixed upstream
> in PR #445), *after* the screenshots are written. The runner treats a nonzero
> exit as a warning and still compares the screenshots, so this does not make
> the suite flaky. A hang is still caught by the 30s timeout.

## Writing a test

Tests are `tests/e2e/*.txt` files of emulator shell commands, fed to the
emulator all at once. The `sleep <ms>` commands pace it: the emulator's CLI
thread blocks in `usleep` while the UI and SDL threads render and flush the
screenshot, so an (asynchronous) `screenshot` is on disk before the next
command runs.

Useful commands: `key <NAME> [NAME...]` (press keys in sequence, e.g.
`key DOWN DOWN ENTER`), `screenshot <name>.bmp` (assertion — compared to
`golden/<test>/<variant>/<name>.bmp`), `sleep <ms>`, `nop <comment>`,
`rssi`/`vbat`/`channel`/`ptt` (pin simulated inputs), `quit`.

Convention: start with `sleep 1000` to clear the splash, put `sleep 300`–`500`
after each `key` (so the UI drains its event queue and renders) and after each
`screenshot`, and end with `quit`. Then generate the golden with
`--update-golden` and **inspect it** before committing.

Golden dir: `golden/<base>/<variant>/`. The variant is `ortx`; a script named
`foo_<variant>.txt` is variant-specific, otherwise it runs for every variant.

## Coverage

| Test | Screens |
|------|---------|
| `vfo` | VFO home (freq hero, channel line, RX dot meter, top bar) |
| `main_menu` | Main menu, selection movement |
| `settings_menu` | Settings submenu, scrolled |
| `settings_display` | Display value rows + Brightness edit mode + commit |
| `settings_accessibility` | Accessibility checklist + checkbox toggle |
| `info` | Info key/value table (RSSI row tolerated, see above) |
| `fm` | Settings > FM: CTCSS tone + enable value rows, tone edit |
| `radio` | Settings > Radio: offset keypad entry, direction flip, step |
| `gps` | GPS Position "no fix" status state |
| `about` | About: brand, firmware version, credits (`#require test_version`) |

Not yet covered (need a deterministic hook first): the GPS live-fix layout
(needs a GPS-injection command or settings seed — the sim feeds a Hamburg fix
but `gps_enabled` defaults off under state isolation), and 1bpp mono rendering
(needs a mono linux variant).
