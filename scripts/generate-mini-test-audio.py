#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate original, deterministic mini-project audio with known onset times.

Python standard library only. Existing differing files are preserved unless
--overwrite is explicitly supplied. --check verifies without writing anything.
"""
import argparse
import hashlib
import io
import json
import math
from pathlib import Path
import struct
import wave

RATE = 44100
DURATION = 12.0
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "testdata" / "m7-mini-project"


def wav_bytes(samples):
    stream = io.BytesIO()
    pcm = bytearray()
    for sample in samples:
        value = round(sample * 32767)
        if not -32767 <= value <= 32767:
            raise ValueError("Clipped generated sample")
        pcm.extend(struct.pack("<h", value))
    with wave.open(stream, "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(RATE)
        output.writeframes(pcm)
    return stream.getvalue()


def tone(frequency):
    count = 5292
    return wav_bytes(0.20 * min(1.0, i / 220.0, (count - 1 - i) / 441.0)
                     * math.sin(2 * math.pi * frequency * i / RATE)
                     for i in range(count))


def build_outputs():
    files = {f"m7-mini-tone-{name}.wav": tone(hz)
             for name, hz in (("low", 220), ("mid", 440), ("high", 660))}
    # Eight regular quarter notes, eight faster eighth notes, then two soft hits.
    # Deliberate silence at both ends and between sections exposes false positives.
    events = [(0.5 + i * 0.5, "kick" if i % 2 == 0 else "snare", 0.65)
              for i in range(8)]
    events += [(5.0 + i * 0.25, "hat", 0.45) for i in range(8)]
    events += [(8.0, "kick", 0.22), (9.5, "snare", 0.22)]
    samples = [0.0] * round(DURATION * RATE)
    for event_index, (start, kind, amplitude) in enumerate(events):
        length = round((0.18 if kind == "kick" else 0.10 if kind == "snare" else 0.055) * RATE)
        offset = round(start * RATE)
        seed = 12345 + event_index * 997
        phase = 0.0
        for i in range(length):
            t = i / RATE
            seed = (1664525 * seed + 1013904223) & 0xFFFFFFFF
            noise = seed / 2147483648.0 - 1.0
            attack = min(1.0, i / (RATE * 0.001))
            release = min(1.0, (length - 1 - i) / (RATE * 0.005))
            if kind == "kick":
                phase += 2 * math.pi * (55 + 110 * math.exp(-t * 45)) / RATE
                value = math.sin(phase) * math.exp(-t * 25) + 0.12 * noise * math.exp(-t * 200)
            elif kind == "snare":
                value = (0.8 * noise + 0.2 * math.sin(2 * math.pi * 180 * t)) * math.exp(-t * 50)
            else:
                value = noise * math.exp(-t * 100)
            samples[offset + i] += amplitude * attack * release * value
    files["slice-onsets.wav"] = wav_bytes(samples)
    manifest = {
        "description": "Original synthetic percussion; source onsets, not guaranteed detector output.",
        "audio": "slice-onsets.wav", "sample_rate": RATE, "channels": 1,
        "duration_seconds": DURATION, "bpm": 120, "event_count": len(events),
        "events": [{"time_seconds": start, "sample_frame": round(start * RATE),
                    "kind": kind, "amplitude": amplitude} for start, kind, amplitude in events],
        "sections": {"regular_hits": [0.5, 4.0], "fast_hits": [5.0, 6.75],
                     "soft_hits": [8.0, 9.5]},
        "notes": ["Leading silence and trailing silence are intentional.",
                  "Soft hits test sensitivity; hats test minimum-gap settings.",
                  "Slice boundaries include leading silence; slice count need not equal event count.",
                  "This fixture diagnoses behavior; it does not prove detection quality on music."],
        "sha256": {name: hashlib.sha256(data).hexdigest() for name, data in files.items()},
    }
    files["slice-onsets.json"] = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--overwrite", action="store_true", help="explicitly regenerate managed audio/manifest, never charts")
    parser.add_argument("--check", action="store_true", help="verify all managed files without writing")
    args = parser.parse_args()
    folder = args.output_dir.resolve()
    outputs = build_outputs()
    # Preflight every managed path before any writes; do not partially replace a fixture.
    for name, data in outputs.items():
        path = folder / name
        if path.is_symlink() or (path.exists() and not path.is_file()):
            parser.error(f"Not a regular managed output: {path}")
        if args.check and (not path.exists() or path.read_bytes() != data):
            parser.error(f"Missing or modified fixture: {path}")
        if not args.check and path.exists() and path.read_bytes() != data and not args.overwrite:
            parser.error(f"Preserving existing file: {path}; use --overwrite explicitly")
    if args.check:
        print(f"Verified {len(outputs)} managed audio/manifest files in {folder}")
        return
    folder.mkdir(parents=True, exist_ok=True)
    for name, data in outputs.items():
        path = folder / name
        if path.exists() and path.read_bytes() == data:
            print(f"Keep {path}")
            continue
        path.write_bytes(data)
        print(f"Write {path} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
