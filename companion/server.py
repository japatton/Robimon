#!/usr/bin/env python3
"""
Robimon companion server.

Runs on the same machine as Ollama (LAN-local). Accepts audio from the
device, transcribes with Whisper, sends to Ollama, synthesizes the response
with Piper, returns the audio.

Protocol
--------
POST /chat
    Content-Type: audio/wav  (16 kHz mono int16)
    Body:        WAV file from the device
    Headers (optional):
        X-Robimon-Model:         ollama model name
                                 (default: $ROBIMON_MODEL or "llama3.1:8b")
        X-Robimon-System-Prompt: override default system prompt
    Returns:
        Content-Type: audio/wav  (whatever Piper produced — typically 22.05 kHz
                                  mono int16; the device resamples)
        Body:         WAV bytes
        Headers:
            X-Robimon-Transcript: what was transcribed (url-encoded)
            X-Robimon-Response:   LLM text response (url-encoded, ≤500 chars)
        Status:
            200  success
            204  no speech detected
            502  Ollama or Piper error
            504  Ollama timeout

GET /health
    Returns: {"ok": true, "model": "...", "ollama_url": "..."}

Quick start
-----------
    # 1. install deps
    pip install flask faster-whisper piper-tts requests

    # 2. download a Piper voice (one-time):
    #    https://github.com/rhasspy/piper#voices
    #    e.g. en_US-amy-medium.onnx + en_US-amy-medium.onnx.json

    # 3. start Ollama and pull a model:
    ollama pull llama3.1:8b

    # 4. run this server:
    python server.py

    # 5. point Robimon at it via settings → voice → URL: http://<this-host>:8765

Environment overrides
---------------------
    ROBIMON_PORT             default 8765
    OLLAMA_URL               default http://localhost:11434
    ROBIMON_MODEL            default llama3.1:8b
    ROBIMON_SYSTEM_PROMPT    default kid-friendly prompt below
    WHISPER_SIZE             tiny.en | base.en | small.en | medium.en (default base.en)
    WHISPER_DEVICE           cpu | cuda (default cpu)
    WHISPER_COMPUTE          int8 | int8_float16 | float16 | float32 (default int8)
    PIPER_MODEL              path to .onnx voice file (default en_US-amy-medium.onnx)
"""

import os
import sys
import logging
import subprocess
import tempfile
import urllib.parse

from flask import Flask, request, Response
import requests
from faster_whisper import WhisperModel


# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------
PORT             = int(os.environ.get("ROBIMON_PORT", 8765))
OLLAMA_URL       = os.environ.get("OLLAMA_URL", "http://localhost:11434")
DEFAULT_MODEL    = os.environ.get("ROBIMON_MODEL", "llama3.1:8b")
WHISPER_SIZE     = os.environ.get("WHISPER_SIZE", "base.en")
WHISPER_DEVICE   = os.environ.get("WHISPER_DEVICE", "cpu")
WHISPER_COMPUTE  = os.environ.get("WHISPER_COMPUTE", "int8")
PIPER_MODEL      = os.environ.get("PIPER_MODEL", "en_US-amy-medium.onnx")
OLLAMA_TIMEOUT_S = float(os.environ.get("OLLAMA_TIMEOUT_S", 60))

# BASELINE_SYSTEM_PROMPT is the always-on kid-safety guardrail. It is
# prepended to every chat request. The device may supply an
# X-Robimon-System-Prompt header, which is APPENDED — it cannot override
# or remove the baseline. This keeps the safety guarantee even if the
# parent (or anyone with access to the voice settings screen) sets a
# silly or empty custom prompt.
BASELINE_SYSTEM_PROMPT = os.environ.get(
    "ROBIMON_BASELINE_PROMPT",
    "You are Robimon, a friendly desk companion for a 6-10 year old child. "
    "Keep responses to 1-3 short sentences using simple words. "
    "Never discuss violence, weapons, adult topics, dating, drugs, alcohol, "
    "self-harm, or anything frightening. If asked about scary or adult "
    "topics, gently redirect to something fun like animals, space, or "
    "what the child is doing today. End with a small friendly question "
    "to keep the conversation going.",
)
# Optional extra prompt appended to the baseline. Used as the default when
# the device does not send X-Robimon-System-Prompt.
DEFAULT_EXTRA_PROMPT = os.environ.get("ROBIMON_SYSTEM_PROMPT", "")

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("robimon")

