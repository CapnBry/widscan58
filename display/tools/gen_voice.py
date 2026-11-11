#!/usr/bin/env python3
# GPL-3.0-or-later.
"""Generate the lap timer's spoken lap times (display/voice/voice.bin and
display/src/voice_clips.h) with a text-to-speech model on OpenRouter.

A lap time is said word by word from 30 clips: "zero" to "nineteen",
"twenty" to "ninety", "hundred" and "point" (src/audio.c puts them together):

  "twenty" "three" "point" "nine"            23.9 s
  "one" "hundred" "four" "point" "two"       104.2 s

Needs numpy and the OPENROUTER_API_KEY environment variable. The model's raw
replies are kept in display/voice/cache/ (not tracked), so a rerun with other
processing settings costs nothing; --refresh asks the model again.

Usage: OPENROUTER_API_KEY=... tools/gen_voice.py [--voice Leda] [--rate 16000]
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import pathlib
import sys
import time
import urllib.error
import urllib.request

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parent.parent
VOICE_DIR = ROOT / "voice"
CACHE = VOICE_DIR / "cache"
BIN = VOICE_DIR / "voice.bin"
HEADER = ROOT / "src" / "voice_clips.h"

URL = "https://openrouter.ai/api/v1/audio/speech"
MODEL = "google/gemini-3.8-flash-tts"
SRC_RATE = 24000  # the model's PCM: 24 kHz, 16-bit little endian, mono

ONES = "zero one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen".split()
TENS = "_ _ twenty thirty forty fifty sixty seventy eighty ninety".split()


def clip_texts():
    """In clip order; the header's VOICE_* macros index this list."""
    return ONES + TENS[2:] + ["hundred", "point"]


def cache_path(text, voice, instructions):
    """Without instructions, the name the first clips were cached under."""
    key = f"{MODEL}|{voice}|{text}" + (f"|{instructions}" if instructions else "")
    h = hashlib.sha1(key.encode()).hexdigest()[:12]
    tag = "-i" + hashlib.sha1(instructions.encode()).hexdigest()[:6] if instructions else ""
    return CACHE / f"{voice}{tag}-{text.replace(' ', '_')}-{h}.pcm"


