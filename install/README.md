# Install templates

| File | Purpose |
| --- | --- |
| `top-autorun.sh` | Firmware **1.4** card-root `autorun.sh` bridge → `autorun/autorun.sh` |
| `sampleboot.txt` | SampleBoot menu label (copied into packaged `autorun/`) |
| `recovery-hint.txt` | Short rollback notes shipped beside the binary |

Packaging is done by `tools/make_autorun.py` (`make package`), which also writes
`autorun/autorun.json`, `autorun/autorun.sh`, and 1.3 `autorun/autorun.check`.
