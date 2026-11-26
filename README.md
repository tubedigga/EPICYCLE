# EPICYCLE

**Tombola** for EPICYCLE on the [Akai MPC Sample](https://www.akaipro.com/) — generative geometric drum: balls bounce, wall hits fire pads.

Primary screen: 2D projected tombola — **Fader** morph, **MAIN** vol, **K1** spin±dir (CW=CW), **MUTE+pad** balls (rest at bottom until spin). **SAMPLE + pad** selects; pad LED flashes on wall hit only. HitMod slots start empty. Freeze / reverb DSP are later.

This repository is **MIT**, static **aarch64 C**. It is **not** sample-base / Melodee (AGPL) and does **not** flash firmware. Custom OSes run from microSD via the stock autorun path.

> **Status:** `0.3.15-matrix-ui` — always-visible LFO matrix (DEST incl POS); SHIFT chrome clear of titles; finer FREE grains; SAMPLE+pad stays on edit page; Mix DEL/REV sends independent. Host-tested. Needs Ash boot + log.

## Update the SD card (Ash — firmware 1.4)

Replace the **autorun payload only**; keep your `Epicycle/` user data.

1. Power off / remove the microSD.
2. On a computer, unzip the new `EPICYCLE-*.zip`.
3. On the card root, **delete or rename** the old `autorun/` folder (and old top-level `autorun.sh` if present).
4. Copy the new `autorun.sh` + `autorun/` onto the card root. Leave `Epicycle/` alone (projects / samples / logs stay).
5. Reinsert → power on → Developer Mode autorun prompt → **Yes**.
6. Expect: splash **PRESS ANY PAD TO CONTINUE** → Tombola; **B1/B2** pages; **B3** quits; encoder push does **not** quit. Paste `Epicycle/logs/last-session.log`.

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
4. Put any WAVs in `Epicycle/samples/` (any filename). Pads boot **empty** — assign via SAMPLE browse (no pad01 auto-map / no demo kit).
5. Insert the card and power on. Stock boots, then asks to run the autorun script — choose **Yes**.
6. The launcher stops `ac50.service`, runs EPICYCLE from `/run`, and on exit starts stock again.

### What you should try on the device

1. Splash holds; centered **PRESS ANY PAD TO CONTINUE** under logo.
2. **Tombola** home: balls rest at bottom; spin lifts; wall hits fire samples. **Fader** = morph; **MAIN** = vol; **K1** = spin (CW=CW); **K2** unused.
3. **SAMPLE + pad** = select (LED solid); **MUTE + pad** = remove/add ball; LED flash = wall hit only.
4. Soft keys: **B1** prev, **B2** next, **B3 QUIT**. Pages: Tombola / Trim / Mix / Amp / Tune / Filt / FEnv / LFO / HitMod.
5. **SEQ** = jump to Tombola from any page. **PAD FX** = Hit Mod. **LOOP** off = one-shot bounce.
6. Prefer short grains in `Epicycle/samples/`; ~8 active balls = messy generative texture.

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
| Tombola | 2D morph + spin + wall-hit triggers | Full 3D mesh morph |
| Ball select / mute | SAMPLE+pad; MUTE+pad | Mute groups on collision |
| Hit Mod | Stub: impact → pitch/filt/level | Full matrix + ball–ball actions |
| Trim / env / filter | Edit pages reused | Freeze 2–5 s → loop export |
| FX | — | Reverb / delay sends (soon) |
| Samples | WAV + synth demo bank | Record from line/mic |
| Hardware sign-off | Needs Ash’s card boot + log | — |

## License

[MIT](LICENSE). Community docs from SampleBoot, MPC-600-S, and sample-base informed the packaging and hardware notes; **no AGPL source is vendored** into this tree.

## Product vision

Tombola vision + phased plan live in the Project docs (`tombola-vision.md` / `mpc-sample-os-plan.md`). Short version: generative geometric tombola on MPC Sample, not a Melodee clone.
