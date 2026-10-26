#!/usr/bin/env bash
set -euo pipefail

PREFIX="${LIVECAPTIONS_PREFIX:-$HOME/.local/opt/livecaptions-nemotron}"

command -v realpath >/dev/null || { echo "realpath is required." >&2; exit 1; }
RESOLVED_PREFIX="$(realpath -m -- "$PREFIX")"
case "$RESOLVED_PREFIX" in
  /|"$HOME"|"$HOME/.local"|"$HOME/.local/opt")
    echo "Refusing unsafe uninstall prefix: $RESOLVED_PREFIX" >&2
    exit 1
    ;;
esac

rm -rf "$RESOLVED_PREFIX"
rm -f "$HOME/.local/bin/livecaptions-nemotron"
rm -f "$HOME/.local/share/applications/net.sapples.LiveCaptions.Nemotron.desktop"
update-desktop-database "$HOME/.local/share/applications" >/dev/null 2>&1 || true

echo "LiveCaptions Nemotron was removed."
echo "The model cache and Ollama models were kept."
echo "Optional cache location: $HOME/.cache/livecaptions-nemotron"
