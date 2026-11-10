"""Generates rampage's sounds: small 16-bit mono WAV files, no downloads.

  make_sounds.py [--out DIR]      writes DIR/hit.wav and DIR/death.wav
                                  (default: assets/sounds of the repository)
  make_sounds.py --check [--out DIR]
                                  regenerates them in memory and fails when a
                                  committed file differs (checks.sounds_fresh)

The synthesis uses integer arithmetic only (a phase accumulator and a
linear congruential generator), so the bytes are the same on every platform
and Python version. Python 3, stdlib only.

  hit    60 ms: a square wave sweeping from 330 Hz down to 110 Hz under a
         linear decay (an effect applies damage, game_rampage EffectSystem)
  death  250 ms: noise under a decay, low-passed by a one-pole integer
         filter (an entity is destroyed, DeathRule; a cosmetic sound)
"""

import argparse
import io
import pathlib
import struct
import sys
import wave

RATE = 48000


def wav_bytes(samples):
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(b"".join(struct.pack("<h", s) for s in samples))
    return buffer.getvalue()


def hit():
    frames = RATE * 60 // 1000
    samples = []
    phase = 0
    for i in range(frames):
        # Frequency in millihertz, from 330 Hz down to 110 Hz.
        freq_mhz = 330_000 - (220_000 * i) // frames
        # Phase in 1/RATE of a millihertz cycle: one cycle is RATE * 1000.
        phase = (phase + freq_mhz) % (RATE * 1000)
        level = 20_000 * (frames - i) // frames
        samples.append(level if phase < RATE * 500 else -level)
    return samples


def death():
    frames = RATE * 250 // 1000
    samples = []
    seed = 0x5EED
    smooth = 0
    for i in range(frames):
        seed = (1103515245 * seed + 12345) % (1 << 31)
        noise = ((seed >> 15) & 0xFFFF) - 32768
        smooth += (noise - smooth) // 4
        level = (frames - i) * 1000 // frames
        samples.append(max(-32768, min(32767, smooth * level // 1000)))
    return samples


SOUNDS = {"hit.wav": hit, "death.wav": death}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(
        pathlib.Path(__file__).resolve().parents[2] / "assets" / "sounds"))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    out = pathlib.Path(args.out)
    stale = []
    for name, make in SOUNDS.items():
        data = wav_bytes(make())
        path = out / name
        if args.check:
            if not path.exists() or path.read_bytes() != data:
                stale.append(str(path))
            continue
        out.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        print(f"wrote {path} ({len(data)} bytes)")
    for path in stale:
        print(f"FAIL: {path} differs from make_sounds.py's output; run "
              f"python tools/audio/make_sounds.py and commit it")
    if args.check and not stale:
        print(f"ok: {len(SOUNDS)} sounds match make_sounds.py")
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
