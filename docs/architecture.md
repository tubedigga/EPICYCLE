# Architecture (loop engine)

EPICYCLE runs as a userspace app started from microSD autorun after stock
`ac50.service` comes up. It does not replace the kernel or flash firmware.

```text
Power on
  → Linux + stock userspace
  → ac50.service
  → 1.4: Developer Mode prompt → top-level autorun.sh → autorun/autorun.sh
     1.3: autorun.check → autorun/autorun.sh
  → launcher copies binary to /run, stops ac50, runs epicycle
  → display (/dev/fb0) + I2S ALSA + Surface MIDI
  → splash → loop UI (large waveform + brace / knobs)
  → audio thread ← engine_render (16 voices, crossfade loops)
  → wheel push → clean close
  → systemctl start ac50
```

## Modules

- **platform** — app dir from `$0` / `MPC_APP_DIR`; card root from `/proc/mounts`;
  user data under `<card>/Epicycle/`; ALSA card id lookup via `/proc/asound/cards`.
- **display** — software 320×240 `0x00RRGGBB` buffer. On device after `ac50`
  stops, `/dev/fb0` is often missing; prefer DRM `/dev/dri/card0` dumb buffers
  (XRGB8888, double-buffered) + `SET_MASTER` / `SETCRTC` / `DIRTYFB`. Fallback:
  fbdev write/mmap with bpp/stride, RGB565 or BGRX, `FBIOBLANK`, pan. Verbose
  `display-diag:` lines go to `last-session.log`.
- **audio** — I2S card PCM via kernel ioctls (no alsa-lib): S32_LE stereo 44.1 kHz,
  realtime thread calls `engine_render` when an engine is bound; `DROP` on close.
  Main Volume (CC19, squared) × fader (CC20, linear) → `master_gain` (0 = silence).
- **surface** — Control Surface raw MIDI: pads (note map + LED SysEx I=pad−1),
  SAMPLE button (ch1 note 6) for select-hold, K1–K3, Main Volume CC19, fader CC20,
  soft keys (free/div, LFO), wheel scroll/quit, selected-pad amber LED, position
  SysEx `0x42`. Map from sample-base `controls.h` / HARDWARE.md (MIT rewrite only).
- **sample** — PCM WAV → interleaved stereo float; synth demo tones for host/no-card.
- **image** — PNG load via vendored `stb_image` (MIT/PD); boot splash only.
- **engine** — 16 pads × scrollable brace window; division (1–32 beats) or freeform;
  pitch (oct/semi/fine); crossfade loopers; 16 voices with note-off + steal;
  2 LFOs + 4 mod routes per pad; brace-scrub budget (`EPICYCLE_SCRUB_BUDGET` = 8).
- **ui** — Ash boot logo splash; loop screen = one large waveform + brace, soft-key
  labels, START/END/POS readouts (no pad grid / no brand chrome).

## Loop brace / modulation

```text
sample  |==============================|
window  |----[###########]-------------|   start + length
LFO     |----[####·······]-------------|   start slides (scrub)
```

Mod destinations (hooks for later UI): `BRACE_START`, `BRACE_LEN`, `PITCH`,
`LEVEL`, `FILTER` (stub). Soft-mid toggles LFO0 → brace start on the selected pad.

## Data on card

```text
<card>/
  autorun.sh                 # 1.4 bridge
  autorun/
    autorun.sh|json|check
    sampleboot.txt
    epicycle/
      epicycle               # binary
      assets/boot.png        # splash logo
      *.txt                  # recovery / samples hints
  Epicycle/
    projects/ samples/ kits/ logs/ settings.json
    samples/pad01.wav …      # optional; else synth demo bank
```
