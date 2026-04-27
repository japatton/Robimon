# Robimon companion — Whisper STT + Ollama proxy + Piper TTS in one HTTP server.
#
# Build:
#   docker build -t robimon-companion .
#
# Run alongside your existing Ollama container — see docker-compose.yml or
# docs/VOICE_SETUP.md for the recommended setups.

FROM python:3.12-slim

# libsndfile for ctranslate2 (Whisper backend); ca-certs for HTTPS to Ollama
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsndfile1 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Pre-cache the Whisper model so first request after cold start is fast.
ARG WHISPER_PRECACHE=base.en
RUN python -c "from faster_whisper import WhisperModel; WhisperModel('${WHISPER_PRECACHE}', device='cpu', compute_type='int8')"

COPY server.py .

# Mount your Piper voice files at /voices and set PIPER_MODEL accordingly,
# OR copy a voice file into the image at build time.
ENV PIPER_MODEL=/voices/en_US-amy-medium.onnx
ENV ROBIMON_PORT=8765

EXPOSE 8765

CMD ["python", "server.py"]
