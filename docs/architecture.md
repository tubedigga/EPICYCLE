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
  Main Volume (CC19, squared, follows hardware incl. 0=silence, smoothed) × fader
  (CC20, linear, latched until user moves) → `master_gain` (gain only; no voice restart).
- **surface** — Untrusted Surface MIDI parser: pads (explicit note-on; status cleared
  after pad msgs), SAMPLE (6), SHIFT (49, **momentary**), LOOP (74), K1–K3, Main CC19
  (dump applies incl. 0), fader CC20, soft keys B1/B2 **on press**, B3 quit on release,
  wheel (press never quits), `0x42` dump + RX drain. SysEx cancels running status;
  AT ignored. MIT rewrite of SampleBoot / sample-base / mpc-sample-lab notes.
- **sample** — PCM WAV → interleaved stereo float; synth demo tones for host/no-card.
- **image** — PNG load via vendored `stb_image` (MIT/PD); boot splash only.
- **engine** — 16 pads × brace window (Start/End lock width + POS slides; LFO scrub
  unchanged); equal-slice division/freeform; pitch; crossfade + ping-pong; LOOP
  latch-toggle / one-shot; Chamberlin SVF filter (default OFF); 2 LFOs + 4 mod
  routes; scrub budget = 8.
- **ui** — splash `PRESS ANY PAD TO CONTINUE`; Trim waveform + DIV grid + FREE/DIV
  badge; Shift overlay momentary; Mix/Filter/LFO pages.

## Loop brace / modulation

```text
sample  |==============================|
window  |----[###########]-------------|   start + length
LFO     |----[####·······]-------------|   start slides (scrub)
DIV N   |[#][#][#][#]…                 |   equal slices; brace = one slice
```

Division mode uses an equal-slice grid (length = 1/N), not tempo-mapped beat
frames. N is sticky (musical 1/2/4/8/16/32 only) with K2 hysteresis so MIDI noise
cannot flicker the count. FREE Trim allows near-zero width (1-frame min).
Latch mode: pad press toggles loop on/off; one-shot plays through on press.

Mod destinations: `BRACE_START`, `BRACE_LEN`, `PITCH`, `LEVEL`, `FILTER` (stub).
LFO page edits shape / rate / sync|free / depth (enables brace scrub). LFO never
rewrites DIV N. Pages cycle only via soft keys B1/B2 (clean release) — knobs /
pads / encoder never change `mode_page`. Filter Chamberlin damping maps knob
0=min reso … 127=max.

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
