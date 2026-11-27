# Spinel AI OS 架构规划与演进路线 (AI_OS_PLAN)

> **核心定位**：不在 `spinel-shell` 中堆砌重型聊天框，而是把 SpinelOS 打造成**以「会话感知 · 沙箱隔离 · 原生待办派单 · 系统级 MCP」为核心的 Agent-Native 操作系统底座**，让 Codex、Antigravity、Claude Code、DeepSeekHarness 等任意 Coding / AI Agent 无缝协同。

---

## 一、设计原则与架构边界

1. **守住常驻壳层轻量底线**：
   - `spinel-shell`（`SPINEL_HAS_WEBKIT=0`）仅承载事件监听、状态聚合与原生 `GTK4 + Cairo` 展示（顶栏灵动岛、展开面板、审批卡片），严禁塞入 WebKitGTK 或常驻大模型推理进程。
2. **不强绑单一 Agent，保持开放接入**：
   - 同时支持**零侵入被动发现**、**Drop-in 状态文件 / CLI Hook 主动上报**、以及**标准 MCP 双向接入**。
3. **重操作、管理台与 MCP 网关进程隔离**：
   - `spinel-agent-ctl`（极轻上报/控制 CLI）与 `spinel-os-mcp`（按需 stdio MCP 网关）独立于 `spinel-shell`，权限页与审计日志按需在 `spinel-webui` 中打开。

---

## 二、总体三层感知与控制架构

```text
  [Codex / Antigravity / Claude / DeepSeekHarness / 本地脚本]
         │                    │                     │
   (1) 进程/窗口/目录     (2) Hook / CLI        (3) 标准 MCP (stdio)
     零侵入自动发现        spinel-agent-ctl      spinel-os-mcp (按需拉起)
         │                    │                     │
         ▼                    ▼                     ▼
  ┌──────────────────────────────────────────────────────────┐
  │       会话聚合与权限仲裁层 (Agent Tracker + Tier Gate)      │
  │  • $XDG_RUNTIME_DIR/spinelos/agents/*.json (inotify)     │
  │  • ~/.local/share/spinelos/tasks.json (原生 Task Inbox)  │
  │  • ~/.config/spinelos/agent_perms.conf (Tier 0~3 权限)   │
  └──────────────┬───────────────────────────┬───────────────┘
                 │ D-Bus / Unix Socket       │ 复用 ipc_router JSON-RPC
                 ▼                           ▼
  ┌──────────────────────────┐  ┌────────────────────────────┐
  │   spinel-shell (常驻)     │  │  OS 能力执行面 (已有底座)   │
  │ • 顶栏「灵动岛」Pill/展开态 │  │ • window_tracker 跳窗/工作区 │
  │ • 展开 Popover (会话+待办)│  │ • spinel-sandbox + PiP 接管│
  │ • 审批卡片 [允许/沙箱/拒绝] │  │ • Snapper Btrfs 快照        │
  └──────────────────────────┘  │ • Doctor/日志/截图/服务启停 │
                                └────────────────────────────┘
```

---

## 三、核心模块详细设计

### 1. 会话采集与窗口绑定（`agent_tracker`）
统一维护运行中与近期完成的 Agent 会话列表：
- **核心字段**：`id`、`agent`、`title`、`repo_path`、`pid`、`status`（`running | waiting_user | done | error`）、`summary`、`win_id`、`workspace_idx`、`in_sandbox`、`task_id`、`approval_id`、`updated_at`。
- **三源融合策略**：
  1. **Drop-in 状态文件**：`GFileMonitor` 监听 `$XDG_RUNTIME_DIR/spinelos/agents/<session_id>.json`（由 Agent、Wrapper 或 `DeepSeekHarness` 写入）。
  2. **CLI / Hook 上报 (`spinel-agent-ctl`)**：通过 D-Bus（`org.spinelos.shell`）或 Unix Socket 上报里程碑事件（适用于 Codex / Claude / Antigravity / 自定义脚本的 Stop & Notification Hooks）。
  3. **进程与窗口兜底扫描**：低频扫描 `/proc/<pid>` 匹配已知 Agent 进程与工作目录 `cwd`，通过父进程链绑定 `wlr-foreign-toplevel` 窗口句柄与 `spinel-sandbox` 沙箱状态；进程退出且超时未刷新时自动归档或清理。

### 2. 顶栏「灵动岛」交互（`top_bar.cpp` + `overlays.cpp`）
- **常态胶囊**：驻留顶栏右侧紧凑区域，仅展示活跃计数与状态点（例如 `● 2 运行中`；若存在 `waiting_user` 则高亮琥珀色呼吸提示）。
- **事件瞬时展开**：状态跃迁至 `done` / `error` / `waiting_user` 时，胶囊横向平滑展宽约 3–4 秒显示一行结论摘要与快捷动作（`✓ DeepSeekHarness · 单测通过 · [查看]`），随后收起保留轻角标；全屏游戏（`fullscreen_hidden`）时自动静默。
- **点击展开双段 Popover**：
  - **上部「活动会话」**：按 `waiting_user > running > done/error` 排序，点击直接切到对应工作区并激活窗口（沙箱任务直接唤起 PiP 画面）。
  - **下部「待办派单箱」**：展示近期未安排事项，支持一行速记与一键派发。

