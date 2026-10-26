#!/usr/bin/env bash
set -euo pipefail

NEMO_VERSION="0.1.0"
PREFIX="${LIVECAPTIONS_PREFIX:-$HOME/.local/opt/livecaptions-nemotron}"
CACHE_DIR="${LIVECAPTIONS_CACHE_DIR:-$HOME/.cache/livecaptions-nemotron}"
NEMO_PREFIX="$PREFIX/nemo-speech"
MODEL_DIR="$CACHE_DIR/models"
MODEL_REVISION="ea30d66debe3740a08b573244286791d423d6b3e"
MODEL_NAME="nemotron-3.5-asr-streaming-0.6b.q8_0.gguf"
MODEL_SHA256="3fc991d3badad7277c11030a7519832cddaf2057aafed6d4b25147e953a070b1"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$SOURCE_DIR/builddir"

validate_prefix() {
  local resolved
  resolved="$(realpath -m -- "$PREFIX")"
  case "$resolved" in
    /|"$HOME"|"$HOME/.local"|"$HOME/.local/opt")
      echo "Refusing unsafe installation prefix: $resolved" >&2
      exit 1
      ;;
  esac
}

if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
  echo "This installer currently supports x86_64 Linux only." >&2
  exit 1
fi

command -v curl >/dev/null || { echo "curl is required." >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 is required." >&2; exit 1; }
command -v realpath >/dev/null || { echo "realpath is required." >&2; exit 1; }
validate_prefix

if [[ "${LIVECAPTIONS_SKIP_DEPS:-0}" != "1" ]]; then
  command -v sudo >/dev/null || { echo "sudo is required to install dependencies." >&2; exit 1; }
  echo "Installing build and runtime dependencies..."
  sudo apt-get update
  sudo apt-get install -y \
    build-essential curl ca-certificates pkg-config meson ninja-build \
    libgtk-4-dev libadwaita-1-dev libpulse-dev libx11-dev \
    libsoup-3.0-dev libjson-glib-dev desktop-file-utils
else
  echo "Skipping dependency installation (LIVECAPTIONS_SKIP_DEPS=1)."
fi

mkdir -p "$CACHE_DIR" "$MODEL_DIR" "$HOME/.local/bin" "$HOME/.local/share/applications"

echo "Installing NeMo-Speech.cpp $NEMO_VERSION CPU runtime..."
curl -fsSL --retry 3 \
  "https://raw.githubusercontent.com/NVIDIA/NeMo-Speech.cpp/v${NEMO_VERSION}/scripts/install.sh" \
  -o "$CACHE_DIR/nemo-install.sh"
chmod +x "$CACHE_DIR/nemo-install.sh"
"$CACHE_DIR/nemo-install.sh" \
  --version "$NEMO_VERSION" \
  --prefix "$NEMO_PREFIX" \
  --backend cpu \
  --no-modify-path

if [[ ! -x "$NEMO_PREFIX/bin/nemo-speech" ]]; then
  echo "NeMo-Speech installation failed: executable not found." >&2
  exit 1
fi

MODEL_PATH="$MODEL_DIR/$MODEL_NAME"
MODEL_URL="https://huggingface.co/nvidia/nemotron-3.5-asr-streaming-0.6b/resolve/$MODEL_REVISION/$MODEL_NAME?download=true"

verify_model() {
  [[ -f "$MODEL_PATH" ]] && echo "$MODEL_SHA256  $MODEL_PATH" | sha256sum -c - >/dev/null 2>&1
}

if verify_model; then
  echo "Using verified cached model: $MODEL_PATH"
else
  echo "Downloading Nemotron model (approximately 742 MB)..."
  rm -f "$MODEL_PATH.part"
  curl -fL --retry 3 --retry-delay 2 "$MODEL_URL" -o "$MODEL_PATH.part"
  echo "$MODEL_SHA256  $MODEL_PATH.part" | sha256sum -c -
  mv "$MODEL_PATH.part" "$MODEL_PATH"
fi

# Some prebuilt NeMo-Speech packages bundle an older libstdc++. Disable it so
# system build tools and GTK resolve against the distribution runtime.
DISABLED_RUNTIME_DIR="$NEMO_PREFIX/lib/bundled-runtime-disabled"
mkdir -p "$DISABLED_RUNTIME_DIR"
shopt -s nullglob
for bundled_cxx in "$NEMO_PREFIX"/lib/libstdc++.so*; do
  mv -f "$bundled_cxx" "$DISABLED_RUNTIME_DIR/"
done
shopt -u nullglob

NEMO_C_LIB="$(find "$NEMO_PREFIX/lib" -maxdepth 1 \( -type f -o -type l \) -name 'libnemo_speech_asr_c.so*' -print -quit)"
if [[ -n "$NEMO_C_LIB" ]] && ldd "$NEMO_C_LIB" 2>/dev/null | grep -q 'not found'; then
  echo "NeMo-Speech has unresolved runtime libraries:" >&2
  ldd "$NEMO_C_LIB" >&2 || true
  exit 1
fi

echo "Building LiveCaptions Nemotron..."
rm -rf "$BUILD_DIR"
BUILD_PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
env -u LD_LIBRARY_PATH PATH="$BUILD_PATH" \
  meson setup "$BUILD_DIR" "$SOURCE_DIR" --buildtype=release --prefix="$PREFIX"
env -u LD_LIBRARY_PATH PATH="$BUILD_PATH" ninja -C "$BUILD_DIR"
env -u LD_LIBRARY_PATH PATH="$BUILD_PATH" ninja -C "$BUILD_DIR" install

cat > "$HOME/.local/bin/livecaptions-nemotron" <<EOF
#!/usr/bin/env bash
set -e
PREFIX="$PREFIX"
export GSETTINGS_SCHEMA_DIR="\$PREFIX/share/glib-2.0/schemas"
export XDG_DATA_DIRS="\$PREFIX/share:\${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export LD_LIBRARY_PATH="$NEMO_PREFIX/lib:\${LD_LIBRARY_PATH:-}"
export LIVECAPTIONS_NEMOTRON_MODEL="$MODEL_PATH"
export LIVECAPTIONS_NEMOTRON_GPU="\${LIVECAPTIONS_NEMOTRON_GPU:--1}"
export LIVECAPTIONS_NEMOTRON_RIGHT_CONTEXT="\${LIVECAPTIONS_NEMOTRON_RIGHT_CONTEXT:-1}"
export LIVECAPTIONS_OLLAMA_URL="\${LIVECAPTIONS_OLLAMA_URL:-http://127.0.0.1:11434}"
exec "\$PREFIX/bin/livecaptions" "\$@"
EOF
chmod +x "$HOME/.local/bin/livecaptions-nemotron"

cat > "$HOME/.local/share/applications/net.sapples.LiveCaptions.Nemotron.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=LiveCaptions
Comment=Local multilingual captions with optional translation
Icon=audio-input-microphone-symbolic
Exec=$HOME/.local/bin/livecaptions-nemotron
Terminal=false
Categories=Accessibility;GTK;Utility;
Keywords=Captions;Subtitles;Nemotron;Translation;Ollama;Speech;
StartupNotify=true
EOF

update-desktop-database "$HOME/.local/share/applications" >/dev/null 2>&1 || true

echo
echo "Installed successfully."
echo "Launch from the application menu as LiveCaptions, or run:"
echo "  $HOME/.local/bin/livecaptions-nemotron"
echo
echo "Optional translation models:"
echo "  ollama pull kaelri/hy-mt2:1.8b-q4_K_M"
echo "  ollama pull translategemma:4b"
