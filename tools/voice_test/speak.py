#!/usr/bin/env python3
"""Say text out loud through the PC speakers, for voice tests near the device.

    speak.py "What time is it?"
    speak.py --voice Kore --espeak "Alexa"

Uses Gemini TTS (natural voice) with the key in ~/.config/nexus/gemini_key,
caching each phrase in ~/.cache/nexus_tts so repeats cost nothing. Falls back
to espeak-ng if TTS fails. The key is never printed.
"""
import argparse, base64, hashlib, json, pathlib, subprocess, sys, urllib.error, urllib.request, wave

KEY_FILE = pathlib.Path.home() / ".config/nexus/gemini_key"
CACHE = pathlib.Path.home() / ".cache/nexus_tts"
MODEL = "gemini-2.5-flash-preview-tts"
RATE = 24000  # Gemini TTS returns 24 kHz mono s16le


def tts(text, voice):
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / (hashlib.sha1(f"{MODEL}|{voice}|{text}".encode()).hexdigest() + ".wav")
    if path.exists():
        return path
    key = KEY_FILE.read_text().strip()
    body = {
        "contents": [{"parts": [{"text": text}]}],
        "generationConfig": {
            "responseModalities": ["AUDIO"],
            "speechConfig": {"voiceConfig": {"prebuiltVoiceConfig": {"voiceName": voice}}},
        },
    }
    req = urllib.request.Request(
        f"https://generativelanguage.googleapis.com/v1beta/models/{MODEL}:generateContent",
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "x-goog-api-key": key},
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            data = json.load(r)
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"TTS HTTP {e.code}: {e.read()[:300].decode(errors='replace').replace(key, '<key>')}")
    pcm = base64.b64decode(data["candidates"][0]["content"]["parts"][0]["inlineData"]["data"])
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)
    return path


def say(text, voice="Kore", espeak=False):
    if not espeak:
        try:
            subprocess.run(["pw-play", str(tts(text, voice))], check=True)
            return
        except Exception as e:
            print(f"speak: Gemini TTS failed ({e}); using espeak-ng", file=sys.stderr)
    subprocess.run(["espeak-ng", "-s", "150", text], check=True)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("text")
    ap.add_argument("--voice", default="Kore")
    ap.add_argument("--espeak", action="store_true", help="use espeak-ng instead of Gemini TTS")
    a = ap.parse_args()
    say(a.text, a.voice, a.espeak)