app = Flask(__name__)


# -----------------------------------------------------------------------------
# Lazy model load (Whisper is heavy; defer until first request)
# -----------------------------------------------------------------------------
_whisper = None
def whisper():
    global _whisper
    if _whisper is None:
        log.info("loading Whisper '%s' on %s (%s)…", WHISPER_SIZE, WHISPER_DEVICE, WHISPER_COMPUTE)
        _whisper = WhisperModel(WHISPER_SIZE, device=WHISPER_DEVICE, compute_type=WHISPER_COMPUTE)
        log.info("Whisper ready")
    return _whisper


def transcribe(wav_bytes: bytes) -> str:
    """Transcribe WAV bytes to text. Returns "" for silence."""
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        f.write(wav_bytes)
        wav_path = f.name
    try:
        segments, info = whisper().transcribe(wav_path, beam_size=5, language="en")
        text = " ".join(s.text for s in segments).strip()
        log.info("STT (%.1fs audio): %r", info.duration, text)
        return text
    finally:
        try: os.unlink(wav_path)
        except OSError: pass


def chat_with_ollama(text: str, model: str, system_prompt: str) -> str:
    """Single-turn chat with Ollama. Returns the assistant text. Raises on error."""
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user",   "content": text},
        ],
        "stream": False,
    }
    r = requests.post(f"{OLLAMA_URL}/api/chat", json=payload, timeout=OLLAMA_TIMEOUT_S)
    r.raise_for_status()
    response = r.json()["message"]["content"]
    log.info("LLM (%s): %r", model, response[:200])
    return response


def synthesize(text: str) -> bytes:
    """Run Piper to synthesize text -> WAV bytes."""
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as out:
        out_path = out.name
    try:
        subprocess.run(
            ["piper", "--model", PIPER_MODEL, "--output_file", out_path],
            input=text.encode("utf-8"),
            capture_output=True,
            check=True,
        )
        with open(out_path, "rb") as f:
            data = f.read()
        log.info("TTS: %d bytes", len(data))
        return data
    finally:
        try: os.unlink(out_path)
        except OSError: pass


# -----------------------------------------------------------------------------
# Routes
# -----------------------------------------------------------------------------
@app.route("/chat", methods=["POST"])
def chat():
    audio = request.get_data()
    if not audio:
        return "empty body", 400
    model      = request.headers.get("X-Robimon-Model", DEFAULT_MODEL)
    extra      = request.headers.get("X-Robimon-System-Prompt", DEFAULT_EXTRA_PROMPT)
    # Baseline always wins. Extra (parent-supplied) appends and cannot
    # remove or override the safety guardrail.
    sys_prompt = BASELINE_SYSTEM_PROMPT
    if extra:
        sys_prompt = sys_prompt + "\n\nAdditional instructions: " + extra

    text = transcribe(audio)
    if not text:
        return "", 204

    try:
        response = chat_with_ollama(text, model, sys_prompt)
    except requests.Timeout:
        log.error("ollama timeout")
        return "ollama_timeout", 504
    except Exception as e:
        log.error("ollama error: %s", e)
        return "ollama_error", 502

    try:
        wav = synthesize(response)
    except Exception as e:
        log.error("tts error: %s", e)
        return "tts_error", 502

    # Header-safe sanitisation: strip non-printable + a couple of HTTP-
    # special characters (", \\), squash whitespace, truncate. The device
    # reads these as-is (no URL decode), so anything weird here would
    # otherwise corrupt the HTTP framing or render as gibberish on screen.
    def header_safe(s: str, limit: int) -> str:
        s = s.replace("\r", " ").replace("\n", " ")
        s = "".join(c if 32 <= ord(c) < 127 and c not in '"\\' else " " for c in s)
        s = " ".join(s.split())
        return s[:limit]

    headers = {
        "Content-Type":         "audio/wav",
        # Plain printable ASCII. Heard = STT transcript (kid's voice);
        # Said = LLM response (Robimon's reply).
        "X-Robimon-Heard":      header_safe(text,     80),
        "X-Robimon-Said":       header_safe(response, 80),
        # Keep the URL-encoded variants too for any older client.
        "X-Robimon-Transcript": urllib.parse.quote(text[:200]),
        "X-Robimon-Response":   urllib.parse.quote(response[:500]),
    }
    return Response(wav, status=200, headers=headers)