### 3. 原生 Task Inbox（待办与派单闭环，`task_inbox`）
将碎片想法与多 Agent 会话打通，而非传统静态待办清单：
- **生命周期**：`backlog（待安排）` → `in_progress（绑定会话执行中）` → `review（完成待验收）` → `done`
- **本地存储**：`~/.local/share/spinelos/tasks.json`（原子写入，零额外数据库依赖，纳入 `spinel-tests` 单元测试）。
- **一键派单与回写**：支持指定关联项目目录并拉起 `spinel-terminal` 或 `spinel-sandbox` 传入 `SPINEL_TASK_ID`；绑定会话完成后自动将该待办转为 `review` 并通过灵动岛提醒验收。

### 4. `spinel-os-mcp` 本地能力网关
以轻量 C++20 独立二进制提供标准 **MCP (`stdio`)**，复用 `ipc_router` 的结构化分发思想，暴露 OS 独有能力而非重复造 `bash` 轮子：
- **桌面情境感知**：`desktop_get_context`（活跃窗口/工作区/通知摘要）、`clipboard_recent`、`screenshot_capture`（窗口截图多模态回传）、`doctor_diagnose`、`logs_tail`。
- **任务与状态中枢**：`agent_report`、`todo_list` / `todo_claim` / `todo_complete`（跨 Agent 接力共享上下文）。
- **安全隔离与回滚**：`snapshot_checkpoint`（仅创建 Btrfs Snapper 还原点，禁止静默删/回滚）、`sandbox_launch` / `sandbox_status`（浏览器测试与高风险运行自动进平行沙箱）。
- **协同与算力控制**：`workspace_focus`、`service_control`（`ollama` / `vllm` / `docker` / `cups`）、`proxy_status`、`request_user_approval`。

### 5. 四级权限闸门与审计（`~/.config/spinelos/agent_perms.conf`）
在「系统设置 → 权限与安全」配置权限级别，并记录 `~/.local/state/spinelos/mcp_audit.log`：
- **Tier 0（默认开放）**：Agent 状态上报、Todo 读写、只读系统概况与当前应用名。
- **Tier 1（按项勾选）**：剪贴板历史、窗口截图、Doctor 体检与脱敏日志读取。
- **Tier 2（推荐默认开启）**：在 `spinel-sandbox` 平行桌面执行任务、创建 Btrfs 保护快照。
- **Tier 3（灵动岛交互审批）**：启停系统服务、改代理/网络、宿主机窗口重排，调用时在灵动岛弹出 `[允许 / 先快照并允许 / 在沙箱执行 / 拒绝]`，不抢终端焦点。

---

## 四、分阶段实施路线图

| 阶段 | 交付目标 | 主要涉及模块 | 验收标准 |
| :--- | :--- | :--- | :--- |
| **Phase 1：灵动岛感知 MVP** | 顶栏实时显示 Codex / Antigravity / DeepSeekHarness 等活动会话，完成/待确认时灵动岛展开提示并支持点击跳窗 | `agent_tracker`、`spinel-agent-ctl`、`top_bar.cpp`、`window_tracker.cpp`、`spinel_tests.cpp` | 状态文件或 CLI 上报后顶栏实时联动，点击直接跳转对应工作区与窗口/沙箱 |
| **Phase 2：原生 Todo 派单箱** | 灵动岛弹出面板接入 Task Inbox，支持随手记、关联项目目录、一键派发到终端/沙箱并在完成后转待验收 | `task_inbox`、`top_bar.cpp`、`overlays.cpp`、`ipc_router.cpp` | 新建 Todo → 一键启动终端/沙箱 Agent → 会话结束后灵动岛高亮提醒验收 |
| **Phase 3：`spinel-os-mcp` + 权限中心** | 交付 `spinel-os-mcp`（开放 Tier 0~2 工具），在设置「权限与安全」接入分级开关与审计查看 | `spinel_os_mcp.cpp`、`CMakeLists.txt`、`desktop/ui/`、四端同步脚本 | 主流 MCP 客户端配置后可直接认领 Todo、读取桌面上下文/截图、创建快照与调用沙箱 |
| **Phase 4：灵动岛交互审批与算力联动** | 上线 Tier 3 灵动岛审批卡片，打通 `GameMode` 与 `ollama` / `vllm` 显存仲裁 | `ipc_router.cpp`、`overlays.cpp`、`app_store.cpp`、`audio_power.cpp` | 高风险动作在顶栏直接点批；启动全屏游戏时自动协调卸载/暂停本地重型推理服务 |
