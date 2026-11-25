# Notes for agents working on EPICYCLE

## Product

**EPICYCLE** is an experimental freeform looping playground for the Akai MPC Sample.
Per-pad start / end / loop brace, knobs + LFOs, time-division or freeform regions,
crossfade loops, phrase recorder. This repo is MIT static C (thin C++ allowed later).

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
| `src/` | App: display, ALSA audio stubs, surface MIDI stubs, loop engine stubs, UI |
| `tools/make_autorun.py` | Pack `autorun/` (+ top `autorun.sh`), compute 1.3 `autorun.check`, zip |
| `install/` | Templates and recovery helpers copied into the package |
| `docs/` | Design notes that belong in-repo (keep Project store docs as source of vision) |

## Build

```sh
make                # aarch64 static-ish binary → build/epicycle
make host           # native stub for CI / desktop smoke
make package        # dist/EPICYCLE-*.zip SD payload
```

Cross compiler: `aarch64-linux-gnu-gcc` (or set `CROSS=`).

## Boot / recovery patterns (SampleBoot OS-AUTHORS)

- Resolve app dir from `$0`, not a hardcoded `/autorun`.
- Resolve **card root from `/proc/mounts`** (longest mount prefix).
- OS under `autorun/epicycle/`; user data under `<card>/Epicycle/`.
- On exit: restore stock with `systemctl start ac50` (trap EXIT).
- Optional SampleBoot assets: `sampleboot.txt`, `sampleboot.png` (320×240).

## Stub vs real

Bring-up scaffold: display/ALSA/MIDI open paths are stubs that log and no-op or
fail soft off-device. Real DRM/ALSA/MIDI wiring is Phase 1 hardware work.
