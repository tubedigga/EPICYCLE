# EPICYCLE

**Bring-up scaffold** for EPICYCLE — an experimental freeform looping playground for the [Akai MPC Sample](https://www.akaipro.com/).

Per pad: sample **start / end / loop brace**, three knobs + LFOs, musical **time-division** or **freeform** regions, crossfade loops, shared mix FX, and a **phrase recorder**. Clean playback by default; granular character only when modulation scans the sample.

This repository is **MIT**, static **aarch64 C**. It is **not** sample-base / Melodee (AGPL) and does **not** flash firmware. Custom OSes run from microSD via the stock autorun path.

> **Status:** scaffold only — **not hardware-tested**. Expect path resolution, stubs for display / ALSA / surface MIDI, autorun packaging, and a host smoke binary. Real panel takeover and audio still to come.

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
make test-host    # run host binary once
```

## Install on firmware 1.4 (Ash’s unit)

1. On the MPC: enable **Developer Mode** (stock Settings → DEV ENHANCEMENTS). Do **not** use “Load Untrusted OS” for this project.
2. Unzip a release (or `dist/EPICYCLE-*.zip` from `make package`).
3. Copy to the **card root**:
   - `autorun.sh` (top-level 1.4 bridge)
   - `autorun/` (launcher + `epicycle/` payload)
   - optional: `Epicycle/` user-data folder (projects / samples / logs)
4. Insert the card and power on. Stock boots, then asks to run the autorun script — choose **Yes**.
5. The launcher stops `ac50.service`, runs EPICYCLE from `/run`, and on exit starts stock again.

### Firmware 1.3 packaging

`make package` also writes `autorun/autorun.check` (`sha256` of `autorun.json` + `autorun.sh` + the public platform salt) for 1.3.0.12 cards. On 1.3 you only need the `autorun/` folder at the card root (no top-level bridge required).

### SampleBoot (multi-boot)

Copy the packaged `autorun/` folder into `autorun/mods/EPICYCLE/` on a SampleBoot card. EPICYCLE resolves the card root from `/proc/mounts` (SampleBoot [OS-AUTHORS](https://github.com/pxcm/SampleBoot/blob/main/docs/OS-AUTHORS.md) pattern), so it should not need `sampleboot.inplace`.

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
  main.c                 entry
  include/epicycle.h     shared types
  platform/              card paths + session log
  display/               320×240 fb stub
  audio/                 ALSA I2S stub
  surface/               surface MIDI stub
  engine/                loop-engine stubs (16 pads)
  ui/                    splash stub
tools/make_autorun.py    1.4 bridge + 1.3 check + zip
install/                 templates shipped into the package
AGENTS.md                notes for future agents
```

## Stub vs real

| Piece | Scaffold | Later |
| --- | --- | --- |
| Paths / logging | Real (`/proc/mounts`, `Epicycle/logs/`) | — |
| Display | Software buffer; optional write to `/dev/fb0` | DRM format/stride, handoff |
| Audio | Open stub (silence) | ALSA S32 @ 44.1, click-free codec |
| Surface | Open stub; host auto-quits | Pads/knobs/wheel, LEDs |
| Engine | 16 pad structs, silent render | Samples, LFO, crossfade, recorder |
| Autorun pack | 1.4 top `autorun.sh` + 1.3 `autorun.check` | Release CI |

## License

[MIT](LICENSE). Community docs from SampleBoot, MPC-600-S, and sample-base informed the packaging and hardware notes; **no AGPL source is vendored** into this tree.

## Product vision

Full identity and phased plan live in the Project docs (`mpc-sample-os-plan.md` / `project-context.md`). Short version: freeform region-loop playground, not a Melodee clone or classic MPC-3000 OS.
