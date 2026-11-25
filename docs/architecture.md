# Architecture (panel bring-up)

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
  → splash → home UI (pads / knobs)
  → wheel push → clean close
  → systemctl start ac50
```

## Modules

- **platform** — app dir from `$0` / `MPC_APP_DIR`; card root from `/proc/mounts`;
  user data under `<card>/Epicycle/`; ALSA card id lookup via `/proc/asound/cards`.
- **display** — software 320×240 `0x00RRGGBB` buffer; present via mmap/write to
  `/dev/fb0` using `FBIOGET_*` stride and bpp (RGB565 or 32-bit).
- **audio** — I2S card PCM via kernel ioctls (no alsa-lib): S32_LE stereo 44.1 kHz,
  silent keep-alive thread, `DROP` on close.
- **surface** — Control Surface raw MIDI: published pad/knob/wheel map, lights off,
  position-request SysEx, quit on wheel push / right soft key.
- **engine** — 16 pads with start/end/loop fields; silent render callback shape.
- **ui** — brand splash + home (status, pad grid, knob bars, quit hint).

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
```
