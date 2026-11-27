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

## 第三步：AI（Coding & AI Agent OS）原生能力（详案见 [AI_OS_PLAN.md](AI_OS_PLAN.md)）

1. **已有基础设施底座**
   - 应用中心已集成 `podman` / `docker`、`git`、`python`、`nodejs`、VS Code、`ollama`、`vllm`、`llama.cpp`，系统设置已接入 Btrfs Snapper 快照、平行沙箱（`spinel-sandbox` + PiP 画中画）与 Cloud Relay 跨端串流。
2. **Agent 会话「灵动岛」与三源感知（Phase 1）**
   - 在 `spinel-shell` 顶栏构建原生 **Agent Island（灵动岛）**，融合 `$XDG_RUNTIME_DIR/spinelos/agents/*.json`、`spinel-agent-ctl` Hook 上报与 `/proc` + `window_tracker` PID 绑定，统一监管 Codex、Antigravity、Claude、DeepSeekHarness 等活动会话，任务完成/待审批时灵动岛即时展开提醒并支持一键切窗/进沙箱。
3. **原生 Task Inbox 任务派单箱（Phase 2）**
   - 在灵动岛面板接入原生待办（`backlog -> in_progress -> review -> done`），支持随手速记并一键派给终端/沙箱 Agent 执行，完成后自动高亮提示验收。
4. **系统级 `spinel-os-mcp` 与 4 级权限闸门（Phase 3–4）**
   - 提供轻量独立 `spinel-os-mcp` 网关，按 Tier 0–3 权限对外暴露桌面上下文感知（窗口/剪贴板/截图/Doctor 日志）、Task Inbox 协同、Btrfs 快照保护、平行沙箱执行与灵动岛交互式审批，并在游戏模式下自动仲裁 `ollama` / `vllm` 显存。
   - 完整技术方案、数据结构与分阶段验收标准见 [docs/AI_OS_PLAN.md](AI_OS_PLAN.md)。

---

## 第四步：Game 持续打磨与发布规范

- 完成真实物理机（AMD / Intel / NVIDIA RTX & 老架构兼容说明）多屏高刷、VRR、手柄大屏及存档备份导入/导出闭环。
- 补充 `.github/workflows/ci.yml`（每次 Push/PR 自动运行 C++ 编译与 `ctest` 单元测试）并同步构建与发版流程。
