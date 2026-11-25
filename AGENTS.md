# Notes for agents working on EPICYCLE

## Product

**EPICYCLE** is an experimental freeform looping playground for the Akai MPC Sample.
Per-pad scrollable loop brace (window), knobs + LFOs, time-division or freeform regions,
crossfade loops, phrase recorder (later). This repo is MIT static C (thin C++ allowed later).

## Hard rules

- **No firmware flash.** Ship SD-card `autorun` only. Card out / rename `autorun` → stock.
- **No AGPL vendoring.** Do not submodule or copy sample-base / Melodee source. Patterns from
  SampleBoot / MPC-600-S / sample-base *docs* are fine; rewrite as MIT code here.
- **Primary target:** firmware **1.4** with Developer Mode + top-level `autorun.sh`.
  Also keep 1.3 `autorun.check` packaging via `tools/make_autorun.py` when practical.
- Do not claim hardware verification until Ash boots a build and pastes `Epicycle/logs/`.

## Layout

| Path | Role |
| --- | --- |
| `src/` | App: DRM/fb, ALSA I2S (ioctl), surface MIDI, loop engine, WAV, PNG, UI |
| `assets/boot.png` | Ash boot splash (320×226 RGBA); packaged as `autorun/epicycle/assets/boot.png` |
| `third_party/stb/` | `stb_image.h` (public domain / MIT) — PNG decode only; no AGPL |
| `tools/make_autorun.py` | Pack `autorun/` (+ top `autorun.sh`), compute 1.3 `autorun.check`, zip |
| `tools/check_preview.py` | Host preview WAV must be non-silent |
| `install/` | Templates and recovery helpers copied into the package |
| `docs/` | Design notes that belong in-repo (keep Project store docs as source of vision) |

## Build

```sh
make                # aarch64 static binary → build/epicycle
make host           # native smoke for CI / desktop
make package        # dist/EPICYCLE-*.zip SD payload
make test-host      # --preview WAV + peak check + --demo
```

Cross compiler: `aarch64-linux-gnu-gcc` (or set `CROSS=`). Links with `-pthread -lm`.
No alsa-lib — kernel ioctls only so static builds stay simple.

## Loop / mod model (v0)

- Brace = **window** (`window_start` + `window_length`); knobs/LFO slide start across the sample.
- Time-locked: length = N beats × tempo × sample rate (no warp). Freeform: length from K2.
- Per pad: up to **2 LFOs** and **4 mod routes** (`EpicycleModDest`: brace start/len, pitch, level, filter stub).
- Aggressive **brace scrub** limited to first **8** pads that request it (`EPICYCLE_SCRUB_BUDGET`); others may still use pitch/level LFO.
- Full modulation-matrix UI can come later — keep the structs/routes wired.

## Boot / recovery patterns (SampleBoot OS-AUTHORS)

- Resolve app dir from `$0`, not a hardcoded `/autorun`.
- Resolve **card root from `/proc/mounts`** (longest mount prefix).
- OS under `autorun/epicycle/`; user data under `<card>/Epicycle/`.
- On exit: restore stock with `systemctl start ac50` (launcher trap / start script).

## Stub vs real (loop engine)

| Real | Still stub / later |
| --- | --- |
| Crossfade loop voices, WAV load, division/free | Phrase recorder |
| Brace scroll (knobs/wheel/LFO), pitch ratio | Filter DSP, full ADSR |
| Mod matrix data model + basic LFO | Full matrix UI |
| Host `--preview` non-silent check | Hardware sign-off |