def fetch(text, voice, key, refresh, instructions=""):
    path = cache_path(text, voice, instructions)
    if path.exists() and not refresh:
        return path.read_bytes()
    if not key:
        raise RuntimeError(f"{text!r} is not cached and OPENROUTER_API_KEY is not set")
    req_body = {"model": MODEL, "input": text, "voice": voice, "response_format": "pcm"}
    if instructions:
        req_body["instructions"] = instructions
    body = json.dumps(req_body).encode()
    req = urllib.request.Request(URL, body, {"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    for attempt in range(5):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                ctype = r.headers.get("Content-Type", "")
                data = r.read()
            break
        except urllib.error.HTTPError as e:
            # 402 and 429 come and go with several requests at once
            if e.code not in (402, 429, 500, 502, 503) or attempt == 4:
                raise RuntimeError(f"{text!r}: HTTP {e.code}: {e.read()[:300]!r}") from None
            time.sleep(2 * 2**attempt)
    if not ctype.startswith("audio/pcm") or f"rate={SRC_RATE}" not in ctype:
        raise RuntimeError(f"{text!r}: unexpected reply {ctype}: {data[:200]!r}")
    CACHE.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return data


def lowpass(cutoff, rate, taps=95):
    n = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * cutoff / rate * n) * np.blackman(taps)
    return h / h.sum()


def resample(x, rate):
    """24 kHz to rate, through a common multiple (16000: up 2, down 3)."""
    if rate == SRC_RATE:
        return x
    g = np.gcd(SRC_RATE, rate)
    up, down = rate // g, SRC_RATE // g
    y = np.zeros(len(x) * up)
    y[::up] = x * up
    y = np.convolve(y, lowpass(0.45 * min(rate, SRC_RATE), SRC_RATE * up, 32 * max(up, down) + 1), "same")
    return y[::down]


def trim(x, rate, floor_db, pad_ms):
    """Cut the silence before and after the speech, keeping pad_ms."""
    win = max(1, rate // 200)  # 5 ms envelope
    env = np.sqrt(np.convolve(x * x, np.ones(win) / win, "same"))
    loud = np.nonzero(env > np.max(env) * 10 ** (floor_db / 20))[0]
    if len(loud) == 0:
        raise RuntimeError("silent clip")
    pad = rate * pad_ms // 1000
    return x[max(0, loud[0] - pad) : loud[-1] + pad]


def fade(x, rate, ms):
    n = min(len(x) // 2, rate * ms // 1000)
    r = 0.5 - 0.5 * np.cos(np.linspace(0, np.pi, n))
    x[:n] *= r
    x[len(x) - n :] *= r[::-1]
    return x


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--voice", default="Leda")
    ap.add_argument("--rate", type=int, default=16000, help="output sample rate, Hz (the DAC's)")
    ap.add_argument("--floor-db", type=float, default=-32, help="trim below this, dB under the clip's peak")
    ap.add_argument("--pad-ms", type=int, default=0, help="kept around the speech when trimming")
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--refresh", action="store_true", help="ask the model again for every clip")
    ap.add_argument("--instructions", default="", help="delivery instructions for the model (cached separately)")
    ap.add_argument("--only", help="comma-separated words: fetch just these into the cache, leave voice.bin")
    a = ap.parse_args()
    key = os.environ.get("OPENROUTER_API_KEY")
    texts = clip_texts()
    if a.only:
        texts = [t for t in a.only.split(",") if t in texts] if all(t in texts for t in a.only.split(",")) else sys.exit(f"--only: not clip words: {a.only}")
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        raw = list(ex.map(lambda t: fetch(t, a.voice, key, a.refresh, a.instructions), texts))
    if a.only:  # trying out: fetched into the cache, voice.bin left as it is
        for t in texts:
            print(cache_path(t, a.voice, a.instructions))
        return

    clips = []
    for t, r in zip(texts, raw):
        x = np.frombuffer(r, "<i2").astype(np.float64) / 32768
        x = trim(resample(x, a.rate), a.rate, a.floor_db, a.pad_ms)
        clips.append(x)
    # One gain for every clip, so their loudness stays as the model said them;
    # the 99.95th percentile of the peaks, the rare louder sample is clipped.
    gain = 0.95 / np.percentile(np.abs(np.concatenate(clips)), 99.95)
    rng = np.random.default_rng(1)
    out, table = bytearray(), []
    for t, x in zip(texts, clips):
        x = fade(x * gain, a.rate, 5)
        d = rng.random(len(x)) - rng.random(len(x))  # TPDF dither, 1 LSB
        u = np.clip(np.round(x * 127 + 128 + d), 0, 255).astype(np.uint8)
        table.append((len(out), len(u), t))
        out += u.tobytes()
        print(f"{t:28s} {len(u) / a.rate * 1000:5.0f} ms  rms {20 * np.log10(np.sqrt(np.mean(x * x)) + 1e-9):6.1f} dBFS")

    BIN.write_bytes(out)
    lines = [
        "/* Generated by tools/gen_voice.py: do not edit. GPL-3.0-or-later. */",
        f"/* {MODEL}, voice {a.voice}; {len(out)} bytes of 8-bit unsigned PCM in voice.bin. */",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"#define VOICE_RATE {a.rate}",
        "#define VOICE_ONES(n) (n)       /* 0-19 */",
        "#define VOICE_TENS(t) (18 + (t)) /* 2-9: twenty to ninety */",
        "#define VOICE_HUNDRED 28",
        "#define VOICE_POINT 29",
        f"#define VOICE_CLIPS {len(table)}",
        "",
        "static const struct { uint32_t off, len; } voice_clips[VOICE_CLIPS] = {",
    ]
    lines += [f"    {{{o}, {n}}}, /* {t} */" for o, n, t in table]
    lines += ["};", ""]
    HEADER.write_text("\n".join(lines))
    print(f"{len(table)} clips, {len(out)} bytes, {len(out) / a.rate:.1f} s at {a.rate} Hz: {BIN.relative_to(ROOT)}, {HEADER.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
