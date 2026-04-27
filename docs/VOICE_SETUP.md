# Voice setup

Robimon's voice mode (double-tap on the face) records audio, sends it to a
companion server you run on your local machine alongside Ollama, and plays
back the LLM's response.

The companion does three jobs in one HTTP request:

1. **Speech-to-text** with [`faster-whisper`](https://github.com/SYSTRAN/faster-whisper)
2. **LLM chat** with your local [Ollama](https://ollama.com) instance
3. **Text-to-speech** with [Piper](https://github.com/rhasspy/piper)

Audio in and out are both 16 kHz mono int16 WAV (Piper TTS may return 22.05 kHz;
the device resamples in software).

## Path A — Docker (recommended if your Ollama is already in Docker)

### A.1 Fresh setup with Ollama + companion bundled

From the `companion/` directory:

```bash
# Download a Piper voice (one-time, ~60 MB)
./scripts/download-voice.sh         # defaults to en_US-amy-medium

docker compose up -d                # builds companion + starts ollama
docker exec ollama ollama pull llama3.1:8b   # pull a chat model
```

That's it. Companion is on `http://<host>:8765`, Ollama on `:11434`,
both restart on boot.

### A.2 You already have an Ollama container

You have two options. Both assume you're running this from `companion/`:

**A.2.1 Same Docker network (preferred):**

```bash
# Find your existing Ollama's network. Usually it's the compose project's
# default bridge — something like "myproj_default".
docker inspect ollama --format '{{range $k,$v := .NetworkSettings.Networks}}{{$k}}{{end}}'

./scripts/download-voice.sh
docker build -t robimon-companion .
docker run -d --name robimon-companion \
    --network <ollama-network-name> \
    -p 8765:8765 \
    -e OLLAMA_URL=http://ollama:11434 \
    -v "$(pwd)/voices:/voices:ro" \
    robimon-companion
```

The `OLLAMA_URL=http://ollama:11434` works because Docker's DNS resolves the
container name when you're on the same network. Replace `ollama` with the
actual name of your Ollama container.

**A.2.2 Host gateway (Ollama exposes 11434 to the host):**

```bash
./scripts/download-voice.sh
docker build -t robimon-companion .
docker run -d --name robimon-companion \
    -p 8765:8765 \
    -e OLLAMA_URL=http://host.docker.internal:11434 \
    --add-host host.docker.internal:host-gateway \
    -v "$(pwd)/voices:/voices:ro" \
    robimon-companion
```

The `--add-host` line is needed on Linux Docker; Docker Desktop on Mac/Windows
has `host.docker.internal` built in.

### A.2.3 Portainer (managing the stack via Portainer)

Portainer's Web editor can't bundle build context, so you have a few options:

**Option 1 — Repository deployment (uses `build:` from compose):** push your
Robimon copy to a Git repo Portainer can reach, then in Portainer →
Stacks → Add → Repository, set the compose path to
`companion/docker-compose.yml`. Voice files: SSH to the Portainer host,
`cd` into the cloned stack directory (path is shown in Portainer's stack
details), and run `companion/scripts/download-voice.sh`.

**Option 2 — Web editor + pre-built image + named volume:** use
`companion/docker-compose.portainer.yml`. The header of that file walks
through building the image, pushing to a registry (or `docker save`+`load`
if you don't have one), and populating the `companion-voices` named
volume with a throwaway helper container. Once that's done it's just
"paste compose, deploy" in Portainer.

**Option 3 — You already have an Ollama stack in Portainer.** Two sub-paths:

- **3a. Add the companion service to your existing stack.** Open the stack
  in Portainer → Editor, append the companion service block from
  `companion/docker-compose.portainer.yml` (the `companion:` section plus
  the named volume), and Update. Same internal Docker network → companion
  reaches Ollama by service name. Pre-build the image first as in Option 2.

- **3b. Companion as its own separate stack joined to the Ollama network.**
  Doesn't touch your existing stack. Use
  `companion/docker-compose.add-on.yml` — its header has the steps for
  finding your Ollama network name, building/loading the image, and
  populating the voices volume.

### A.3 Add companion to your existing docker-compose.yml

Drop this service into your compose file alongside `ollama`:

```yaml
services:
  # ... your existing ollama: service ...

  companion:
    build: ./Robimon/companion          # path to this directory
    ports: ["8765:8765"]
    depends_on: [ollama]
    environment:
      OLLAMA_URL: http://ollama:11434
      ROBIMON_MODEL: llama3.1:8b
      PIPER_MODEL: /voices/en_US-amy-medium.onnx
    volumes:
      - ./Robimon/companion/voices:/voices:ro
    restart: unless-stopped
```

Then `docker compose up -d companion`.

## Path B — Native Python (no Docker)

### B.1 Install Ollama and pull a model

```bash
ollama pull llama3.1:8b
```

### B.2 Install Python dependencies

```bash
cd companion
pip install -r requirements.txt
```

### B.3 Download a Piper voice

```bash
./scripts/download-voice.sh         # defaults to en_US-amy-medium
```

### B.4 Run the server

```bash
python server.py
```

Defaults: port `8765`, Ollama at `http://localhost:11434`, model `llama3.1:8b`.
Override via env vars (see the docstring at the top of `server.py`).

## Verify (any path)

From any machine on the same LAN:

```bash
curl http://<host>:8765/health
# {"ok":true,"model":"llama3.1:8b","ollama_url":"http://...", ...}
```

## On the device

Settings → **voice**. Three fields:

- **URL** — full URL of the companion's `/chat` endpoint, e.g.
  `http://192.168.1.50:8765/chat`. (HTTPS is supported but the device doesn't
  validate certificates yet — keep it on your LAN.)
- **Model** — Ollama model name. Defaults to `llama3.1:8b` if blank.
- **System prompt** — overrides the companion's default prompt. Leave blank to
  use the kid-friendly one shipped with the companion.

Tap **test** to verify the device can reach `/health`. Save and back out.

## Using it

Double-tap on the face screen. The face transitions to **listening** while
audio is being captured (5 s for v1; VAD auto-end coming later), then to
**thinking** while the LLM is generating, then to **speaking** while the TTS
plays back, then back to neutral.

If the network is unreachable or the companion errors, the face shows
**confused** briefly and returns to neutral. You'll never see a stack trace
on the kid-facing screen — diagnostics live in Settings → about (and on the
serial console with `status`).

## Tuning

- **Smaller Whisper** for faster STT: `WHISPER_SIZE=tiny.en`
  (slightly worse accuracy, ~3× faster on CPU).
- **GPU Whisper**: `WHISPER_DEVICE=cuda WHISPER_COMPUTE=float16` if you have
  a CUDA box.
- **Lower-latency TTS**: try a `low` Piper voice (e.g. `en_US-lessac-low`).
- **Different Ollama model**: pass `X-Robimon-Model` header by setting it in
  the device's voice settings.

## Protocol reference

If you want to write your own companion that's compatible with the device,
the protocol is documented at the top of `companion/server.py`. The contract
is intentionally tiny: one POST in, one WAV out, two response headers for
diagnostics.

## Child-safety note

The default system prompt steers the LLM toward short, warm, age-appropriate
responses and asks it to redirect inappropriate questions. It is **not** a
content-safety guarantee — local models are generally well-behaved with this
prompt, but if you want stricter filtering you can also wrap the response in a
classifier on the companion side before sending it to TTS.
