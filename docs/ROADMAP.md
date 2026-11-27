# Spinel OS 演进路线图与架构重构规划

> **定位目标**：将 Spinel OS 从“轻量级 Wayland 桌面发行版（Alpha）”演进为可长期作为日常主力的 **Office（办公） + AI（Coding & AI Agent OS） + Game（游戏）三合一桌面操作系统**。

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

## 第一步：模块化拆分与结构化 IPC（工程地基 · 已完成）

1. **拆分 `system_control` 到 `desktop/src/sys/` 按域聚合**
   - `sys/network.cpp`、`sys/display_input.cpp`、`sys/audio_power.cpp`、`sys/devices.cpp`、`sys/doctor_logs.cpp`。
2. **拆分 `native_shell` 到 `desktop/src/shell/` 原生壳层模块**
   - `shell/shell_core.cpp`、`shell/top_bar.cpp`、`shell/sni_tray.cpp`、`shell/desktop_monitors.cpp`、`shell/dock.cpp`、`shell/overlays.cpp`、`shell/sandbox_pip.cpp`。
3. **升级 IPC 路由与拆分前端 UI JS 模块**
   - 引入 `ipc_router.hpp` / `ipc_router.cpp` JSON-RPC 分发表（`{"id", "method", "params"}` + legacy fallback）并纳入 `spinel-tests` 单测。
   - 拆分 `desktop/ui/js/` 模块（`bridge.js`、`games_store.js`、`settings.js`、`installer_welcome.js`）。

---

## 第二步：主力办公与日常桌面底座补齐（主干已落地，持续微调）

1. **桌面系统托盘（StatusNotifierItem / SNI）**
   - 已在 `shell/sni_tray.cpp` 与顶栏实现 D-Bus `org.kde.StatusNotifierWatcher` / Host、`DBusMenu`（勾选/单选/子菜单）与 `NeedsAttention` 徽标。
2. **应用中心与游戏中心拆分 + 办公套件**
   - 独立 **「Spinel 应用中心」**（专属 Logo、应用市场 + 我的应用启动台）与 **「Spinel 游戏中心」**（Steam 游戏库 + 单游戏调优 + 缓存清理）。
   - 应用中心覆盖 LibreOffice、Papers PDF、Xournal++ PDF 批注、CUPS + PDF 虚拟打印、Obsidian、Thunderbird 邮件日程及原生浏览器。
3. **多工作区与窗口生产力**
   - 顶栏工作区指示与滚轮/快捷键切换（`Super + 1~4` / `Super + Shift + 1~4` / `Ctrl + Alt + ←/→`），`Alt+Tab` 窗口切换器支持直接 `Delete` 关窗，顶栏动态窗口控制菜单。

---

## 第三步：AI（Agent-Native OS 底座）原生能力（详案见 [AI_OS_PLAN.md](AI_OS_PLAN.md)）

> **策略定位**：不自研独立 Coding Agent，专注做全生态商业/自研 Agent（Codex、Antigravity、Claude、DeepSeekHarness 等）的最佳操作系统宿主，按 **Atoms First（Stage 1 → 2 → 3）** 顺序推进。

1. **已有基础设施底座**
   - 应用中心已集成 `podman` / `docker`、`git`、`python`、`nodejs`、VS Code、`ollama`、`vllm`、`llama.cpp`，系统设置已接入 Btrfs Snapper 快照、平行沙箱（`spinel-sandbox` + PiP 画中画）与 Cloud Relay 跨端串流。
2. **Stage 1：基础原子能力 + CLI + 单元测试**
   - 构建 `agent_perms`（Tier 0–3 分级权限与审计）、`agent_bus`（Unix Socket 控制总线与异步审批原语）、`agent_tracker`（三源会话注册表 + PID/窗口绑定）、`task_inbox`（原生待办派单状态机）、OS 无头原子封装（桌面上下文/截图/Doctor/快照/沙箱）与 `spinel-agent-ctl` CLI，全部纳入 `spinel-tests`。
3. **Stage 2：协议出口层 (`spinel-os-mcp`)**
   - 提供轻量独立 `spinel-os-mcp`（`stdio`）网关，把 Stage 1 原子能力按权限暴露给所有支持 MCP 的外部 Agent 接入与联调。
4. **Stage 3：桌面业务展示与协同闭环**
   - 在 `spinel-shell` 顶栏构建原生 **Agent 灵动岛**（会话聚合、一键跳窗/进沙箱 PiP、Tier 3 交互审批卡片），并在「系统设置 → 权限与安全」与「系统监视器 → 系统日志」接入分级权限配置、MCP 接入生成与审计日志查看，联动 Cloud Relay 跨端协同。
   - 完整技术方案、数据结构与验收标准见 [docs/AI_OS_PLAN.md](AI_OS_PLAN.md)。
