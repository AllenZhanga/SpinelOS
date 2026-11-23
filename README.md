# SpinelOS — 定制 Arch Linux 游戏桌面发行版

一个基于 **Arch Linux (`archiso`)** 构建的可打包分发 Linux 系统镜像：
- **完全独立桌面**：不依赖 KDE / XFCE / GNOME，采用 **C++20 (`gtk4` + `webkitgtk-6.0` + `gtk4-layer-shell`)** 作为原生 Wayland 高性能宿主，搭配 **HTML + CSS + JS** 构建桌面顶栏与游戏启动中心，底层仅使用轻量可控的 `labwc` (`wlroots`) 负责 DRM/KMS 与 XWayland 窗口托管。
- **开箱即用 Windows Steam 游戏**：内置 `[multilib]` 32 位兼容库、`steam`、`gamescope`、`mangohud`、`pipewire` 与 `wireplumber`。
- **全显卡兼容**：同时集成 AMD（`mesa` + `vulkan-radeon`）、Intel（`mesa` + `vulkan-intel`）与 NVIDIA RTX 20/30/40/50 及 GTX 16 系（`nvidia-open` + `nvidia-utils` + `lib32-nvidia-utils`）。

---

## 目录结构

```text
.
├── desktop/                     # 独立桌面外壳 (C++20 + HTML/CSS/JS)
│   ├── CMakeLists.txt
│   ├── src/main.cpp             # Wayland Layer-Shell 宿主 & JS<->C++ sysbridge IPC
│   └── ui/                      # 桌面前端 (index.html / styles.css / app.js)
├── archiso/                     # ArchISO 定制覆盖层
│   ├── packages.custom.x86_64   # 显卡、Wayland、Steam、音频、字体等软件清单
│   └── airootfs/                # 根文件系统覆盖（自动登录、NVIDIA KMS、桌面启动脚本）
├── docker/
│   ├── Dockerfile.builder       # 基于 archlinux:latest 的打包容器
│   └── build-inside-container.sh
├── scripts/
│   └── build-iso.sh             # 在 x86_64 Linux ECS 上一键打包 ISO
└── .github/workflows/build-iso.yml
```

---

## 在 x86_64 Linux ECS 上构建 ISO

无论你的 ECS 宿主机是 Ubuntu、Debian、Alibaba Cloud Linux 还是 Arch Linux，只要装有 **Docker** 即可直接打包（建议预留 15GB+ 可用磁盘空间）：

```bash
chmod +x scripts/build-iso.sh docker/build-inside-container.sh
./scripts/build-iso.sh
```

构建完成后，镜像位于 `out/spinelos-*.iso`，可写入 U 盘（如 Ventoy / `dd`）或直接在虚拟机（UEFI 模式）启动测试。

> **国内 ECS 网络加速提示**：如果在国内 ECS 上拉取 Arch 官方源较慢，可在 `docker/Dockerfile.builder` 和 `docker/build-inside-container.sh` 中将 `/etc/pacman.d/mirrorlist` 替换为国内镜像源（如阿里云、清华 TUNA、中科大镜像）。

---

## 关于支持 ARM64 (`aarch64`) CPU 的架构说明

1. **为什么不能把 x86_64 和 ARM64 塞进同一个 Arch ISO？**
   - Arch 官方 `archiso` 仅面向 `x86_64`。`aarch64` 需要基于 **Arch Linux ARM**（或 Fedora/Debian ARM64）单独构建镜像，且两者内核、引导器、驱动及二进制指令完全不同。
2. **ARM64 上如何跑 Windows Steam 游戏？**
   - 需要引入 **FEX-Emu + Proton 11+**（参考 Valve 在 Steam Frame 等 ARM64 设备上的做法）将 x86/x86_64 游戏指令实时转译为 ARM64，并将图形调用转发至主机 ARM GPU Vulkan 驱动（如 Adreno Turnip、Mali PanVK、Apple AGX）。
3. **跨架构复用策略**：
   - 本仓库的 `desktop/`（C++20 + HTML/CSS/JS）代码本身是完全跨架构的，后续可在同一台 x86_64 ECS 上通过 `qemu-user-static` + Docker `--platform linux/arm64` 交叉编译，并针对特定 ARM64 设备（如 UEFI+ACPI SBSA 设备、骁龙笔记本或 RK3588 等带对应 DTB/内核的板卡）新增 `aarch64` 构建目标。
