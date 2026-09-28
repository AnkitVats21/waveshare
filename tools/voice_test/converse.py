#!/usr/bin/env python3
"""Voice test against the device: wake it over the API, speak each prompt
through the PC speakers, and print what the device heard and answered.

    converse.py "What's the weather like?" "Set an alarm for 7 am"
    converse.py --wake-word "Tell me a joke"   # say "Alexa" instead of the API wake

Each prompt waits for the model's reply to finish before the next one; the
session stays open between prompts (follow-up), as when talking to it.
"""
import argparse, json, os, sys, time, urllib.error, urllib.request

sys.path.insert(0, os.path.dirname(__file__))
from speak import say, tts  # noqa: E402

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
    if a.wake_word:
        say("Alexa", a.voice)
    else:
        try:
            api("POST", "/api/assistant/wake")
        except urllib.error.HTTPError as e:
            sys.exit(f"wake refused: {e.code} {e.read().decode()}")
    wait_for(lambda s: s["state"] == "listening" and s["connection"] == "connected", 20, "session to connect")
    print("[session listening]")

    for prompt in a.prompts:
        print(f"> {prompt}")
        say(prompt, a.voice)
        # Reply finished: model entry closed by turnComplete, and playback done.
        end = time.time() + a.reply_timeout
        model_done = False
        while time.time() < end:
            t = api("GET", f"/api/assistant/transcript?since={seq}")
            for e in t["entries"]:
                if e["done"]:
                    print(f"  {e['role']}: {e['text'].strip()}")
                    if e["role"] == "model":
                        model_done = True
                    seq = max(seq, e["seq"])
            if model_done and status()["state"] != "speaking":
                break
            time.sleep(0.3)
        else:
            print("  [no complete reply before timeout]")
        time.sleep(0.5)

    print(f"[state {status()['state']}]")


if __name__ == "__main__":
    main()
