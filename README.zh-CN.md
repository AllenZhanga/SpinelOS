<div align="center">

# Spinel OS

**一个不基于 KDE / GNOME / XFCE 的轻量级 Wayland 桌面操作系统**

基于 **Arch Linux (`archiso`)** 构建 · **C++20 原生桌面外壳 (`spinel-shell`)** · **AI 原生 OS 底座 (`spinel-os-mcp` & 平行沙箱)** · **Steam / Gamescope / MangoHud 开箱即用**

[![Arch Linux Base](https://img.shields.io/badge/Base-Arch%20Linux-1793D1?logo=archlinux&logoColor=white)](https://archlinux.org/)
[![Wayland Compositor](https://img.shields.io/badge/Wayland-labwc%20%2B%20GTK4%20Layer--Shell-38BDF8)](https://labwc.github.io/)
[![C++20](https://img.shields.io/badge/Desktop%20Shell-C%2B%2B20%20%2B%20WebKitGTK%206-6366F1)](desktop/)
[![License: MIT](https://img.shields.io/badge/License-MIT-10B981.svg)](LICENSE)
[![GitHub Issues](https://img.shields.io/github/issues/AllenZhanga/SpinelOS)](https://github.com/AllenZhanga/SpinelOS/issues)

[English](README.md) · **简体中文**

[核心特性](#核心特性) · [架构设计](#为什么不用传统桌面) · [AI 原生能力](#3-ai-原生-os-基础设施与平行沙箱) · [快速上手](#快速体验与安装) · [构建 ISO](#从源码构建-iso)

</div>

---

> **预览截图位（待补充真机录屏 / 高清截图）**  
> 建议将物理机或显卡直通虚拟机实拍图放入 `docs/assets/` 后在此取消注释展示：
> <!-- ![SpinelOS Desktop](docs/assets/desktop-light.png) -->
> <!-- ![SpinelOS Gamescope & Control Center](docs/assets/games-center.png) -->

> [!NOTE]
> **当前项目状态（Alpha 预览阶段）**：Spinel OS 目前面向桌面开发者、Linux 日用及游戏玩家提供源码构建、Live ISO 与云端 OTA 更新链路。欢迎在 [Issues](https://github.com/AllenZhanga/SpinelOS/issues) 提交真机显卡适配（AMD / Intel / NVIDIA）与体验反馈。

---

## Spinel OS 是什么？

很多 Arch 衍生发行版只是在 KDE 或 GNOME 上预装常用软件或驱动。**Spinel OS 希望做得更轻、更统一、全链路可控**：

- **完全独立的 C++20 桌面外壳与双进程低内存架构**：不依赖 KDE / GNOME / XFCE 整套桌面环境，由自研 C++20 原生外壳 [`spinel-shell`](desktop/)（纯 `GTK4 + Cairo + gtk4-layer-shell`，常驻内存仅约 **75 MB PSS**）承担全局顶栏、Dock、桌面、Control Center、Spotlight 全局启动器与 AI 灵动岛；设置、应用中心、游戏中心、系统监视器、Spinel Doctor 与安装向导拆分为按需拉起的 `spinel-webui`（`WebKitGTK 6.0`），关闭即释放全部内存。底层由轻量的 `labwc`（`wlroots`）负责 Wayland/DRM 合成与 `XWayland` 兼容。
- **全自研核心桌面应用栈**：内置原生 GTK4 文件管理器 [`spinel-files`](desktop/)（`GListStore` 虚拟化列表、`org.freedesktop.FileManager1` D-Bus 服务与内置归档压缩/解压）、多标签终端 [`spinel-terminal`](desktop/)（`GTK4 + VTE4`，实时同步系统主题）以及隔离平行沙箱 [`spinel-sandbox`](desktop/)（支持画中画 PiP 接管），并配备彩色尖晶石 Logo、静默 GRUB 与 Plymouth 开机动画。
- **面向「办公 + AI Agent 协作 + 游戏」三合一设计**：兼顾窗口多任务、通过 MCP 与 Coding Agent 原生联动、开箱中文环境与系统级代理透传，以及预置 GPU 与 Gamescope 游戏运行栈。

---

## 核心特性

### 1. 独立设计的 Spinel Shell 桌面体验
- **全局动态顶部菜单栏、系统托盘与多工作区**：随焦点应用动态切换菜单栏菜单，内置原生 `StatusNotifierItem`（`SNI` + `DBusMenu`）系统托盘、`wl-clip-persist` 剪贴板历史（10 条）、Bento 风格控制中心以及多工作区切换（`Super + 1~4` / 顶栏指示器）。
- **Spotlight 全局搜索启动器 (`Alt + Space`)**：一键模糊搜索已安装桌面应用、本地 Steam 游戏、Windows 应用、系统设置直达页与快捷操作。
- **悬浮 Dock、桌面交互与多主题引擎**：支持 Dock 拖拽排序与右键菜单，遵循统一视觉规范 [docs/UI_SPEC.md](docs/UI_SPEC.md)，内置多套主题（默认 `tahoe-light` 浅色液态玻璃）及自定义本地壁纸图片，搭配原生截图（`spinel-screenshot`）与锁屏（`spinel-lock`）。

### 2. 全自研日用与生产力组件
- **原生文件管理器 (`spinel-files`)**：基于 `GtkListView`/`GtkGridView` + `GListStore` 虚拟化视图与异步目录扫描，支持 `FileManager1` 在文件夹中显示，以及右键内置压缩/解压。
- **多标签终端 (`spinel-terminal`)**：基于 `GTK4 + VTE4`（`Super + Enter` / `Ctrl + T` 继承工作目录 / `Ctrl + Shift + C/V` / `Ctrl + F`），配色与字体随系统主题同步。
- **独立拆分的功能窗口 (`spinel-webui`)**：
  - **Spinel 应用中心**：全部采用宿主机原生安装（`pacman` 与官方原生包），覆盖浏览器、LibreOffice、PDF/笔记工具、VS Code、容器工具与本地大模型引擎。
  - **系统监视器 & Spinel Doctor**：独立进程与 `PSS` 内存监控、分类系统/服务/AI 审计日志查看（`?mode=monitor`）以及一键系统体检（`?mode=doctor`）。
  - **精细化系统设置**：Wi-Fi/网络与蓝牙、显示与缩放、声音与电源、交互式全局快捷键录制（自动同步 `labwc`）、Btrfs `snapper` 快照管理与分类「权限与安全」。

### 3. AI 原生 OS 基础设施与平行沙箱
> 完整使用与排查说明见 [docs/AI_OS_GUIDE.md](docs/AI_OS_GUIDE.md) · [docs/AI_OS_PLAN.md](docs/AI_OS_PLAN.md)
- **顶栏 AI 灵动岛**：实时聚合运行中的 AI Coding 会话（自动识别 Antigravity、Codex、Claude Code、`spinel-os-mcp` 工具调用及 `/proc` 探针），支持一键跳转对应窗口/工作区或沙箱 PiP 接管，并弹出 **Tier 3 交互审批卡片**（允许 / 快照后允许 / 沙箱运行 / 拒绝）。
- **`spinel-os-mcp` (`stdio`) 与一键无损写入配置**：向外部 Agent 暴露 18 项受控系统能力（桌面上下文、截图、剪贴板、体检、沙箱、Btrfs 还原点、本地服务控制），配合 `Tier 0–3` 分级权限与去噪审计日志；支持在 **系统设置 → 权限与安全** 或 `spinel-agent-ctl mcp-install <client>` 一键无损写入 **Antigravity**（`~/.gemini/config/mcp_config.json`）、**Codex**、**Claude Code**、**DeepSeekHarness**、**Gemini CLI** 配置。
- **平行沙箱 (`spinel-sandbox`) 与 Cloud Relay**：支持隔离桌面环境与画中画预览、网络/剪贴板开关，并可搭配 [server/spinel-relay](server/spinel-relay) 实现远程桌面连续性与云端 OTA 分发。

### 4. 开箱即用的 PC / 掌机游戏环境
- **全系主流 GPU 驱动预置**（见 [archiso/packages.custom.x86_64](archiso/packages.custom.x86_64)）：
  - **AMD**：`mesa` + `vulkan-radeon`（含 32 位 `lib32-*`）
  - **Intel**：`mesa` + `vulkan-intel`（含 32 位 `lib32-*`）
  - **NVIDIA**：`nvidia-open` + `nvidia-utils` + `lib32-nvidia-utils`（适配 GTX 16 及 RTX 20/30/40/50 系）
- **开源合规游戏栈**：ISO 预集成 `[multilib]`、`gamescope`、`mangohud`、`gamemode`、PipeWire 与蓝牙手柄支持；专有 `Steam` 客户端未预捆绑在镜像中，点击图标或在**游戏中心**可通过应用商店一键在线安装（构建 ISO 同时生成 `spinelos-packages-licenses.tsv` 与 `NOTICE-THIRD-PARTY.txt`）。
- **游戏中心 & 本地 Windows 程序支持**：自动扫描本地 Steam 游戏库与封面图，支持直达 **Steam** 或 **Gamescope 主机大屏模式**、逐游戏调节 MangoHud/GameMode/缩放，并支持按需安装 Wine（`spinel-wine`，虚拟 C 盘位于 `~/.local/share/spinelos/wine-prefix/drive_c`）。

### 5. 真正面向中文用户的日用体验
- **Fcitx5 Wayland 输入法开箱可用**：预置中文拼音（`fcitx5-chinese-addons`）、可选 **Rime 雾凇拼音**、`Noto Color Emoji` 回退及浅/深色 9-patch 磨砂皮肤。
- **系统级代理自动接管**：HTTP / SOCKS 代理设置（或外部 `org.gnome.system.proxy` 工具如 Clash/Monocloud）自动同步至浏览器与 Steam。

---

## 为什么不用传统桌面？

| 维度 | 传统桌面环境（KDE / GNOME / XFCE） | **Spinel OS (`Spinel Shell + labwc`)** |
| :--- | :--- | :--- |
| **桌面外壳** | 完整桌面套件，后台组件与服务繁多 | 自研 C++20 `spinel-shell`（纯 GTK4 + Cairo）+ 按需 `spinel-webui` + `labwc` |
| **视觉与交互一致性** | 常混用 GTK3 / GTK4 / Qt，顶栏与设置体验割裂 | 全局统一 [UI_SPEC.md](docs/UI_SPEC.md) 规范，壁纸、顶栏、Dock 与终端主题一键同步 |
| **中文与代理开箱度** | 装机后常需手动配置 `fcitx5` 环境变量与代理 | 开箱预设 Wayland IME、Portals（`SpinelOS:GNOME`）与 GSettings 代理继承 |
| **AI 与隔离底座** | 缺少系统级 Agent 会话感知、分级审批与沙箱画中画 | 原生 AI 灵动岛、`spinel-os-mcp`（`Tier 0–3`）、Btrfs 还原点与 `spinel-sandbox` |
| **可定制与迭代速度** | 修改全局交互需维护多仓库插件或 Patch 上游 | 完整桌面栈均位于 [desktop/](desktop/) 内，支持秒级热部署与 OTA 更新 |

---

## 快速体验与安装

### 硬件与虚拟机建议
- **架构**：`x86_64`（UEFI 启动，建议开启 3D 加速；无 3D 加速的虚拟机亦已做软件渲染兼容）
- **内存**：建议 8 GB 及以上（最低 4 GB）
- **显卡**：AMD Radeon / Intel UHD·Arc / NVIDIA GTX 16 系及 RTX 20~50 系

### 使用方式
1. 从 [Releases](https://github.com/AllenZhanga/SpinelOS/releases) 下载最新镜像（或按下方步骤自行构建 `out/spinelos-*.iso`）。
2. **真机体验**：使用 [Ventoy](https://www.ventoy.net/)、`balenaEtcher` 或 `dd` 写入 U 盘，以 **UEFI 模式** 引导进入 Live 桌面。
3. **虚拟机体验**：在 QEMU / VMware / VirtualBox 中挂载 ISO（建议启用 UEFI 与 3D 加速）。
4. **安装到硬盘**：在 Live 桌面点击 **「安装 Spinel OS」**（`spinel-install`），配置目标磁盘（支持 Btrfs `@`/`@home`/`@snapshots` 与双系统）、主机名与账户密码即可完成安装。

> [!IMPORTANT]
> **Live 环境说明**：Live ISO 默认自动登录演示账户 `gamer` 并允许免密 `sudo` 以便快速尝鲜与部署测试；安装到硬盘正式使用前，请务必为系统账户设置强密码。

---

## 从源码构建 ISO

只要宿主机为 `x86_64` Linux 并安装 **Docker**（建议预留 15 GB 以上磁盘空间），即可一键构建完整可启动 ISO：

```bash
chmod +x scripts/build-iso.sh docker/build-inside-container.sh
./scripts/build-iso.sh
```

构建产物位于 `out/spinelos-*.iso`。

<details>
<summary>点击展开：C++20 单元测试、热部署、云端 OTA 与仓库目录结构</summary>

### 目录结构

```text
.
├── desktop/                     # 原生 C++20 桌面栈 + HTML/CSS/JS 前端
│   ├── CMakeLists.txt           # 构建 shell、webui、files、terminal、sandbox、agent-ctl、os-mcp、tests
│   ├── src/                     # Wayland layer-shell、IPC 路由、AI/MCP 引擎与系统控制
│   ├── tests/                   # spinel-tests (CTest spinel_unit_tests)
│   └── ui/                      # 设置、应用中心、游戏中心、监视器、Doctor 与安装器界面
├── docs/                        # UI_SPEC.md、AI_OS_GUIDE.md、AI_OS_PLAN.md、OTA_RELEASE_GUIDE.md、ROADMAP.md
├── archiso/                     # ArchISO 定制覆盖层
│   ├── packages.custom.x86_64   # 内核、显卡、Wayland、Gamescope、Fcitx5 与 Spinel 依赖包清单
│   └── airootfs/                # 根文件系统配置、labwc/portals、spinel-install、spinel-update
├── docker/                      # 容器化 ISO 与 C++20 交叉构建环境
├── server/spinel-relay/         # Go TLS 中继服务端（远程沙箱串流 + 云端 OTA 分发）
├── scripts/
│   ├── build-iso.sh             # 一键打包可启动 ISO
│   ├── dev-deploy.sh            # 向运行中的 SpinelOS VM 热更新 (--ui-only / 默认 / --full)
│   ├── release-ota.sh           # 打包签名并发布云端 OTA 更新
│   ├── build-relay.sh           # 升级云端 spinel-relay 服务
│   └── sync-ecs.sh              # 同步代码到远程构建机
└── .github/workflows/build-iso.yml
```

### C++20 全目标构建与单元测试（通过 `spinelos-builder`）

```bash
docker run --rm --platform linux/amd64 -v "$PWD":/workspace -w /workspace spinelos-builder \
  bash -c "cmake -S desktop -B build/linux-amd64 -DCMAKE_BUILD_TYPE=Release && cmake --build build/linux-amd64 -j4 && ctest --test-dir build/linux-amd64 --output-on-failure -V"
```

### 热部署与云端 OTA 更新

```bash
# 仅同步 UI 改动并重启（约 1 秒）
./scripts/dev-deploy.sh <user>@<VM_IP> --ui-only

# 增量编译 C++20 二进制 + 同步 UI + 热重启
./scripts/dev-deploy.sh <user>@<VM_IP>

# 全量更新系统包、主题、labwc/portal 配置与启动脚本
./scripts/dev-deploy.sh <user>@<VM_IP> --full

# 在已装 Spinel OS 设备上直接拉取云端最新 OTA 更新
sudo spinel-update upgrade --full --yes
```

</details>

---

## 路线图与 ARM64 (`aarch64`) 探索

- [x] 自研 C++20 + GTK4 Layer-Shell + WebKitGTK 6.0 双进程桌面（`spinel-shell`、`spinel-webui`、`spinel-terminal`、`spinel-files`）
- [x] 独立应用中心（宿主机原生安装）、游戏中心、系统监视器（`PSS` + 分类日志）与 Spinel Doctor
- [x] AI OS 基础设施（`spinel-os-mcp`、灵动岛、`spinel-sandbox`、安全一键写入 MCP 配置与审计去噪）及云端 OTA（`spinel-update`）
- [ ] 更多真机显卡兼容性测试与硬盘安装器存储/双系统细节打磨
- [ ] **ARM64 (`aarch64`) 探索**：`desktop/` 自身可跨架构编译，上游 `archiso` 仅面向 `x86_64`；后续计划探索基于 **Arch Linux ARM + FEX-Emu + Proton** 与 Vulkan（Turnip / PanVK / AGX）转发运行桌面与游戏负载

更多架构演进细节请见 [docs/ROADMAP.md](docs/ROADMAP.md)。

---

## 参与贡献与反馈

欢迎提交 Issue 或 Pull Request！提交 Bug 时建议附上：
1. 运行环境（物理机型号或 QEMU / VMware 配置，以及 GPU 型号）
2. 问题复现步骤
3. 在「**系统监视器 → 系统日志**」导出的日志，或 `/tmp/spinel-shell.log`、`~/.local/state/spinelos/apps.log`、`spinel-agent-ctl doctor` 输出

---

## 开源许可 (License)

- 本仓库中的 **Spinel Shell 源码、UI 前端、内置主题配置与构建/安装脚本** 基于 [MIT License](LICENSE) 开源。
- 构建生成的系统镜像及其中集成的第三方开源或专有组件（如 Linux 内核、`labwc`、Mesa、`VTE4`、NVIDIA 驱动、Fcitx5 等）各自遵循其原始开源或最终用户许可协议；每次构建 ISO 均会在输出目录附带完整软件包及许可清单（`spinelos-packages-licenses.tsv` 与 `NOTICE-THIRD-PARTY.txt`）。