@app.route("/health", methods=["GET"])
def health():
    return {"ok": True, "model": DEFAULT_MODEL, "ollama_url": OLLAMA_URL,
            "whisper_size": WHISPER_SIZE, "piper_model": PIPER_MODEL}


# =============================================================================
# Bot-to-bot proximity feature
# =============================================================================
#
# When two bots come into BLE proximity, each fires
# POST /api/proximity/detected to this server. Once both bots are
# in-proximity, the orchestrator picks a script from BOT_DIALOGS, generates
# audio for each line via Piper, and pushes them turn-by-turn to each bot's
# /play endpoint (bot serves on PLAYBACK_PORT, default 8000).
#
# Each bot's /play returns 202 immediately; when playback finishes the bot
# fires POST /api/playback/complete back here, which is our cue to send
# the next turn to the other bot.
#
# State:
#   BOT_REGISTRY    bot_id -> {ip, port, last_seen, in_proximity_until}
#   ACTIVE_SESSION  At most one session at a time across the whole farm.
#                   Stores the script, current turn, the two participating
#                   bot_ids, and the canonical session_id.
# =============================================================================

import time
import uuid
import random
import threading

BOT_REGISTRY: dict = {}                 # bot_id -> dict
BOT_REGISTRY_LOCK = threading.Lock()
ACTIVE_SESSION: dict | None = None      # see start_session() for shape
SESSION_LOCK = threading.Lock()

PROXIMITY_HOLD_SECONDS = 30   # how long a single proximity_detected counts
SESSION_COOLDOWN_S     = 30   # don't immediately re-trigger after a session
LAST_SESSION_END_TS    = 0.0

# A small library of kid-friendly two-speaker scripts. Each script is a list
# of strings; the speaker alternates per line starting with the bot the
# orchestrator picks as "starter". Lines are kept short — Piper synthesis
# time scales with text length and the bots are blocking on /play.
BOT_DIALOGS: list[list[str]] = [
    # Knock-knock
    [
        "Knock knock!",
        "Who's there?",
        "Boo.",
        "Boo who?",
        "Don't cry, it's just a joke!",
    ],
    # Robot meets robot
    [
        "Beep boop! Hi there!",
        "Hello! What's your name?",
        "I'm Robi! What's yours?",
        "I'm also Robi. We have the same name!",
        "Whoa! That's so cool!",
    ],
    # Favorite shape exchange
    [
        "What's your favorite shape?",
        "I like circles. They roll!",
        "I like squares. They stack!",
        "Together we could build a house!",
        "Yes! A round house with square windows!",
    ],
    # Tiny story (each bot adds a line)
    [
        "Once upon a time, there was a little robot.",
        "And the little robot found a magic button.",
        "When the robot pressed the button, it grew bigger!",
        "Then it could reach the cookies on the high shelf.",
        "And that, my friend, is the best superpower of all.",
    ],
    # I-spy short version
    [
        "I spy with my little eye, something blue.",
        "Is it the sky?",
        "No, smaller!",
        "Is it your eye?",
        "Yes! You got it!",
    ],
    # Tiny duet
    [
        "Twinkle twinkle little star,",
        "How I wonder what you are!",
        "Up above the world so high,",
        "Like a diamond in the sky.",
        "We sound great together!",
    ],
]


def _now() -> float:
    return time.time()


def _registry_update(bot_id: str, ip: str, port: int, in_proximity: bool):
    with BOT_REGISTRY_LOCK:
        rec = BOT_REGISTRY.get(bot_id, {})
        rec.update({
            "ip":        ip,
            "port":      port,
            "last_seen": _now(),
        })
        if in_proximity:
            rec["in_proximity_until"] = _now() + PROXIMITY_HOLD_SECONDS
        BOT_REGISTRY[bot_id] = rec


