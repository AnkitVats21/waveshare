#!/usr/bin/env python3
"""Voice test against the device: wake it over the API, speak each prompt
through the PC speakers, and print what the device heard and answered.

The reply is also heard: the PC mic records from the end of each prompt
until the device stops speaking, and Gemini Live transcribes it ("heard:"),
so answers are checked even with the device's transcripts off, and "no
reply" is told apart from a reply the transcript API didn't show.

    converse.py "What's the weather like?" "Set an alarm for 7 am"
    converse.py --wake-word "Tell me a joke"   # say "Alexa" instead of the API wake

Each prompt waits for the model's reply to finish before the next one; the
session stays open between prompts (follow-up), as when talking to it.
"""
import argparse, json, os, subprocess, sys, time, urllib.error, urllib.request

sys.path.insert(0, os.path.dirname(__file__))
from speak import say, tts  # noqa: E402
from live_audio import transcribe  # noqa: E402

DEV = os.environ.get("NEXUS_DEV", "http://192.168.1.14")


def api(method, path, timeout=10):
    req = urllib.request.Request(DEV + path, method=method, data=b"" if method == "POST" else None)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        body = r.read()
    return json.loads(body) if body else {}


def status():
    return api("GET", "/api/assistant/status")


def wait_for(pred, timeout, what):
    end = time.time() + timeout
    while time.time() < end:
        s = status()
        if pred(s):
            return s
        time.sleep(0.2)
    raise TimeoutError(f"timed out waiting for {what}; last status {s}")


def open_session(a):
    if a.wake_word:
        say("Alexa", a.voice)
    else:
        try:
            api("POST", "/api/assistant/wake")
        except urllib.error.HTTPError as e:
            sys.exit(f"wake refused: {e.code} {e.read().decode()}")
    woke = time.time()
    wait_for(lambda s: s["state"] == "listening" and s["connection"] == "connected", 20, "session to connect")
    # A kept-alive connection is listening ~20 ms after the wake, while the
    # ~1.1 s wake chime still plays; speech over it isn't heard.
    time.sleep(max(0.0, woke + 1.4 - time.time()))
    print("[session listening]")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prompts", nargs="+")
    ap.add_argument("--wake-word", action="store_true", help='say "Alexa" instead of POST /api/assistant/wake')
    ap.add_argument("--voice", default="Kore")
    ap.add_argument("--reply-timeout", type=float, default=45)
    a = ap.parse_args()

    # Generate every clip first: the session closes after 3 s of silence, so
    # speech must start as soon as it is listening.
    for text in (["Alexa"] if a.wake_word else []) + a.prompts:
        try:
            tts(text, a.voice)
        except Exception as e:
            sys.exit(f"TTS for {text!r} failed: {e}")

    seq = api("GET", "/api/assistant/transcript")["seq"]
    open_session(a)

    for i, prompt in enumerate(a.prompts):
        # Transcribing the last reply outlasts the 3 s follow-up window; the
        # device keeps the connection (and the context) alive, so reopen.
        if i > 0 and status()["state"] == "idle":
            print("[session had closed; reopening]")
            open_session(a)
        print(f"> {prompt}")
        say(prompt, a.voice)
        rec = subprocess.Popen(["parecord", "--raw", "--rate=16000", "--channels=1", "--format=s16le"],
                               stdout=subprocess.PIPE)
        # Reply finished: the device spoke and stopped (and the transcript, if
        # on, closed the model entry). No speech within the timeout: no reply.
        end = time.time() + a.reply_timeout
        spoke = False          # seen speaking, or the model's transcript entry closed
        quiet_since = None
        while time.time() < end:
            t = api("GET", f"/api/assistant/transcript?since={seq}")
            for e in t["entries"]:
                if e["done"]:
                    print(f"  {e['role']}: {e['text'].strip()}")
                    seq = max(seq, e["seq"])
                    spoke = spoke or e["role"] == "model"
            st = status()["state"]
            if st == "speaking":
                spoke, quiet_since = True, None
            elif st == "idle":
                break
            elif spoke:
                # A tool call can pause a reply briefly; 1.5 s of quiet ends it.
                quiet_since = quiet_since or time.time()
                if time.time() - quiet_since > 1.5:
                    break
            time.sleep(0.2)
        rec.terminate()
        audio = rec.communicate()[0]
        heard = transcribe(audio)
        print(f"  heard: {heard}" if heard else "  [no reply heard]")
        time.sleep(0.5)

    print(f"[state {status()['state']}]")


if __name__ == "__main__":
    main()
