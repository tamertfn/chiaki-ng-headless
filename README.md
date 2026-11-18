# 🎮 chiaki-ng-headless (`chiaki-cli stream`)

> **A lightweight, pure-C, zero-transcode headless streaming fork of [`chiaki-ng`](https://github.com/chiaki-ng/chiaki-ng) built for Kubernetes, Docker, and self-hosted WebRTC servers (MediaMTX).**

## 🎯 Why This Fork Exists

Standard `chiaki-ng` provides an awesome desktop/handheld experience, but streaming PlayStation 5 / PlayStation 4 gameplay to a self-hosted media server (such as **MediaMTX** for sub-second WebRTC browser viewing) traditionally required keeping a desktop PC or virtual X11 display (`Xvfb`) running with Qt6, SDL2, OpenGL, PulseAudio, and OBS Studio — decoding the console's H.264/HEVC + Opus stream only to re-encode it again.

**`chiaki-ng-headless`** eliminates the entire GUI and transcoding stack by extending `chiaki-cli` with a native C `stream` subcommand ([`cli/src/stream.c`](cli/src/stream.c)):

* **100% Zero-Transcode Passthrough (`0%` Decode/Encode):** Hooks directly into `libchiaki`'s `ChiakiVideoSampleCallback` (reconstructed H.264/HEVC Annex-B NAL units) and `ChiakiAudioSinkFrame` (raw 48 kHz Stereo Opus frames) and muxes them directly to RTSP or file containers via `libavformat` with monotonic wall-clock presentation timestamps (`PTS`).
* **Featherweight Footprint:** Runs in **`~7 MiB` RAM** and **`~3%` of a single ARM64 CPU core** while streaming `1080p @ 60 FPS`, with zero GPU/VPU hardware requirements and full unprivileged container support (`UID 10001`).
* **On-Demand TCP Trigger Daemon (`--listen-trigger`):** Sits idle (`0%` CPU) listening on a TCP port until triggered by MediaMTX's `runOnDemand` hook when a viewer opens the WebRTC stream, automatically wakes the PS5 from Rest Mode (`--wakeup`), and gracefully disconnects (`chiaki_session_stop`) when the last viewer leaves.
* **PS5 68-Slice 1080p & High-Bitrate Tuning:** Includes a `2 MB` Takion UDP receive buffer (`SO_RCVBUF`), a 256-packet reorder queue, disabled fake audio-sequence packet-loss throttling, and a bundled MediaMTX build with `MaxNALUsPerAccessUnit = 256` (preventing upstream MediaMTX's 50-NALU limit from dropping PS5's 68-slice 1080p frames).

---

## 🚀 Quick Start

### 1. Container Image (`linux/arm64`)

```bash
docker pull ghcr.io/tamertfn/chiaki-ng-headless:latest
```

### 2. CLI Usage (`chiaki-cli stream`)

```bash
chiaki-cli stream \
  --ps5 \
  --wakeup \
  --host 192.168.1.100 \
  --registkey "<8-CHAR-ASCII-OR-32-CHAR-HEX>" \
  --morning "<BASE64-OR-32-CHAR-HEX-RP-KEY>" \
  --resolution 1080p \
  --fps 60 \
  --bitrate 20000 \
  --codec h264 \
  --idr-interval 2000 \
  --output rtsp://127.0.0.1:8554/ps5
```

All credentials can also be passed via environment variables (`CHIAKI_HOST`, `CHIAKI_REGIST_KEY`, `CHIAKI_MORNING_KEY`, `CHIAKI_OUTPUT_URL`) for clean Kubernetes `Secret` injection.

### 3. On-Demand WebRTC Mode with MediaMTX

Start `chiaki-cli` in on-demand daemon mode:
```bash
chiaki-cli stream --ps5 --wakeup --listen-trigger 9090
```

Configure `mediamtx.yml` to trigger `chiaki-cli` only when a viewer opens the stream:
```yaml
paths:
  ps5:
    runOnDemand: nc chiaki-headless 9090 -e sleep 86400
    runOnDemandRestart: no
    runOnDemandStartTimeout: 45s
    runOnDemandCloseAfter: 30s
```

---

![chiaki-ng Logo](gui/res/chiaking-logo.svg)

# Upstream [chiaki-ng](https://chiaki-ng.github.io/chiaki-ng/)

An open source PlayStation remote play project serving as the next-generation of Chiaki with improvements and ongoing support now that the original Chiaki project is in maintenance mode only. [Click here to see the accompanying site for documentation, updates and more](https://chiaki-ng.github.io/chiaki-ng/).

## Discord
[chiaki-ng community Discord](https://discord.gg/tAMbRuwXDH)

## Official downloads
Only download chiaki-ng from these sources:

- [GitHub Releases](https://github.com/chiaki-ng/chiaki-ng/releases)
- [Flathub](https://flathub.org/apps/io.github.streetpea.Chiaki4deck)
- winget: `winget install --id=StreetPea.chiaki-ng -e`
- Homebrew: `brew install --cask streetpea/streetpea/chiaki-ng`

Other websites offering chiaki-ng downloads, such as chiaki-ng.com, are not run by this project, and we can't vouch for the files they serve.

## Disclaimer
This project is not endorsed or certified by Sony Interactive Entertainment LLC.

Chiaki is a Free and Open Source Software Client for PlayStation 4 and PlayStation 5 Remote Play
for Linux, FreeBSD, OpenBSD, Android, macOS, Windows, Nintendo Switch and potentially even more platforms.