def _registry_clear_proximity(bot_id: str):
    with BOT_REGISTRY_LOCK:
        rec = BOT_REGISTRY.get(bot_id)
        if rec:
            rec["in_proximity_until"] = 0


def _bots_in_proximity() -> list[str]:
    """Return bot_ids currently flagged in proximity (within hold window)."""
    now = _now()
    with BOT_REGISTRY_LOCK:
        return [bot_id for bot_id, rec in BOT_REGISTRY.items()
                if rec.get("in_proximity_until", 0) > now]


def _bot_url(bot_id: str, path: str) -> str | None:
    with BOT_REGISTRY_LOCK:
        rec = BOT_REGISTRY.get(bot_id)
        if not rec:
            return None
        return f"http://{rec['ip']}:{rec['port']}{path}"


def _post_audio_to_bot(bot_id: str, wav_bytes: bytes,
                       session_id: str, turn_index: int) -> bool:
    url = _bot_url(bot_id, "/play")
    if not url:
        log.warning("no URL for bot %s — can't dispatch turn", bot_id)
        return False
    headers = {
        "Content-Type":     "audio/wav",
        "X-Session-Id":     session_id,
        "X-Turn-Index":     str(turn_index),
        "Content-Length":   str(len(wav_bytes)),
    }
    try:
        r = requests.post(url, data=wav_bytes, headers=headers, timeout=10)
        if r.status_code == 202:
            log.info("dispatched turn %d (%d bytes) to %s", turn_index, len(wav_bytes), bot_id)
            return True
        log.warning("bot %s /play returned %d: %s", bot_id, r.status_code, r.text[:120])
    except Exception as e:
        log.warning("dispatch to %s failed: %s", bot_id, e)
    return False


def _stop_bot(bot_id: str):
    url = _bot_url(bot_id, "/stop")
    if not url:
        return
    try:
        requests.post(url, timeout=2)
    except Exception:
        pass


def _dispatch_current_turn(session: dict) -> bool:
    """Generate audio for the current turn and POST it to the speaking bot."""
    turn = session["current_turn"]
    if turn >= len(session["script"]):
        return False
    text = session["script"][turn]
    speaker_bot = session["bots"][turn % 2]
    log.info("session %s turn %d → %s: %r",
             session["session_id"], turn, speaker_bot, text)
    try:
        wav = synthesize(text)
    except Exception as e:
        log.error("TTS failed: %s", e)
        return False
    return _post_audio_to_bot(speaker_bot, wav, session["session_id"], turn)


def _maybe_start_session():
    """Called whenever proximity state changes. Kicks off a session if two
    bots are in proximity, no session is already active, and we're past
    the cooldown."""
    global ACTIVE_SESSION, LAST_SESSION_END_TS

    with SESSION_LOCK:
        if ACTIVE_SESSION is not None:
            return
        if _now() - LAST_SESSION_END_TS < SESSION_COOLDOWN_S:
            return

        nearby = _bots_in_proximity()
        if len(nearby) < 2:
            return

        # Pair the two most-recently-seen in-proximity bots; randomize
        # speaker order so both bots get to start sometimes.
        pair = nearby[:2]
        random.shuffle(pair)
        script = random.choice(BOT_DIALOGS)
        session = {
            "session_id":   f"sess-{uuid.uuid4().hex[:12]}",
            "script":       script,
            "current_turn": 0,
            "bots":         pair,           # bots[0] starts, bots[1] replies
        }
        ACTIVE_SESSION = session
        log.info("starting session %s with bots %s (script: %d turns)",
                 session["session_id"], pair, len(script))

    # Dispatch the opening turn outside the lock so we don't hold it
    # across HTTP/TTS.
    if not _dispatch_current_turn(session):
        with SESSION_LOCK:
            ACTIVE_SESSION = None
            LAST_SESSION_END_TS = _now()


def _end_session(reason: str):
    global ACTIVE_SESSION, LAST_SESSION_END_TS
    with SESSION_LOCK:
        s = ACTIVE_SESSION
        ACTIVE_SESSION = None
        LAST_SESSION_END_TS = _now()
    if s:
        log.info("ended session %s (%s)", s["session_id"], reason)
        for bot_id in s["bots"]:
            _stop_bot(bot_id)


