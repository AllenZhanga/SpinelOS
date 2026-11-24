<div align="center">

# SpinelOS

**一个不基于 KDE / GNOME / XFCE 的轻量级 Wayland 游戏桌面发行版**

基于 **Arch Linux (`archiso`)** 构建 · **C++20 原生桌面外壳 (`Spinel Shell`)** · **Steam / Gamescope / MangoHud 开箱即用** · **原生中文输入法与系统代理**

[![Arch Linux Base](https://img.shields.io/badge/Base-Arch%20Linux-1793D1?logo=archlinux&logoColor=white)](https://archlinux.org/)
[![Wayland Compositor](https://img.shields.io/badge/Wayland-labwc%20%2B%20GTK4%20Layer--Shell-38BDF8)](https://labwc.github.io/)
[![C++20](https://img.shields.io/badge/Desktop%20Shell-C%2B%2B20%20%2B%20WebKitGTK%206-6366F1)](desktop/)
[![License: MIT](https://img.shields.io/badge/License-MIT-10B981.svg)](LICENSE)
[![GitHub Issues](https://img.shields.io/github/issues/AllenZhanga/SpinelOS)](https://github.com/AllenZhanga/SpinelOS/issues)

[核心特性](#核心特性) · [架构设计](#为什么不用传统桌面) · [快速上手](#快速体验与安装) · [自构建 ISO](#从源码构建-iso) · [English Overview](#english-overview)

</div>

---

> **预览截图位（待补充真机录屏 / 高清截图）**  
> 建议将物理机或显卡直通虚拟机实拍图放入 `docs/assets/` 后在此取消注释展示：
> <!-- ![SpinelOS Desktop](docs/assets/desktop-light.png) -->
> <!-- ![SpinelOS Gamescope & Control Center](docs/assets/games-center.png) -->

> [!NOTE]
> **当前项目状态（Alpha 预览阶段）**：SpinelOS 目前面向桌面开发者、Linux 玩家与早期尝鲜用户提供源码与 Live ISO 构建链路。欢迎在 [Issues](https://github.com/AllenZhanga/SpinelOS/issues) 提交真机显卡适配（AMD / Intel / NVIDIA）与体验反馈。

---

## SpinelOS 是什么？

很多 Linux 游戏发行版只是在 KDE 或 GNOME 上预装驱动和 Steam。**SpinelOS 希望做得更轻、更统一**：

- **完全独立的桌面外壳**：不依赖 KDE / GNOME / XFCE 整套桌面环境，由自研的 C++20 程序 [`spinel-shell`](desktop/)（`GTK4` + `gtk4-layer-shell` + `WebKitGTK 6.0`）承担顶栏、Dock、控制中心、设置、应用商店、游戏中心与 Live 安装器；底层仅由极轻量的 `labwc`（`wlroots`）负责 Wayland/DRM 合成与 `XWayland` 兼容。
- **为桌面体验与游戏玩家同步打磨**：兼顾主机大屏模式、桌面级窗口多任务、开箱中文环境与透明的系统权限/进程控制。

---

## 核心特性

### 1. 独立设计的 Spinel Shell 桌面体验
- **全局动态顶部菜单栏**：随当前焦点应用自动切换菜单项，右上角集成状态监控与弹出式控制中心。
- **悬浮 Dock 与统一主题引擎**：遵循 [UI 设计规范](docs/UI_SPEC.md)，内置 4 套主题并支持 `~/.config/spinelos/themes/*.theme` 扩展：
  - **macOS 浅色液态玻璃**（`tahoe-light`，默认）
  - **尖晶石深蓝极光**（`crystal-aurora`）
  - **黑曜星云霓虹**（`obsidian-neon`）
  - **森境翡翠极光**（`emerald-nordic`）
- **一致的多窗口原生架构**：顶栏与 Dock 常驻轻量运行，设置、应用商店、游戏中心、安装器按需独立拉起并在关闭时自动释放内存；虚拟机/无 3D 加速环境下自动降级渲染特效以保障响应速度。

### 2. 开箱即用的 PC / 掌机游戏环境
- **全系主流 GPU 驱动预置**（见 [packages.custom.x86_64](archiso/packages.custom.x86_64)）：
  - **AMD**：`mesa` + `vulkan-radeon`（含 32 位 `lib32-*`）
  - **Intel**：`mesa` + `vulkan-intel`（含 32 位 `lib32-*`）
  - **NVIDIA**：`nvidia-open` + `nvidia-utils` + `lib32-nvidia-utils`（适配 RTX 20/30/40/50 及 GTX 16 系）
- **游戏栈即装即玩**：预集成 `[multilib]`、Steam、Proton 兼容预设、Gamescope、MangoHud、GameMode、PipeWire 与蓝牙手柄支持。
- **游戏与启动中心**：自动扫描本地 Steam 游戏库与封面图，可直接从桌面/游戏中心一键直达 **Steam 客户端** 或 **Gamescope 主机大屏模式**，并在设置页可视化开关 MangoHud HUD、GameMode 与渲染分辨率。

### 3. 真正面向中文用户的日用体验
- **中文输入法开箱可用**：内置 **Fcitx5 Wayland**、中文拼音（`fcitx5-chinese-addons`）及 **Rime / 雾凇拼音** 一键切换，并与桌面浅/深色主题自动联动。
- **系统级代理自动接管**：在「系统设置 → Wi-Fi 与网络」配置 HTTP / SOCKS 代理后，自动同步至桌面启动的浏览器与 Steam（遇到未加载代理的旧 Steam 更新进程会自动重启接管，解决首启更新缓慢问题）。
- **日常工具精简完备**：
  - 原生多标签终端 **Ptyxis**（预设 `Ctrl+T` 新开标签页，自动跟随系统浅/深色切换）
  - **Thunar** 文件管理（含压缩包插件与 GVFS）、Mousepad、Ristretto
  - **应用商店**：整合系统软件源（`pacman`）与 Flatpak / Flathub 一键安装主流浏览器、Heroic、Lutris、Discord、OBS 等
  - **进程监控 / 权限与安全 / 系统日志**：设置内直接查看 CPU 与内存占用并结束异常进程、用通俗条目管理 Flatpak / Portal 应用授权与管理员提权弹窗、导出桌面与系统排障日志

---

## 为什么不用传统桌面？

| 维度 | 传统游戏桌面（KDE / GNOME / XFCE） | **SpinelOS (`Spinel Shell + labwc`)** |
| :--- | :--- | :--- |
| **桌面外壳** | 完整桌面套件，服务与后台组件多 | 自研 C++20 `GTK4 + WebKitGTK 6.0` 外壳 + `labwc` 合成器 |
| **主题与交互一致性** | 常混用 GTK3/GTK4/Qt 多套风格，顶栏/设置体验割裂 | 全局统一 [UI_SPEC.md](docs/UI_SPEC.md) 设计规范，主题一键同步到壁纸、顶栏、Dock、窗口装饰与终端 |
| **国内网络与中文开箱度** | 装机后往往需手动配置 `fcitx5` 环境变量与 Steam 代理 | 开箱预设 Wayland IME、`xdg-desktop-portal` 授权链与代理环境变量同步 |
| **可定制性** | 修改全局桌面交互需维护插件或 Patch 上游 | 桌面、顶栏、应用商店、设置全部在本仓库 [desktop/](desktop/) 内掌控，热部署秒级迭代 |

---

## 快速体验与安装

### 硬件与虚拟机建议
- **架构**：`x86_64`（UEFI 启动，建议开启 3D 加速以获得最佳体验；无 3D 加速的虚拟机也已做软件渲染兼容）
- **内存**：建议 8 GB 及以上（最低 4 GB）
- **显卡**：AMD Radeon / Intel UHD·Arc / NVIDIA GTX 16 系及 RTX 20~50 系

### 使用方式
1. 从 [Releases](https://github.com/AllenZhanga/SpinelOS/releases) 下载最新镜像（或按下方步骤自行构建 `out/spinelos-*.iso`）。
2. **真机体验**：使用 [Ventoy](https://www.ventoy.net/)、`balenaEtcher` 或 `dd` 写入 U 盘，以 **UEFI 模式** 引导进入 Live 桌面。
3. **虚拟机体验**：在 QEMU / VMware / VirtualBox 中挂载 ISO（建议启用 UEFI 与 3D 加速）。
4. **安装到硬盘**：在 Live 桌面点击 **「安装 SpinelOS」**，按向导选择目标磁盘并配置主机名、账户与密码即可完成安装。

> [!IMPORTANT]
> **Live 环境说明**：Live ISO 默认自动登录演示账户 `gamer` 并允许免密 `sudo` 以便快速尝鲜与部署测试；安装到硬盘正式使用前，请务必为系统账户设置强密码。

---

## 从源码构建 ISO

只要宿主机为 `x86_64` Linux（Ubuntu / Debian / Arch / Alibaba Cloud Linux 等）并安装 **Docker**（建议预留 15 GB 以上磁盘空间），即可一键构建完整可启动 ISO：

```bash
chmod +x scripts/build-iso.sh docker/build-inside-container.sh
./scripts/build-iso.sh
```

构建产物位于 `out/spinelos-*.iso`。

<details>
<summary>点击展开：面向开发者的热部署与仓库目录结构</summary>

### 目录结构

```text
.
├── desktop/                     # 独立桌面外壳 Spinel Shell (C++20 + HTML/CSS/JS)
│   ├── CMakeLists.txt
│   ├── src/                     # Wayland Layer-Shell 宿主、sysbridge IPC、主题/Steam/系统控制
│   └── ui/                      # 桌面前端、设置、游戏中心、应用商店、安装器界面
├── docs/
│   └── UI_SPEC.md               # SpinelOS 统一 UI 设计规范（Design Tokens / 栅格 / 组件 / 主题）
├── archiso/                     # ArchISO 定制覆盖层
│   ├── packages.custom.x86_64   # 内核、显卡、Wayland、Steam、Gamescope、Fcitx5、Ptyxis 等软件清单
│   └── airootfs/                # 根文件系统覆盖（labwc/portals/themes/autostart/install 脚本）
├── docker/                      # 容器化 ISO 与 C++20 交叉构建环境
├── scripts/
│   ├── build-iso.sh             # 一键打包可启动 ISO
│   ├── dev-deploy.sh            # 向运行中的 SpinelOS VM 热更新二进制/UI/系统包 (--ui-only / --full)
│   └── sync-ecs.sh              # 同步代码到远程构建机
└── .github/workflows/build-iso.yml
```

### 对运行中的 VM 快速热部署

```bash
# 仅同步 UI 改动并重启 spinel-shell（约 1 秒）
./scripts/dev-deploy.sh gamer@<VM_IP> --ui-only

# 增量编译 C++20 外壳 + 同步 UI + 热重启
./scripts/dev-deploy.sh gamer@<VM_IP>

# 全量更新系统包、主题、labwc/portal 配置与启动脚本
./scripts/dev-deploy.sh gamer@<VM_IP> --full
```

</details>

---

## 路线图与 ARM64 (`aarch64`) 探索

- [x] 自研 C++20 + GTK4 Layer-Shell + WebKitGTK 6.0 原生桌面外壳与双主题窗口 chrome
- [x] Steam / Gamescope / MangoHud 集成、Flatpak 应用商店、Fcitx5 中文输入法与进程监控
- [ ] 更多真机显卡（多屏高刷、混合显卡笔记本、掌机手柄热键）实测与打磨
- [ ] 优化硬盘安装器分区体验（双系统识别 / Btrfs 快照恢复探索）
- [ ] **ARM64 (`aarch64`) 支持调研**：Arch 官方 `archiso` 仅面向 `x86_64`，`desktop/` 自身可跨架构编译；后续计划探索基于 **Arch Linux ARM + FEX-Emu + Proton** 在 ARM64 设备上转译运行 Windows Steam 游戏与 Vulkan（Turnip / PanVK / AGX）转发

---

## English Overview

**SpinelOS** is an Arch-based gaming Linux distribution featuring a **custom standalone Wayland desktop shell (`spinel-shell`)** built from scratch in **C++20 (`GTK4` + `gtk4-layer-shell` + `WebKitGTK 6.0`)** on top of the lightweight **`labwc`** compositor — without KDE, GNOME, or XFCE.

- **Ready for Gaming**: Pre-configured `[multilib]`, AMD (`mesa`/`vulkan-radeon`), Intel (`mesa`/`vulkan-intel`), NVIDIA (`nvidia-open`), `steam`, `gamescope`, `mangohud`, `gamemode`, and `pipewire`.
- **Cohesive Custom UI**: Unified global top menu bar, floating dock, control center, multi-theme engine (Light Liquid Glass, Crystal Aurora, Obsidian Neon, Emerald Nordic), Flatpak + `pacman` App Store, and built-in Process Monitor.
- **Out-of-the-Box CJK & Proxy Workflow**: Fcitx5 Wayland + Rime/Pinyin IME, Ptyxis tabbed terminal (`Ctrl+T`), and GUI proxy toggles automatically propagated to Steam and browsers.
- **Build Your Own ISO**: Run `./scripts/build-iso.sh` on any Docker-enabled `x86_64` Linux host to generate `out/spinelos-*.iso`.

---

## 参与贡献与反馈

欢迎提交 Issue 或 Pull Request！提交 Bug 时建议附上：
1. 运行环境（物理机型号或 VMware / QEMU 配置，以及 GPU 型号）
2. 问题复现步骤
3. 在「系统设置 → 系统日志」或 `/tmp/spinel-shell.log`、`~/.local/state/spinelos/apps.log` 捕获的相关日志

---

## 开源许可 (License)

- 本仓库中的 **Spinel Shell 源码、UI 前端、内置主题配置与构建/安装脚本** 基于 [MIT License](LICENSE) 开源。
- 构建生成的系统镜像及其中集成的第三方开源或专有组件（如 Arch Linux 基础包、Linux 内核、`labwc`、Mesa、NVIDIA 驱动、Ptyxis、Fcitx5、Steam 客户端等）各自遵循其原始开源或最终用户许可协议。
