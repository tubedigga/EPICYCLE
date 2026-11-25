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
  → splash → loop UI (pads / waveform brace / knobs)
  → audio thread ← engine_render (16 voices, crossfade loops)
  → wheel push → clean close
  → systemctl start ac50
```

## Modules

- **platform** — app dir from `$0` / `MPC_APP_DIR`; card root from `/proc/mounts`;
  user data under `<card>/Epicycle/`; ALSA card id lookup via `/proc/asound/cards`.
- **display** — software 320×240 `0x00RRGGBB` buffer; present via mmap/write to
  `/dev/fb0` using `FBIOGET_*` stride and bpp (RGB565 or 32-bit).
- **audio** — I2S card PCM via kernel ioctls (no alsa-lib): S32_LE stereo 44.1 kHz,
  realtime thread calls `engine_render` when an engine is bound; `DROP` on close.
- **surface** — Control Surface raw MIDI: pads, K1–K3, soft keys (free/div, LFO),
  wheel scroll/quit, lights off, position-request SysEx.
- **sample** — PCM WAV → interleaved stereo float; synth demo tones for host/no-card.
- **engine** — 16 pads × scrollable brace window; division (1–32 beats) or freeform;
  pitch (oct/semi/fine); crossfade loopers; 16 voices; 2 LFOs + 4 mod routes per pad;
  brace-scrub budget (`EPICYCLE_SCRUB_BUDGET` = 8).
- **ui** — brand splash + loop screen (grid, waveform, brace, knob legend).

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
    epicycle/epicycle        # binary + hints
  Epicycle/
    projects/ samples/ kits/ logs/ settings.json
    samples/pad01.wav …      # optional; else synth demo bank
```
