# LiveCaptions Nemotron

[简体中文](README.zh-CN.md) · English

A privacy-focused Linux live-caption application powered by **NVIDIA Nemotron 3.5 ASR Streaming 0.6B**, with optional real-time translation through a local [Ollama](https://ollama.com/) model.

This is a modified version of [abb128/LiveCaptions](https://github.com/abb128/LiveCaptions). It keeps the original GTK/Libadwaita captioning experience while replacing the retired April-ASR/ONNX pipeline with the native streaming C API from [NVIDIA NeMo-Speech.cpp](https://github.com/NVIDIA/NeMo-Speech.cpp).

> [!WARNING]
> Speech recognition and machine translation can make mistakes, especially with names, numbers, accents, overlapping speakers, and noisy audio. Do not rely on the output for safety-critical, legal, medical, or financial decisions.

## Highlights

- True streaming multilingual ASR with Nemotron 3.5 ASR Streaming 0.6B.
- Captions microphone input or desktop audio through PulseAudio.
- Runs speech recognition locally; microphone audio is not uploaded.
- Distinct icons and tooltips for microphone and system-audio modes.
- Optional streaming translation through Ollama.
- Presets for **HY-MT2 1.8B** and **TranslateGemma 4B**, plus a custom model field.
- Latest-request-wins translation queue: stale requests are cancelled instead of building a backlog.
- Stable translation display: completed text remains visible while the live tail catches up.
- Separate font settings for source captions and translations.
- Caption width adapts to the current monitor instead of using a fixed maximum.
- History, filtering, confidence fading, always-on-top support, and D-Bus text streaming.
- Separate application ID and settings schema, so it can coexist with upstream Live Captions.

## Architecture

```text
Microphone / desktop audio
          │
          ▼
PulseAudio capture (16 kHz PCM)
          │
          ▼
NeMo-Speech.cpp ── Nemotron 3.5 Streaming 0.6B (GGUF)
          │
          ▼
CaptionResult → line/history/filter/D-Bus pipeline → GTK caption line
          │
          └── optional Ollama streaming request → translated caption line
```

The ASR path does not require Python, PyTorch, Ollama, ONNX Runtime, or an NVIDIA GPU. Ollama is needed only for translation.

## Supported environment

The automated installer currently targets:

- x86_64 Linux
- Ubuntu 24.04 / Zorin OS 18-class distributions
- GTK 4 and Libadwaita
- PulseAudio or PipeWire's PulseAudio compatibility layer
- CPU inference by default

Other distributions may work with equivalent development packages, but have not been validated.

## Quick install

```bash
git clone https://github.com/Jingyuan-Zheng/LiveCaptions-Nemotron.git
cd LiveCaptions-Nemotron
./scripts/install.sh
```

The installer:

1. installs build dependencies with `apt`;
2. installs the pinned NeMo-Speech.cpp CPU runtime under `~/.local/opt/livecaptions-nemotron`;
3. downloads and verifies the pinned Q8 GGUF model under `~/.cache/livecaptions-nemotron/models`;
4. builds this source tree with Meson/Ninja;
5. creates a user-local launcher and desktop entry.

The model is **not stored in this repository**. The first install downloads approximately 742 MB.

To skip the `apt` step when dependencies are already present:

```bash
LIVECAPTIONS_SKIP_DEPS=1 ./scripts/install.sh
```

## Run

Open **LiveCaptions** from the application menu, or run:

```bash
~/.local/bin/livecaptions-nemotron
```

Use the microphone button in the upper-left corner to switch between microphone and system-audio capture. The icon and tooltip show the active mode.

## Optional translation

Translation is performed through Ollama's local HTTP API. Install Ollama separately, then pull one of the supported models:

```bash
# Recommended default for lower CPU latency
ollama pull kaelri/hy-mt2:1.8b-q4_K_M

# Alternative
ollama pull translategemma:4b
```

Enable translation in the caption window or Preferences. Preferences also lets you choose the model, target language, and translation font.

The default endpoint is `http://127.0.0.1:11434`. Override it when launching:

```bash
LIVECAPTIONS_OLLAMA_URL=http://127.0.0.1:11434 \
  ~/.local/bin/livecaptions-nemotron
```

Pointing the endpoint at a remote server means recognized text will be sent to that server.

## Runtime tuning

The launcher defaults to CPU inference and one RNNT right-context step:

```bash
LIVECAPTIONS_NEMOTRON_GPU=-1
LIVECAPTIONS_NEMOTRON_RIGHT_CONTEXT=1
```

Increasing right context may improve stability at the cost of latency. Hardware acceleration is experimental and depends on the installed NeMo-Speech backend.

## Manual build

The build expects NeMo-Speech.cpp inside the chosen Meson prefix:

```text
<prefix>/nemo-speech/include
<prefix>/nemo-speech/lib
```

After installing the runtime there:

```bash
meson setup builddir --buildtype=release --prefix="$HOME/.local/opt/livecaptions-nemotron"
ninja -C builddir
ninja -C builddir install
```

At runtime, set `LIVECAPTIONS_NEMOTRON_MODEL` to the GGUF model path and expose the NeMo-Speech library directory through `LD_LIBRARY_PATH`. The automated installer generates a launcher with these values.

## Uninstall

```bash
./scripts/uninstall.sh
```

This removes the user-local application, launcher, and desktop entry. It intentionally keeps downloaded models and does not remove Ollama or Ollama models. To remove the cache separately:

```bash
rm -rf "$HOME/.cache/livecaptions-nemotron"
```

## Privacy and downloaded components

- Audio recognition is local.
- Translation is local when Ollama uses the default loopback address.
- The installer connects to Ubuntu repositories, GitHub, and Hugging Face to download dependencies and models.
- No model weights, user history, audio recordings, credentials, or machine-specific configuration are committed to this repository.

Pinned components:

| Component | Version / revision |
|---|---|
| Upstream LiveCaptions base | `93734270e69a9d5d8a349075604477681c308dfa` |
| NeMo-Speech.cpp | `0.1.0` |
| Nemotron model revision | `ea30d66debe3740a08b573244286791d423d6b3e` |
| Model SHA-256 | `3fc991d3badad7277c11030a7519832cddaf2057aafed6d4b25147e953a070b1` |

Review the licenses and usage terms of NeMo-Speech.cpp, Nemotron, Ollama, and the selected translation model before redistribution or deployment.

## Development status

This is a personal/community fork, not an official release from the original LiveCaptions project or NVIDIA. It is currently optimized for the maintainer's x86_64 Ubuntu/Zorin desktop. Bug reports and reproducible performance results are welcome.

## Credits and license

- Original application: [abb128/LiveCaptions](https://github.com/abb128/LiveCaptions)
- Streaming ASR runtime and model: NVIDIA NeMo-Speech.cpp and Nemotron
- Translation runtime: Ollama

The application source remains licensed under **GNU GPL v3**, matching the upstream project. See [COPYING](COPYING). Modified files are distributed without warranty.
