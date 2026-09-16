# /// script
# requires-python = ">=3.11,<3.12"
# dependencies = ["pyopenjtalk-prebuilt==0.3.0", "numpy<2", "setuptools<81"]
# ///
"""Render the Japanese utterances vru_selftest pushes through model-ja.

Open JTalk speaks them, so they are made again rather than kept as binaries:
48 kHz mono 16-bit, the format vru_selftest and RetroXR's vru_probe read.

    uv run tools/vru_render_ja_clips.py <out dir>
    vru_selftest <system dir> --ja-clips=<out dir>

The pins are what makes it run at all: pyopenjtalk-prebuilt 0.3.0 has wheels
up to CPython 3.11, was built against numpy 1, and imports pkg_resources,
which setuptools 81 removes. The first run downloads Open JTalk's dictionary.

A synthesized voice is the easy case, clearer than any player, so a pass
proves the path -- decode, lexicon, grammar, match -- and not how well a
person is heard.
"""

import os
import sys
import wave

import numpy as np
import pyopenjtalk

RATE = 48000
PAD_SECONDS = 0.25

# Spelled as the game sends them: two words from the selftest's list -- one the
# model has whole, one it has only as two words -- and one the list lacks.
CLIPS = {
    "ja_pikachu.wav": "ぴかちゅう",
    "ja_game_start.wav": "げーむすたーと",
    "ja_banana.wav": "ばなな",
}


def render(text, path):
    samples, rate = pyopenjtalk.tts(text)
    if rate != RATE:
        raise SystemExit(f"Open JTalk spoke at {rate} Hz, not {RATE}")
    pad = np.zeros(int(RATE * PAD_SECONDS))
    pcm = np.clip(np.concatenate([pad, samples, pad]), -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(pcm.tobytes())
    print(f"{path}: {text}, {len(pcm) / RATE:.2f} s")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: vru_render_ja_clips.py <out dir>")
    os.makedirs(sys.argv[1], exist_ok=True)
    for name, text in CLIPS.items():
        render(text, os.path.join(sys.argv[1], name))


if __name__ == "__main__":
    main()
