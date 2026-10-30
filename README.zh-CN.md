# LiveCaptions Nemotron

简体中文 · [English](README.md)

这是一个注重隐私的 Linux 实时字幕应用：使用 **NVIDIA Nemotron 3.5 ASR Streaming 0.6B** 在本机进行流式语音识别，并可通过本机 [Ollama](https://ollama.com/) 模型实时翻译。

项目基于 [abb128/LiveCaptions](https://github.com/abb128/LiveCaptions) 修改，保留原有 GTK/Libadwaita 字幕体验，并以 [NVIDIA NeMo-Speech.cpp](https://github.com/NVIDIA/NeMo-Speech.cpp) 原生流式 C API 替换原 April-ASR/ONNX 推理链。

<p align="center">
  <img src="screenshot.png" alt="LiveCaptions 字幕窗口演示" width="836">
</p>

<p align="center"><em>悬浮在桌面内容之上的紧凑实时字幕窗口。</em></p>

> [!WARNING]
> 语音识别和机器翻译都可能出错，姓名、数字、口音、多人同时说话和嘈杂环境尤其如此。请勿将结果用于安全、法律、医疗或财务等关键决策。

## 主要功能

- Nemotron 3.5 ASR Streaming 0.6B 多语言真流式识别。
- 支持麦克风与桌面音频字幕，左上角图标和提示会显示当前输入模式。
- 语音识别完全在本机运行，不上传麦克风音频。
- 可选 Ollama 流式翻译，内置 HY-MT2 1.8B、TranslateGemma 4B 预设和自定义模型字段。
- 翻译采用 latest-wins 队列：新文本到来时取消过期请求，避免延迟不断累积。
- 已完成译文持续显示，实时尾段追赶时不会反复清空或闪烁。
- 原文和译文可分别设置字体。
- 字幕最大宽度根据当前屏幕动态计算，不写死像素上限。
- 保留历史、敏感词过滤、置信度淡化、窗口置顶和 D-Bus 文本流功能。
- 使用独立 Application ID 与 GSettings schema，可与上游 Live Captions 共存。

## 数据流程

```text
麦克风 / 桌面音频
        │
        ▼
PulseAudio 采集（16 kHz PCM）
        │
        ▼
NeMo-Speech.cpp ── Nemotron 3.5 Streaming 0.6B（GGUF）
        │
        ▼
CaptionResult → 断行/历史/过滤/D-Bus → GTK 原文字幕
        │
        └── 可选 Ollama 流式请求 → 译文字幕
```

识别部分不需要 Python、PyTorch、Ollama、ONNX Runtime 或 NVIDIA GPU；只有启用翻译时才需要 Ollama。

## 支持环境

自动安装器目前面向：

- x86_64 Linux；
- Ubuntu 24.04 / Zorin OS 18 同类系统；
- GTK 4 与 Libadwaita；
- PulseAudio，或 PipeWire 的 PulseAudio 兼容层；
- 默认使用 CPU 推理。

其他发行版安装等价依赖后也可能工作，但尚未验证。

## 快速安装

```bash
git clone https://github.com/Jingyuan-Zheng/LiveCaptions-Nemotron.git
cd LiveCaptions-Nemotron
./scripts/install.sh
```

安装脚本会：

1. 通过 `apt` 安装构建依赖；
2. 将固定版本的 NeMo-Speech.cpp CPU runtime 安装到 `~/.local/opt/livecaptions-nemotron`；
3. 下载并校验 Nemotron Q8 GGUF 模型，保存到 `~/.cache/livecaptions-nemotron/models`；
4. 使用 Meson/Ninja 编译当前仓库源码；
5. 创建用户级启动脚本和桌面菜单项。

模型权重**不包含在 GitHub 仓库中**，首次安装约需下载 742 MB。

已具备依赖时可跳过 `apt`：

```bash
LIVECAPTIONS_SKIP_DEPS=1 ./scripts/install.sh
```

## 启动与使用

从应用菜单打开 **LiveCaptions**，或执行：

```bash
~/.local/bin/livecaptions-nemotron
```

点击左上角麦克风按钮可在麦克风和系统音频之间切换；按钮图标和悬浮提示会显示当前模式。

## 可选翻译

翻译通过 Ollama 本地 HTTP API 完成。请单独安装 Ollama，然后选择一个模型：

```bash
# 默认推荐，CPU 延迟较低
ollama pull kaelri/hy-mt2:1.8b-q4_K_M

# 可选
ollama pull translategemma:4b
```

在字幕窗口或设置中开启翻译；设置页面还可以选择模型、目标语言和译文字体。

默认 Ollama 地址为 `http://127.0.0.1:11434`。可在启动时覆盖：

```bash
LIVECAPTIONS_OLLAMA_URL=http://127.0.0.1:11434 \
  ~/.local/bin/livecaptions-nemotron
```

如果将地址改为远程服务器，识别文本将被发送到该服务器。

## 性能参数

默认强制 CPU 推理，并使用一个 RNNT right-context step：

```bash
LIVECAPTIONS_NEMOTRON_GPU=-1
LIVECAPTIONS_NEMOTRON_RIGHT_CONTEXT=1
```

提高 right context 可能增强识别稳定性，但会增加延迟。硬件加速属于实验功能，并取决于安装的 NeMo-Speech backend。

## 卸载

```bash
./scripts/uninstall.sh
```

脚本会移除用户级应用、启动器和菜单项，不会删除模型缓存、Ollama 或 Ollama 模型。如需另外删除缓存：

```bash
rm -rf "$HOME/.cache/livecaptions-nemotron"
```

## 隐私与下载内容

- 语音识别在本机完成。
- Ollama 使用默认回环地址时，翻译也在本机完成。
- 安装期间会连接 Ubuntu 软件源、GitHub 和 Hugging Face 下载依赖与模型。
- 仓库不包含模型权重、用户历史、录音、凭据或机器专属配置。

固定组件：

| 组件 | 版本 / revision |
|---|---|
| LiveCaptions 上游基线 | `93734270e69a9d5d8a349075604477681c308dfa` |
| NeMo-Speech.cpp | `0.1.0` |
| Nemotron 模型 revision | `ea30d66debe3740a08b573244286791d423d6b3e` |
| 模型 SHA-256 | `3fc991d3badad7277c11030a7519832cddaf2057aafed6d4b25147e953a070b1` |

重新分发或部署前，请分别查看 NeMo-Speech.cpp、Nemotron、Ollama 和所选翻译模型的许可证与使用条款。

## 项目定位与许可证

这是个人/社区维护的分支，不是 LiveCaptions 原项目或 NVIDIA 的官方发布。目前主要针对维护者的 x86_64 Ubuntu/Zorin 桌面优化。

- 原项目：[abb128/LiveCaptions](https://github.com/abb128/LiveCaptions)
- 流式识别 runtime 与模型：NVIDIA NeMo-Speech.cpp / Nemotron
- 翻译 runtime：Ollama

应用源码继续使用上游的 **GNU GPL v3** 许可证，详见 [COPYING](COPYING)。项目不提供任何担保。
