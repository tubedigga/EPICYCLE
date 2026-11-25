#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail if a host preview WAV is missing, too short, or essentially silent."""
from __future__ import annotations

import sys
import wave
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: check_preview.py PATH.wav", file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    if not path.is_file():
        print(f"missing preview: {path}", file=sys.stderr)
        return 1
    with wave.open(str(path), "rb") as w:
        frames = w.getnframes()
        rate = w.getframerate()
        ch = w.getnchannels()
        sw = w.getsampwidth()
        raw = w.readframes(frames)
    if frames < rate // 4:
        print(f"preview too short: {frames} frames @ {rate}", file=sys.stderr)
        return 1
    if sw != 2:
        print(f"expected 16-bit PCM, got sampwidth={sw}", file=sys.stderr)
        return 1
    # Peak absolute sample
    peak = 0
    for i in range(0, len(raw), 2):
        s = int.from_bytes(raw[i : i + 2], "little", signed=True)
        a = abs(s)
        if a > peak:
            peak = a
    if peak < 500:
        print(f"preview essentially silent (peak={peak})", file=sys.stderr)
        return 1
    print(f"preview ok: {path} frames={frames} rate={rate} ch={ch} peak={peak}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
