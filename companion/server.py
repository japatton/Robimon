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

    headers = {
        "Content-Type":         "audio/wav",
        "X-Robimon-Transcript": urllib.parse.quote(text[:200]),
        "X-Robimon-Response":   urllib.parse.quote(response[:500]),
    }
    return Response(wav, status=200, headers=headers)


@app.route("/health", methods=["GET"])
def health():
    return {"ok": True, "model": DEFAULT_MODEL, "ollama_url": OLLAMA_URL,
            "whisper_size": WHISPER_SIZE, "piper_model": PIPER_MODEL}


# -----------------------------------------------------------------------------
if __name__ == "__main__":
    log.info("Robimon companion on port %d", PORT)
    log.info("Ollama: %s | model: %s", OLLAMA_URL, DEFAULT_MODEL)
    app.run(host="0.0.0.0", port=PORT, threaded=True)
