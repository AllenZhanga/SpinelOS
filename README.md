<div align="center">

# Spinel OS

**A lightweight Wayland desktop OS built without KDE, GNOME, or XFCE**

Based on **Arch Linux (`archiso`)** · **Custom C++20 Desktop Shell (`spinel-shell`)** · **AI-Native OS Layer (`spinel-os-mcp` & Sandbox)** · **Steam / Gamescope / MangoHud Ready**

[![Arch Linux Base](https://img.shields.io/badge/Base-Arch%20Linux-1793D1?logo=archlinux&logoColor=white)](https://archlinux.org/)
[![Wayland Compositor](https://img.shields.io/badge/Wayland-labwc%20%2B%20GTK4%20Layer--Shell-38BDF8)](https://labwc.github.io/)
[![C++20](https://img.shields.io/badge/Desktop%20Shell-C%2B%2B20%20%2B%20WebKitGTK%206-6366F1)](desktop/)
[![License: MIT](https://img.shields.io/badge/License-MIT-10B981.svg)](LICENSE)
[![GitHub Issues](https://img.shields.io/github/issues/AllenZhanga/SpinelOS)](https://github.com/AllenZhanga/SpinelOS/issues)

**English** · [简体中文](README.zh-CN.md)

[Features](#core-features) · [Architecture](#why-not-a-traditional-de) · [AI OS Layer](#3-ai-native-os-infrastructure--parallel-sandbox) · [Quick Start](#quick-start--installation) · [Build ISO](#build-iso-from-source)

</div>

---

> **Screenshots (Coming Soon)**  
> Place real hardware / GPU-passthrough captures under `docs/assets/` and uncomment below:
> <!-- ![SpinelOS Desktop](docs/assets/desktop-light.png) -->
> <!-- ![SpinelOS Gamescope & Control Center](docs/assets/games-center.png) -->

> [!NOTE]
> **Project Status (Alpha Preview)**: Spinel OS currently provides source builds, Live ISOs, and cloud OTA updates for desktop developers, daily Linux users, and PC/handheld gamers. Hardware compatibility reports (AMD / Intel / NVIDIA) and feedback are welcome in [Issues](https://github.com/AllenZhanga/SpinelOS/issues).

---

## What is Spinel OS?

Most Arch derivatives simply bundle themes or pre-installed packages on top of KDE or GNOME. **Spinel OS is built to be leaner, cohesive, and controllable end-to-end**:

- **Independent C++20 Shell with Dual-Process Low-Memory Design**: Instead of pulling in a full KDE/GNOME/XFCE stack, Spinel OS runs a custom native Wayland shell [`spinel-shell`](desktop/) (`GTK4 + Cairo + gtk4-layer-shell`, ~**75 MB PSS** resident) for the global top bar, Dock, desktop canvas, Control Center, Spotlight launcher, and AI Dynamic Island. Heavy windows (`Settings`, `App Center`, `Games Center`, `System Monitor`, `Spinel Doctor`, `Installer`) spawn on demand via `spinel-webui` (`WebKitGTK 6.0`) and free all memory on close, backed by lightweight `labwc` (`wlroots`) for Wayland/XWayland compositing.
- **First-Party Native Core Apps**: Includes [`spinel-files`](desktop/) (GTK4 virtualized file manager with `org.freedesktop.FileManager1` D-Bus integration and built-in archive compression/extraction), [`spinel-terminal`](desktop/) (`GTK4 + VTE4` multi-tabbed terminal with theme sync), and [`spinel-sandbox`](desktop/) (isolated parallel sandbox desktop with Picture-in-Picture takeover), alongside custom GRUB and Plymouth boot animations.
- **Built for Office + AI Agent Workflows + Gaming**: Designed as a unified daily workstation combining desktop multitasking, first-class OS integration for coding agents via MCP, out-of-the-box CJK input and proxy routing, and pre-configured GPU + Gamescope gaming stacks.

---

## Core Features

### 1. Custom Spinel Shell Desktop Experience
- **Dynamic Global Menu Bar, System Tray & Workspaces**: Context-aware top bar with active-window menus, native `StatusNotifierItem` (`SNI` + `DBusMenu`) tray support, `wl-clip-persist` clipboard history (10 entries), Bento Control Center, and multi-workspace switching (`Super + 1~4` / top-bar indicator).
- **Spotlight Global Launcher (`Alt + Space`)**: Instant fuzzy search across installed desktop apps, local Steam games, Windows apps, Settings deep-links, and quick system actions.
- **Floating Dock, Desktop Icons & Theme Engine**: Drag-to-reorder Dock, context menus, unified visual design tokens ([docs/UI_SPEC.md](docs/UI_SPEC.md)), built-in themes (default `tahoe-light` liquid glass), custom local wallpapers, native screenshot (`spinel-screenshot`), and lock screen (`spinel-lock`).

### 2. First-Party Apps & Daily Productivity
- **Native File Manager (`spinel-files`)**: Fast `GListStore` virtualized list/grid views, asynchronous directory scanning, D-Bus `FileManager1` reveal-in-folder support, and native archive extract/compress actions.
- **Multi-Tab Terminal (`spinel-terminal`)**: Built on `GTK4 + VTE4` (`Super + Enter` / `Ctrl + T` inheriting working directory / `Ctrl + Shift + C/V` / `Ctrl + F`), synced live with active desktop themes.
- **Dedicated System & Utility Apps (`spinel-webui`)**:
  - **App Center**: Host-native software installation via `pacman` and native packages (browsers, office suites, LibreOffice, PDF/note tools, VS Code, containers, local LLM runtimes).
  - **System Monitor & Spinel Doctor**: Independent `PSS`-accurate process monitor, categorized system/service/AI audit logs (`?mode=monitor`), and one-click health diagnostics (`?mode=doctor`).
  - **Granular Settings**: Network/Wi-Fi/VPN & Bluetooth, Display & scaling, Audio & Power, interactive global shortcut recording (synced to `labwc`), Btrfs `snapper` snapshots, and categorized Security & Permissions.

### 3. AI-Native OS Infrastructure & Parallel Sandbox
> Full specification and CLI usage: [docs/AI_OS_GUIDE.md](docs/AI_OS_GUIDE.md) · [docs/AI_OS_PLAN.md](docs/AI_OS_PLAN.md)
- **Top-Bar AI Dynamic Island**: Automatically aggregates live coding-agent sessions (e.g., Antigravity, Codex, Claude Code, `spinel-os-mcp` tool calls, or `/proc` probes), supports instant workspace/window jumping or sandbox PiP takeover, and surfaces **Tier 3 interactive approval cards** (`Allow`, `Snapshot + Allow`, `Run in Sandbox`, `Deny`).
- **`spinel-os-mcp` Gateway (`stdio`) & One-Click Setup**: Exposes 18 OS capabilities (desktop context, screenshots, clipboard, Doctor diagnostics, sandbox launch, Btrfs checkpoints, service control) guarded by `Tier 0–3` permissions and denoised audit logging. Configure **Antigravity** (`~/.gemini/config/mcp_config.json`), **Codex**, **Claude Code**, **DeepSeekHarness**, or **Gemini CLI** non-destructively via **Settings → Permissions & Security** or `spinel-agent-ctl mcp-install <client>`.
- **Parallel Sandbox (`spinel-sandbox`) & Cloud Relay**: Isolated sandboxed desktop sessions with PiP preview, network/clipboard toggles, plus optional `spinel-relay` ([server/spinel-relay](server/spinel-relay)) for remote session continuity and cloud OTA distribution.

### 4. Ready-to-Play PC & Handheld Gaming
- **Pre-installed GPU Drivers** (see [archiso/packages.custom.x86_64](archiso/packages.custom.x86_64)):
  - **AMD**: `mesa` + `vulkan-radeon` (including 32-bit `lib32-*`)
  - **Intel**: `mesa` + `vulkan-intel` (including 32-bit `lib32-*`)
  - **NVIDIA**: `nvidia-open` + `nvidia-utils` + `lib32-nvidia-utils` (GTX 16 & RTX 20/30/40/50 series)
- **Compliance-Friendly Gaming Stack**: Pre-configured `[multilib]`, `gamescope`, `mangohud`, `gamemode`, PipeWire audio, and Bluetooth controller support. Proprietary `Steam` is not bundled into the ISO; clicking Steam or **Games Center** triggers one-click online installation from the App Center (every ISO build also emits `spinelos-packages-licenses.tsv` and `NOTICE-THIRD-PARTY.txt`).
- **Games Center & Windows `.exe` Support**: Automatically discovers local Steam libraries and artwork, launches into **Steam** or **Gamescope Big Picture**, configures per-game MangoHud/GameMode/upscaling toggles, and supports on-demand Wine execution (`spinel-wine`, prefix at `~/.local/share/spinelos/wine-prefix/drive_c`).

### 5. Out-of-the-Box CJK & System Proxy Routing
- **Fcitx5 Wayland IME**: Pre-configured Pinyin (`fcitx5-chinese-addons`), optional **Rime-Ice**, `Noto Color Emoji` fallback, and matching 9-patch frosted light/dark skins.
- **Unified Proxy Sync**: HTTP / SOCKS proxy settings (or external `org.gnome.system.proxy` tools like Clash/Monocloud) automatically propagate across Chromium browsers and Steam.

---

## Why Not a Traditional DE?

| Area | Traditional Desktops (KDE / GNOME / XFCE) | **Spinel OS (`Spinel Shell + labwc`)** |
| :--- | :--- | :--- |
| **Desktop Stack** | Heavy suites with many background daemons | Custom C++20 `spinel-shell` (`GTK4 + Cairo`) + on-demand `spinel-webui` + `labwc` |
| **Visual Consistency** | Mixed GTK3 / GTK4 / Qt toolkits and fragmented settings | Unified [UI_SPEC.md](docs/UI_SPEC.md) tokens across top bar, Dock, apps, walls & terminal |
| **CJK & Proxy Readiness** | Often requires manual `fcitx5` env vars and per-app proxy setup | Pre-wired Wayland IME, Portals (`SpinelOS:GNOME`), and GSettings proxy inheritance |
| **AI & Isolation Layer** | Ad-hoc scripts without OS-level approval or sandbox PiP | Native Dynamic Island, `spinel-os-mcp` (`Tier 0–3`), Btrfs checkpoints & `spinel-sandbox` |
| **Hackability** | Maintaining patches/extensions across multiple upstream repos | Complete desktop stack lives in [desktop/](desktop/) with sub-second hot-deploy |

---

## Quick Start & Installation

### Hardware / VM Requirements
- **Architecture**: `x86_64` (UEFI boot; 3D acceleration recommended, software rendering fallback included for VMs)
- **Memory**: 8 GB+ recommended (4 GB minimum)
- **GPU**: AMD Radeon / Intel UHD & Arc / NVIDIA GTX 16 & RTX 20–50 series

### Getting Started
1. Download the latest ISO from [Releases](https://github.com/AllenZhanga/SpinelOS/releases) (or build `out/spinelos-*.iso` from source below).
2. **Bare Metal**: Flash to USB via [Ventoy](https://www.ventoy.net/), `balenaEtcher`, or `dd`, and boot in **UEFI mode**.
3. **Virtual Machine**: Attach the ISO in QEMU / VMware / VirtualBox with UEFI enabled.
4. **Install to Disk**: Launch **Install Spinel OS** (`spinel-install`) from the Live desktop to configure target disk, Btrfs `@`/`@home`/`@snapshots` layout, hostname, user account, and dual-boot detection.

> [!IMPORTANT]
> **Live Environment Note**: The Live ISO auto-logs into demo user `gamer` with passwordless `sudo` for rapid evaluation and VM testing. Set a strong password for your system account when installing to disk.

---

## Build ISO from Source

On any `x86_64` Linux host with **Docker** installed (15 GB+ free disk space recommended):

```bash
chmod +x scripts/build-iso.sh docker/build-inside-container.sh
./scripts/build-iso.sh
```

The bootable image is written to `out/spinelos-*.iso`.

<details>
<summary>Expand: Developer Build, C++20 Unit Tests, Hot Deploy & Repository Layout</summary>

### Repository Layout

```text
.
├── desktop/                     # Native C++20 desktop stack + HTML/CSS/JS UI
│   ├── CMakeLists.txt           # Builds shell, webui, files, terminal, sandbox, agent-ctl, os-mcp, tests
│   ├── src/                     # Wayland layer-shell, IPC router, AI/MCP engine, sys controls
│   ├── tests/                   # spinel-tests (CTest spinel_unit_tests)
│   └── ui/                      # Frontend for Settings, App Center, Games, Monitor, Doctor, Installer
├── docs/                        # UI_SPEC.md, AI_OS_GUIDE.md, AI_OS_PLAN.md, OTA_RELEASE_GUIDE.md, ROADMAP.md
├── archiso/                     # ArchISO overlay
│   ├── packages.custom.x86_64   # Kernel, GPU drivers, Wayland, Gamescope, Fcitx5, Spinel dependencies
│   └── airootfs/                # Rootfs configs, labwc/portals, spinel-install, spinel-update
├── docker/                      # Containerized ISO & C++20 cross-build environment
├── server/spinel-relay/         # Go TLS relay server for remote sandbox streaming & OTA hosting
├── scripts/
│   ├── build-iso.sh             # Build bootable ISO
│   ├── dev-deploy.sh            # Hot-deploy C++ / UI / full system configs to a running VM
│   ├── release-ota.sh           # Sign and publish cloud OTA bundles
│   ├── build-relay.sh           # Deploy/upgrade spinel-relay server
│   └── sync-ecs.sh              # Sync sources to remote build host
└── .github/workflows/build-iso.yml
```

### Build & Run C++20 Unit Tests (via `spinelos-builder`)

```bash
docker run --rm --platform linux/amd64 -v "$PWD":/workspace -w /workspace spinelos-builder \
  bash -c "cmake -S desktop -B build/linux-amd64 -DCMAKE_BUILD_TYPE=Release && cmake --build build/linux-amd64 -j4 && ctest --test-dir build/linux-amd64 --output-on-failure -V"
```

### Hot Deploy & Cloud OTA

```bash
# Sync desktop UI only (~1s)
./scripts/dev-deploy.sh <user>@<VM_IP> --ui-only

# Sync compiled C++20 binaries + UI + D-Bus/MIME entries
./scripts/dev-deploy.sh <user>@<VM_IP>

# Full sync including packages, themes, labwc, portals, and boot scripts
./scripts/dev-deploy.sh <user>@<VM_IP> --full

# Pull latest published Cloud OTA directly on a Spinel OS machine
sudo spinel-update upgrade --full --yes
```

</details>

---

## Roadmap & ARM64 (`aarch64`) Exploration

- [x] Custom C++20 + GTK4 Layer-Shell + WebKitGTK 6.0 dual-process desktop (`spinel-shell`, `spinel-webui`, `spinel-terminal`, `spinel-files`)
- [x] Dedicated App Center (host-native `pacman`/packages), Games Center, System Monitor (`PSS` + categorized logs), and Spinel Doctor
- [x] AI OS infrastructure (`spinel-os-mcp`, Dynamic Island, `spinel-sandbox`, safe MCP auto-config, denoised audit logs) & Cloud OTA (`spinel-update`)
- [ ] Bare-metal GPU validation and installer storage/dual-boot refinements
- [ ] **ARM64 (`aarch64`) Research**: `desktop/` cross-compiles cleanly while upstream `archiso` targets `x86_64`; future exploration covers **Arch Linux ARM + FEX-Emu + Proton** with Vulkan forwarding (Turnip / PanVK / AGX)

See [docs/ROADMAP.md](docs/ROADMAP.md) for the architectural evolution plan.

---

## Contributing & Feedback

Issues and Pull Requests are welcome! When filing a bug report, please include:
1. Environment details (bare-metal model or QEMU / VMware setup, plus GPU model)
2. Steps to reproduce
3. Logs exported from **System Monitor → System Logs** or `/tmp/spinel-shell.log`, `~/.local/state/spinelos/apps.log`, and `spinel-agent-ctl doctor`

---

## License

- First-party **Spinel Shell source code, UI frontend, built-in themes, and build/install scripts** in this repository are licensed under the [MIT License](LICENSE).
- Built OS images bundle third-party open-source and proprietary components (such as the Linux kernel, `labwc`, Mesa, `VTE4`, NVIDIA drivers, Fcitx5, etc.), each governed by its own upstream or EULA terms; every ISO build generates `spinelos-packages-licenses.tsv` and `NOTICE-THIRD-PARTY.txt`.