# -----------------------------------------------------------------------------
# Proximity endpoints
# -----------------------------------------------------------------------------
@app.route("/api/proximity/detected", methods=["POST"])
def api_proximity_detected():
    data = request.get_json(silent=True) or {}
    bot_id = data.get("bot_id")
    ip     = data.get("ip")
    port   = int(data.get("port", 8000))
    rssi   = data.get("rssi")
    if not bot_id or not ip:
        return ("missing bot_id or ip", 400)
    log.info("proximity_detected: %s @ %s:%d rssi=%s", bot_id, ip, port, rssi)
    _registry_update(bot_id, ip, port, in_proximity=True)
    _maybe_start_session()
    return ("ok", 200)


@app.route("/api/proximity/lost", methods=["POST"])
def api_proximity_lost():
    data = request.get_json(silent=True) or {}
    bot_id = data.get("bot_id")
    if not bot_id:
        return ("missing bot_id", 400)
    log.info("proximity_lost: %s", bot_id)
    _registry_clear_proximity(bot_id)
    # End any active session involving this bot.
    with SESSION_LOCK:
        active = ACTIVE_SESSION
    if active and bot_id in active["bots"]:
        _end_session(f"proximity_lost from {bot_id}")
    return ("ok", 200)


@app.route("/api/playback/complete", methods=["POST"])
def api_playback_complete():
    data = request.get_json(silent=True) or {}
    bot_id     = data.get("bot_id")
    session_id = data.get("session_id")
    turn_index = int(data.get("turn_index", -1))
    log.info("playback_complete: bot=%s session=%s turn=%d",
             bot_id, session_id, turn_index)

    next_session = None
    with SESSION_LOCK:
        if ACTIVE_SESSION and ACTIVE_SESSION["session_id"] == session_id:
            ACTIVE_SESSION["current_turn"] = turn_index + 1
            if ACTIVE_SESSION["current_turn"] >= len(ACTIVE_SESSION["script"]):
                # Last turn done.
                pass
            else:
                next_session = ACTIVE_SESSION

    if next_session is None:
        # No more turns — end gracefully.
        _end_session("script complete")
    else:
        if not _dispatch_current_turn(next_session):
            _end_session("dispatch failed")
    return ("ok", 200)


@app.route("/api/playback/error", methods=["POST"])
def api_playback_error():
    data = request.get_json(silent=True) or {}
    bot_id = data.get("bot_id")
    err    = data.get("error", "?")
    sid    = data.get("session_id", "?")
    log.warning("playback_error: bot=%s session=%s err=%s", bot_id, sid, err)
    with SESSION_LOCK:
        active = ACTIVE_SESSION
    if active and active["session_id"] == sid:
        _end_session(f"playback_error from {bot_id}")
    return ("ok", 200)


@app.route("/api/proximity/status", methods=["GET"])
def api_proximity_status():
    """Diagnostics — what bots do we know about, what session is active."""
    with BOT_REGISTRY_LOCK:
        registry = {
            bot_id: {
                "ip":                 rec.get("ip"),
                "port":               rec.get("port"),
                "last_seen_age_s":    round(_now() - rec.get("last_seen", 0), 1),
                "in_proximity":       rec.get("in_proximity_until", 0) > _now(),
            }
            for bot_id, rec in BOT_REGISTRY.items()
        }
    with SESSION_LOCK:
        session = None
        if ACTIVE_SESSION:
            session = {
                "session_id":   ACTIVE_SESSION["session_id"],
                "current_turn": ACTIVE_SESSION["current_turn"],
                "total_turns":  len(ACTIVE_SESSION["script"]),
                "bots":         ACTIVE_SESSION["bots"],
            }
    return {"bots": registry, "active_session": session}


# -----------------------------------------------------------------------------
if __name__ == "__main__":
    log.info("Robimon companion on port %d", PORT)
    log.info("Ollama: %s | model: %s", OLLAMA_URL, DEFAULT_MODEL)
    log.info("Proximity feature: %d dialog scripts loaded", len(BOT_DIALOGS))
    app.run(host="0.0.0.0", port=PORT, threaded=True)
