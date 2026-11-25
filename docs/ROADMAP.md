# SpinelOS 演进路线图与架构重构规划

> **定位目标**：将 SpinelOS 从“轻量级 Wayland 游戏发行版（Alpha）”演进为可长期作为日常主力的 **Office（办公） + AI（Coding & AI Agent OS） + Game（游戏）三合一桌面操作系统**。

---

## 现状审计摘要（2026-11-25 基线）

- **优势保留**：
  - 双进程低内存桌面架构：`labwc`（`wlroots`）+ 纯原生 GTK4/Cairo 常驻外壳 `spinel-shell` + 按需拉起的 WebKitGTK6 `spinel-webui` + 自研多标签终端 `spinel-terminal`。
  - 系统工程底座完善：ArchISO + `[multilib]`、AMD / Intel / `nvidia-open` 驱动、DRM/Pixman/Safe Graphics 兜底、`spinel-install`（Btrfs `@`/`@home`/`@snapshots` + 双系统）、`fcitx5`、代理环境变量透传、Polkit / XDG Portals、`spinel-doctor` 体检与 `spinel-tests` 单元测试。
- **需要优先解决的技术债**：
  - 核心模块“上帝文件”偏大（重构前 `native_shell.cpp` 约 4,165 行、`system_control.cpp` 约 3,169 行、`app.js` 约 2,724 行），后续接入办公托盘、Agent 总线和本地推理控制会急剧增加耦合度。
  - IPC 仍依赖长串联 `if/else if` 字符串匹配与换行拼参数，缺少结构化请求/响应与可复用的指令注册表。
  - 办公（系统托盘 SNI、Office/PDF/打印栈）与 AI Agent 底座（容器沙箱、受控系统能力 API/MCP、GPU 显存仲裁）尚待建设。

---

## 第一步：模块化拆分与结构化 IPC（工程地基）

1. **拆分 `system_control` 到 `desktop/src/sys/` 按域聚合**
   - `sys/network.cpp`：Telemetry、Media 快照、Proxy、Perf 预设、Wi-Fi、Bluetooth。
   - `sys/display_input.cpp`：显示器模式与 `wlr-randr`、全局快捷键与 `labwc/rc.xml` 生成、Fcitx5/Rime 与终端默认配置。
   - `sys/audio_power.cpp`：音频输出/输入设备、功耗档位探测、闲置自动锁屏、亮度控制、硬件 OSD 联动、手柄热键监听。
   - `sys/doctor_logs.cpp`：系统日志与原生诊断窗、Spinel Doctor 健康体检与脱敏导出、权限/Polkit/Portals、进程/PSS 监控、Mako 通知历史与勿扰、剪贴板历史、OOBE 欢迎状态。
2. **拆分 `native_shell` 到 `desktop/src/shell/` 原生壳层模块**
   - `shell/shell_core.cpp`：共享 Cairo/Glyph/Logo 绘制、`apply_native_css`、`init_native_shell` / `shutdown_native_shell` 及状态同步调度。
   - `shell/top_bar.cpp`：顶部菜单栏、聚焦应用动态菜单、剪贴板弹窗与通知历史抽屉。
   - `shell/desktop_monitors.cpp`：桌面自定义壁纸缓存/极光绘制、桌面快捷图标、右键菜单及多显示器 `GdkMonitor` 热插拔守护。
   - `shell/dock.cpp`：磨砂底部 Dock 布局、悬停缩放动画、拖拽排序与运行态角标。
   - `shell/overlays.cpp`：控制中心 Bento 面板、Spotlight 全局启动器、开机 Logo 动画、硬件音量/亮度 OSD Pill。
3. **升级 IPC 路由与拆分前端 UI JS 模块**
   - 引入 `ipc_router.hpp` / `ipc_router.cpp`：建立表驱动指令路由与兼容现有纯字符串指令的 JSON-RPC 请求/参数解析（`{"id", "method", "params"}` + legacy fallback），为后续扩展插件、托盘及 AI Agent 工具桥做准备。
   - 将 `desktop/ui/app.js` 拆分为面向 `file://` 安全加载的 `desktop/ui/js/` 模块（`bridge.js`、`games_store.js`、`settings.js`、`installer_welcome.js`）。
   - 全量纳入 `CMakeLists.txt` 与 `spinel-tests` 回归测试。

---

## 第二步：主力办公（Office & Daily Productivity）底座补齐

1. **桌面系统托盘（StatusNotifierItem / SNI）**
   - 在 `spinel-shell` 顶栏右侧实现 D-Bus `org.kde.StatusNotifierWatcher` / 托盘宿主，承接微信、飞书、Telegram、系统代理客户端、OBS 等后台最小化图标与右键菜单。
2. **软件商店与软件包扩充（Office 套件）**
   - `app_store.cpp` 目录新增并支持一键安装：LibreOffice / OnlyOffice / WPS（视可用源）、PDF 阅读/批注、Obsidian / 笔记工具，并在 `packages.custom.x86_64` 按需补齐 `cups` + `system-config-printer` 打印基础设施。
3. **桌面与多窗口生产力打磨**
   - 顶栏增加清晰的多工作区（1..4）指示/点击切换控件；优化 `Alt+Tab` 窗口切换提示与多屏窗口归属体验。
   - 修复实机/VM 回归中发现的细节（已完成 Spotlight、自定义壁纸、OSD、多屏、OOBE、Doctor 的实机验收闭环）。

---

## 第三步：AI（Coding & AI Agent OS）原生能力

1. **Developer & Sandbox 底座**
   - 应用商店与系统集成：`podman`（Rootless 容器优先）/ `docker`、`git`、`python`、`nodejs`、VS Code / Cursor、本地模型运行时（Ollama / llama.cpp）。
   - **安全执行闭环**：在系统设置与控制中心接入 **Btrfs Snapper 快照管理界面**，支持 AI Coding Agent 修改系统级配置或大版本更新前自动创建 `@snapshots` 还原点。
2. **受控桌面能力总线（OS Agent Bus / Local MCP Bridge）**
   - 基于第一步的结构化 `ipc_router`，向受信任的本地 Coding / AI Agent 暴露权限门控的 Unix Socket / D-Bus / MCP 接口：
     - 只读感知：当前聚焦应用、窗口列表、最近截图、剪贴板/通知摘要、Spinel Doctor 系统健康度；
     - 受控执行：应用启动、分屏布局、工作区切换、代理/性能预设切换（集成权限白名单与用户弹窗确认）。
3. **Game ↔ AI GPU 显存与算力协同**
   - 在控制中心增加「AI 算力 / 显存模式」与与 GameMode 联动的显存仲裁机制：启动大型游戏或 Gamescope 主机模式前，可自动暂停/卸载本地推理服务释放 VRAM，退出游戏后恢复开发模式。
4. **Spotlight 升级为 AI Command Palette**
   - `Alt + Space` 保留零延迟本地应用/游戏/设置检索；支持输入自然语言或 `@agent` 前缀直接调用本地/云端 Agent 执行桌面任务或诊断日志异常。

---

## 第四步：Game 持续打磨与发布规范

- 完成真实物理机（AMD / Intel / NVIDIA RTX & 老架构兼容说明）多屏高刷、VRR、手柄大屏及存档备份导入/导出闭环。
- 补充 `.github/workflows/ci.yml`（每次 Push/PR 自动运行 C++ 编译与 `ctest` 单元测试）并同步构建与发版流程。
