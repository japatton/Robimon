#!/usr/bin/env bash
# Download a default Piper voice (en_US-amy-medium) into ./voices/.
# Run this once before starting the companion.
set -euo pipefail

VOICE="${1:-en_US-amy-medium}"
BASE="https://huggingface.co/rhasspy/piper-voices/resolve/main"

# Voice files live under en/<lang>/<name>/<quality>/...
# Parse our compact name into URL parts.
LANG_REGION="${VOICE%%-*}"          # e.g. "en_US"
REST="${VOICE#*-}"                  # e.g. "amy-medium"
NAME="${REST%-*}"                   # "amy"
QUALITY="${REST##*-}"               # "medium"
LANG_PARENT="${LANG_REGION%%_*}"    # "en"

DEST_DIR="$(dirname "$0")/../voices"
mkdir -p "$DEST_DIR"

for ext in onnx onnx.json; do
  URL="$BASE/$LANG_PARENT/$LANG_REGION/$NAME/$QUALITY/$VOICE.$ext"
  OUT="$DEST_DIR/$VOICE.$ext"
  if [[ -f "$OUT" ]]; then
    echo "exists: $OUT"
    continue
  fi
  echo "downloading: $URL"
  curl -L --fail -o "$OUT" "$URL"
done

echo "done. PIPER_MODEL=$DEST_DIR/$VOICE.onnx"
