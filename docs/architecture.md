# Architecture (scaffold)

EPICYCLE runs as a userspace app started from microSD autorun after stock
`ac50.service` comes up. It does not replace the kernel or flash firmware.

```text
Power on
  → Linux + stock userspace
  → ac50.service
  → 1.4: Developer Mode prompt → top-level autorun.sh → autorun/autorun.sh
     1.3: autorun.check → autorun/autorun.sh
  → launcher copies binary to /run, stops ac50, runs epicycle
  → display + audio + surface stubs → splash → quit
  → systemctl start ac50
```

## Modules

- **platform** — app dir from `$0` / `MPC_APP_DIR`; card root from `/proc/mounts`;
  user data under `<card>/Epicycle/`.
- **display** — 320×240 software framebuffer (BGRX), optional `/dev/fb0` write.
- **audio** — placeholder for AK4558 I2S ALSA (44.1 on firmware 1.4).
- **surface** — placeholder for Control Surface raw MIDI.
- **engine** — 16 pads with start/end/loop fields; silent render callback shape.
- **ui** — brand splash only.

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
