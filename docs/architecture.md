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
  → display (DRM card0 / fb0) + I2S ALSA + Surface MIDI
  → splash holds until encoder / soft key / pad
  → Sample-Mode-style pages (B1/B2 cycle; B3 quit)
  → audio thread ← engine_render (16 voices, crossfade loops)
  → soft-right quit → clean close (encoder push never quits)
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
  Main Volume (CC19, squared) × fader (CC20, linear, **latched** until user moves)
  → `master_gain`.
- **surface** — Control Surface raw MIDI: pads (note map + LED SysEx I=pad−1),
  SAMPLE (6) select-hold, SHIFT (49), LOOP (74), K1–K3, Main CC19, fader CC20,
  soft keys B1/B2/B3, wheel (press never quits), selected-pad amber LED, position
  SysEx `0x42`. Poly AT must not steal note running-status. Map from sample-base
  HARDWARE.md / controls.h note numbers (MIT rewrite only).
- **sample** — PCM WAV → interleaved stereo float; synth demo tones for host/no-card.
- **image** — PNG load via vendored `stb_image` (MIT/PD); boot splash only.
- **engine** — 16 pads × brace window (Start/End width + Position slide); division
  or freeform; pitch; forward crossfade loops + ping-pong; LOOP latch/one-shot;
  2 LFOs + 4 mod routes; LFO scrub targets Position; scrub budget = 8.
- **ui** — splash centered `PRESS ANY PAD TO CONTINUE`; Trim waveform + Shift
  overlay (xfade / FORWARD|PINGPONG); Mix pad vol/pan; filter type names;
  pages Trim/Mix/Amp/Tune/Filter/FiltEnv/LFO.

## Loop brace / modulation

```text
sample  |==============================|
window  |----[###########]-------------|   start + length
LFO     |----[####·······]-------------|   start slides (scrub)
```

Mod destinations: `BRACE_START`, `BRACE_LEN`, `PITCH`, `LEVEL`, `FILTER` (stub).
LFO page edits shape / rate / sync|free / depth (enables brace scrub).

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
