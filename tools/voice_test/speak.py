#!/usr/bin/env python3
"""Say text out loud through the PC speakers, for voice tests near the device.

    speak.py "What time is it?"
    speak.py --voice Kore --espeak "Alexa"

Speech comes from a Gemini Live session reading the text verbatim
(live_audio.py; checked against its own transcript), falling back to the
Gemini TTS model (small free quota) with the key in
~/.config/nexus/gemini_key, then gemini_key2 on a 429. Each phrase is
cached in ~/.cache/nexus_tts so repeats cost nothing. If both fail it stops
rather than fall back to espeak-ng, which the model mishears; espeak-ng
only with --espeak. Keys are never printed.
"""
import argparse, base64, hashlib, json, pathlib, subprocess, sys, urllib.error, urllib.request, wave

KEY_FILES = [pathlib.Path.home() / ".config/nexus" / n for n in ("gemini_key", "gemini_key2")]
CACHE = pathlib.Path.home() / ".cache/nexus_tts"
MODEL = "gemini-2.5-flash-preview-tts"
RATE = 24000  # Gemini TTS returns 24 kHz mono s16le


def _write_wav(path, pcm):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)


def tts(text, voice):
    CACHE.mkdir(parents=True, exist_ok=True)
    for model in ("live", MODEL):
        path = CACHE / (hashlib.sha1(f"{model}|{voice}|{text}".encode()).hexdigest() + ".wav")
        if path.exists():
            return path
    try:
        from live_audio import speech
        path = CACHE / (hashlib.sha1(f"live|{voice}|{text}".encode()).hexdigest() + ".wav")
        _write_wav(path, speech(text, voice))
        return path
    except Exception as e:
        print(f"Live TTS failed ({e}); trying the TTS model", file=sys.stderr)
    path = CACHE / (hashlib.sha1(f"{MODEL}|{voice}|{text}".encode()).hexdigest() + ".wav")
    body = {
        "contents": [{"parts": [{"text": text}]}],
        "generationConfig": {
            "responseModalities": ["AUDIO"],
            "speechConfig": {"voiceConfig": {"prebuiltVoiceConfig": {"voiceName": voice}}},
        },
    }
    data, err = None, "no key file"
    for key_file in KEY_FILES:
        if not key_file.exists():
            continue
        key = key_file.read_text().strip()
        req = urllib.request.Request(
            f"https://generativelanguage.googleapis.com/v1beta/models/{MODEL}:generateContent",
            data=json.dumps(body).encode(),
            headers={"Content-Type": "application/json", "x-goog-api-key": key},
        )
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                data = json.load(r)
            break
        except urllib.error.HTTPError as e:
            err = f"TTS HTTP {e.code} ({key_file.name}): {e.read()[:200].decode(errors='replace').replace(key, '<key>')}"
            if e.code != 429:
                break
    if data is None:
        raise RuntimeError(err)
    _write_wav(path, base64.b64decode(data["candidates"][0]["content"]["parts"][0]["inlineData"]["data"]))
    return path


def say(text, voice="Kore", espeak=False):
    if espeak:
        subprocess.run(["espeak-ng", "-s", "150", text], check=True)
    else:
        subprocess.run(["pw-play", str(tts(text, voice))], check=True)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("text")
    ap.add_argument("--voice", default="Kore")
    ap.add_argument("--espeak", action="store_true", help="use espeak-ng instead of Gemini TTS")
    a = ap.parse_args()
    say(a.text, a.voice, a.espeak)
