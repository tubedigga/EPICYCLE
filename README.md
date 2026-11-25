# EPICYCLE

**Loop engine** for EPICYCLE — an experimental freeform looping playground for the [Akai MPC Sample](https://www.akaipro.com/).

Per pad: sample **start / end / loop brace** (a **window** you can scroll across the sample), three knobs + LFOs, musical **time-division** or **freeform** regions, crossfade loops, pitch (octave / semi / fine — no warp). Clean playback by default; the window slides when you turn knobs or when an LFO scans it.

This repository is **MIT**, static **aarch64 C**. It is **not** sample-base / Melodee (AGPL) and does **not** flash firmware. Custom OSes run from microSD via the stock autorun path.

> **Status:** `0.2.3-display-drm` — after `ac50` stop, `/dev/fb0` is gone on 1.4; display now prefers DRM `/dev/dri/card0` dumb buffers + `DIRTYFB`. Also: pad LED latch (running-status after SysEx), fader→silence. Host-tested. Needs Ash’s card retest + `Epicycle/logs/last-session.log`.

## Update the SD card (Ash — firmware 1.4)

Replace the **autorun payload only**; keep your `Epicycle/` user data.

1. Power off / remove the microSD.
2. On a computer, unzip the new `EPICYCLE-*.zip`.
3. On the card root, **delete or rename** the old `autorun/` folder (and old top-level `autorun.sh` if present).
4. Copy the new `autorun.sh` + `autorun/` onto the card root. Leave `Epicycle/` alone (projects / samples / logs stay).
5. Reinsert → power on → Developer Mode autorun prompt → **Yes**.
6. Expect: boot logo splash → loop UI on screen; pad N lights pad N; **Main Volume** + **fader** (bottom = silence); pads play. Paste `Epicycle/logs/last-session.log` (look for `display: drm … dirtyfb` or `display-diag:`).

Rollback: remove the card, or rename `autorun` → `autorun-off`.

## Requirements

- Akai MPC Sample (primary: **firmware 1.4.x** with **Developer Mode**)
- microSD card (FAT32 or exFAT)
- Optional: [SampleBoot](https://github.com/pxcm/SampleBoot) for multi-boot
- Build host: `gcc`, `python3`, `aarch64-linux-gnu-gcc` (or set `CROSS=`)

## Quick build

```sh
make              # build/epicycle (aarch64, static)
make host         # build/epicycle-host (native smoke)
make package      # dist/EPICYCLE-*.zip + dist/sd-root/
make test-host    # demo bank + non-silent preview WAV check
```

## Install on firmware 1.4 (Ash’s unit)

1. On the MPC: enable **Developer Mode** (stock Settings → DEV ENHANCEMENTS). Do **not** use “Load Untrusted OS” for this project.
2. Unzip a release (or `dist/EPICYCLE-*.zip` from `make package`).
3. Copy to the **card root**:
   - `autorun.sh` (top-level 1.4 bridge)
   - `autorun/` (launcher + `epicycle/` payload)
   - optional: `Epicycle/` user-data folder (projects / samples / logs)
4. Put WAVs on the card as `Epicycle/samples/pad01.wav` … `pad16.wav` (or `1.wav` …). If none are found, EPICYCLE loads a built-in synth demo bank.
5. Insert the card and power on. Stock boots, then asks to run the autorun script — choose **Yes**.
6. The launcher stops `ac50.service`, runs EPICYCLE from `/run`, and on exit starts stock again.

### What you should try on the device

1. Boot splash (Ash logo, letterboxed on black) → loop screen — must not stay black.
2. **Hit pads** — hear loops; **pad LED must match** the pad you hit. Demo: pads **1–2** synced 4-beat; **3–4** = 3 vs 5.
3. **Main Volume** (CC19) and **control fader** (CC20) set loudness; fader at bottom = silence.
4. **Select a pad**, then **K1** scroll brace, **K2** length/division, **K3** crossfade.
5. **Left soft key** toggles free vs division; **middle soft key** toggles LFO brace scrub (~8 scrub budget).
6. Wheel nudge scrolls; **push the wheel** (or right soft key) → quit to stock.

Session log: `Epicycle/logs/last-session.log` on the card.

### Firmware 1.3 packaging

`make package` also writes `autorun/autorun.check` for 1.3.0.12 cards. On 1.3 you only need the `autorun/` folder at the card root.

### SampleBoot (multi-boot)

Copy the packaged `autorun/` folder into `autorun/mods/EPICYCLE/` on a SampleBoot card. EPICYCLE resolves the card root from `/proc/mounts` (SampleBoot [OS-AUTHORS](https://github.com/pxcm/SampleBoot/blob/main/docs/OS-AUTHORS.md) pattern).

## Rollback

| Situation | What to do |
| --- | --- |
| Hang | Hold **POWER** until off → **remove the card** → boot stock |
| Bad build | Rename card `autorun` → `autorun-off` (or delete it) |
| Multi-boot | SampleBoot encoder → stock or previous mod |
| Keep projects | Leave `Epicycle/` on the card when replacing `autorun/` |

**Never** write U-Boot, `boot` / `recovery` partitions, or “untrusted OS” images for v0.

## Repository layout

```text
src/
  main.c                 entry + pad/knob → engine loop
  include/epicycle.h     pads, voices, LFO / mod-matrix hooks
  platform/              card paths, session log, ALSA card lookup
  display/               320×240 DRM/fb present
  audio/                 I2S ALSA + realtime engine callback
  surface/               surface MIDI pads/knobs/soft keys/quit
  sample/                WAV load + synth demo tones
  engine/                crossfade loopers, division/free, pitch, LFO scrub
  ui/                    splash + loop screen (waveform brace)
tools/make_autorun.py    1.4 bridge + 1.3 check + zip
tools/check_preview.py   host preview WAV sanity
install/                 templates shipped into the package
```

## What’s in / still later

| Piece | This release | Still later |
| --- | --- | --- |
| Loop brace window | Scrollable start + length; knobs + wheel | Full ADSR one-shots |
| Division / free | 1–32 beats vs freeform vs tempo | Scene snapshots |
| Crossfade loops | Per-pad amount (K3) | Streaming long beds from SD |
| Pitch | Octave / semi / fine (API + UI readout) | Dedicated pitch knobs page |
| LFO / mod matrix | 2 LFOs × 4 routes per pad; brace scrub budgeted to 8 | Full matrix UI, filter DSP |
| Samples | WAV load from card; synth demo bank | Record from line/mic |
| Phrase recorder | — | Next milestone |
| Hardware sign-off | Needs Ash’s card boot + log | — |

## License

[MIT](LICENSE). Community docs from SampleBoot, MPC-600-S, and sample-base informed the packaging and hardware notes; **no AGPL source is vendored** into this tree.

## Product vision

Full identity and phased plan live in the Project docs (`mpc-sample-os-plan.md` / `project-context.md`). Short version: freeform region-loop playground, not a Melodee clone or classic MPC-3000 OS.
