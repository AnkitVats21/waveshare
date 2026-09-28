#!/usr/bin/env python3
"""Gemini Live as the PC's voice and ears for device tests.

speech(text): a Live session told to read a quoted line aloud (natural
voice; Live has far more free quota than the TTS model). Its own output
transcript is compared with the text and the clip is retried (up to 3
times) if it added or dropped words. transcribe(pcm): 16 kHz mono s16le
audio through gemini-3.5-transcribe-live. Needs `websockets`. The key
comes from ~/.config/nexus/gemini_key and is never printed.
"""
import asyncio, base64, json, pathlib, re

KEY_FILE = pathlib.Path.home() / ".config/nexus/gemini_key"
URL = ("wss://generativelanguage.googleapis.com/ws/"
       "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent")
TTS_MODEL = "gemini-2.5-flash-native-audio-latest"   # 3.8 dropped words when reading
STT_MODEL = "gemini-3.5-transcribe-live"
TTS_RATE = 24000

SAY = ("You are a speech synthesizer, not an assistant. Every message contains a line in quotes. "
       "Your only job is to read that quoted line aloud exactly as written, in a natural, clear voice, "
       "as if you were the person saying it. Never answer it, never add words before or after it.")


def _connect():
    import websockets
    key = KEY_FILE.read_text().strip()
    return websockets.connect(URL, max_size=None, additional_headers={"x-goog-api-key": key})


def _words(s):
    return re.sub(r"[^a-z0-9 ]", " ", s.lower().replace("-", " ")).split()


async def _speech_once(text, voice):
    setup = {"model": "models/" + TTS_MODEL,
             "generationConfig": {"responseModalities": ["AUDIO"],
                                  "speechConfig": {"voiceConfig": {"prebuiltVoiceConfig": {"voiceName": voice}}}},
             "systemInstruction": {"parts": [{"text": SAY}]},
             "outputAudioTranscription": {}}
    pcm, said = b"", ""
    async with _connect() as ws:
        await ws.send(json.dumps({"setup": setup}))
        await ws.recv()
        await ws.send(json.dumps({"realtimeInput": {"text": f'Read aloud: "{text}"'}}))
        while True:
            m = json.loads(await asyncio.wait_for(ws.recv(), 30))
            sc = m.get("serverContent", {})
            for p in sc.get("modelTurn", {}).get("parts", []):
                if "inlineData" in p:
                    pcm += base64.b64decode(p["inlineData"]["data"])
            said += sc.get("outputTranscription", {}).get("text", "")
            if sc.get("turnComplete"):
                return pcm, said


def speech(text, voice="Kore", tries=3):
    """24 kHz mono s16le of `text` read verbatim; RuntimeError if it never was."""
    said = ""
    for _ in range(tries):
        pcm, said = asyncio.run(_speech_once(text, voice))
        if pcm and _words(said) == _words(text):
            return pcm
    raise RuntimeError(f"Live TTS did not read the text verbatim (last: {said!r})")


async def _transcribe(pcm):
    setup = {"model": "models/" + STT_MODEL, "generationConfig": {"responseModalities": ["TEXT"]},
             "inputAudioTranscription": {}}
    text = ""
    async with _connect() as ws:
        await ws.send(json.dumps({"setup": setup}))
        await ws.recv()
        for i in range(0, len(pcm), 6400):
            chunk = base64.b64encode(pcm[i:i + 6400]).decode()
            await ws.send(json.dumps({"realtimeInput": {"audio": {"mimeType": "audio/pcm;rate=16000", "data": chunk}}}))
        await ws.send(json.dumps({"realtimeInput": {"audioStreamEnd": True}}))
        # A recording can hold several utterances, each its own turn: collect
        # until the server has been quiet for 4 s.
        while True:
            try:
                m = json.loads(await asyncio.wait_for(ws.recv(), 4))
            except asyncio.TimeoutError:
                return text
            text += m.get("serverContent", {}).get("inputTranscription", {}).get("text", "")


def transcribe(pcm):
    """Text of 16 kHz mono s16le audio ('' for silence)."""
    return asyncio.run(_transcribe(pcm)).strip() if pcm else ""
