# Robimon companion

Runs alongside [Ollama](https://ollama.com) on a local machine (any
Mac/Linux/Windows box on the same LAN as the device). Provides STT
(Whisper) + LLM proxy (Ollama) + TTS (Piper) over a single HTTP endpoint.

See [`../docs/VOICE_SETUP.md`](../docs/VOICE_SETUP.md) for full setup —
covers Docker (with or without an existing Ollama container) and native
Python options.

## Quick reference

```bash
# Docker (fresh setup, includes Ollama):
./scripts/download-voice.sh
docker compose up -d
docker exec ollama ollama pull llama3.1:8b

# Docker (you already have an Ollama container):
./scripts/download-voice.sh
docker build -t robimon-companion .
docker run -d --name robimon-companion \
    --network <your-ollama-network> \
    -p 8765:8765 \
    -e OLLAMA_URL=http://ollama:11434 \
    -v "$(pwd)/voices:/voices:ro" \
    robimon-companion

# Native Python:
pip install -r requirements.txt
./scripts/download-voice.sh
python server.py
```

## Files

- `server.py` — the HTTP server (full protocol docs at the top).
- `requirements.txt` — Python deps.
- `Dockerfile` — Python 3.12 slim base + Whisper precache.
- `docker-compose.yml` — companion + Ollama bundled (host bind-mount voices).
- `docker-compose.portainer.yml` — pre-built image + named volume variant
  for Portainer Web-editor stacks; header explains the workflow.
- `scripts/download-voice.sh` — fetches a Piper voice into `voices/`.
- `voices/` — Piper voice files (created by the download script;
  bind-mounted into the container in the standard compose).

