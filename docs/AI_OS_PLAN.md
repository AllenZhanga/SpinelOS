# Spinel AI OS 架构规划与演进路线 (AI_OS_PLAN)

> **核心定位**：**不自研通用 Coding Agent**（商业与社区 Agent 迭代极快），而是把 SpinelOS 打造成**全生态 Agent 的最佳操作系统宿主**——提供商业软件自身无法提供的 OS 底层原语：全局会话感知、沙箱隔离与 PiP 接管、Btrfs 快照兜底、原生 Task Inbox 派单与分级系统 MCP，让 Codex、Antigravity、Claude Code、DeepSeekHarness 等任意工具无缝接入。

---

## 一、设计原则与架构边界

1. **不做独立 Agent，专注做开放 OS 宿主平台**：
   - 不与商业 Coding Agent 竞争推理/编码循环；通过「进程探针 + CLI Hook + 标准 MCP」三源开放接入，谁强接谁。
2. **守住常驻壳层轻量底线**：
   - `spinel-shell`（`SPINEL_HAS_WEBKIT=0`）仅承载事件监听、状态聚合与原生 `GTK4 + Cairo` 展示（顶栏灵动岛、展开面板、审批卡片），严禁塞入 WebKitGTK 或常驻大模型推理进程。
3. **Atoms First（先底层原子、再协议桥、后桌面 UI）**：
   - 先把无 UI 依赖的核心状态机、权限审计总线和无头 OS 原语打牢并纳入 `spinel-tests` 单元测试，再暴露给 `spinel-os-mcp`、顶栏灵动岛和 `spinel-webui` 消费，保证多端复用且架构稳固。
4. **重操作、管理台与 MCP 网关进程隔离**：
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
  - **上部「待人工审批 / 活动会话」**：展示 Tier 3 审批卡片与按 `waiting_user > running > done/error` 排序的会话列表，点击直接切到对应工作区与窗口（沙箱任务直接唤起 PiP 画面）。
  - **底部极简操作栏**：提供 **[复制当前现场]** 与 **[权限与审计]** 快捷入口。

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

### 6. 跨应用现场、端侧轻守夜人与跨设备连续性
- **工作现场打包（Context Fabric）**：`os_context` 统一采集当前活跃窗口/工作区、聚焦终端/IDE 工作目录（`cwd`/Git 根）、剪贴板与报错通知摘要、按需截图与 Doctor 日志；支持通过 MCP 拉取或快捷键一键打包传递给新建/活跃 Agent。
- **端侧轻守夜人（规则 + 按需本地小模型协同）**：不常驻吃显存大模型，依托按需启停的 `ollama` / `llama.cpp` 轻量模型或确定性摘要规则，负责把长输出压缩为灵动岛一句话结论、给零碎 Todo 自动关联仓库、`Alt+Space` 跨应用检索，并与 `GameMode` 联动在全屏游戏时自动让出 VRAM。
- **跨设备连续性（Cloud Relay 扩展）**：通过现有 Go Cloud Relay + E2EE 信道与 macOS 客户端，把本机 `agent_tracker` 进度、灵动岛完成/待审批事件同步到远程设备，支持异地一键审批并直接通过 PiP 接管平行沙箱继续操作。

---

## 四、五大核心跃迁与实施计划映射表

确保「真 AI OS」的 5 个核心跃迁在工程推进中一个不丢：

| 真 AI OS 关键跃迁 | 核心能力落点 | 对应实施阶段 |
| :--- | :--- | :--- |
| **1. 从「管理窗口」转向「管理任务与会话」** | `agent_tracker`（三源会话注册表 + PID 绑窗）+ `task_inbox`（`backlog→review→done` 派单闭环）+ 顶栏「灵动岛」主动提示与跳窗 | **Stage 1**（原子状态机）→ **Stage 2**（`todo_*` / `agent_report`）→ **Stage 3**（灵动岛 + Todo 面板） |
| **2. 跨应用「工作现场总线 (Context Fabric)」** | `os_context` 无头采集当前窗口/工作区/仓库 `cwd`/剪贴板/通知/截图/Doctor 日志，打包直喂 Agent | **Stage 1**（`os_context` 原子）→ **Stage 2**（`desktop_get_context` 等 MCP）→ **Stage 3**（Tier 1 开关与一键喂现场） |
| **3. 「默认沙箱 + 人类接管 + Btrfs 快照」OS 原语** | `guarded_actions` + `spinel-sandbox` 隔离跑 GUI/网页测试 + PiP 实时人工接管 + 改前建 Snapper 快照 + Tier 3 审批 | **Stage 1**（`guarded_actions` + `approval_gate`）→ **Stage 2**（`snapshot_checkpoint` / `sandbox_launch`）→ **Stage 3**（灵动岛审批卡片） |
| **4. 「端侧轻守夜人」与「云端强主力」算力分工** | 本地规则/轻小模型负责日志摘要、Todo 归类、语义检索；商业/自研重 Agent 负责编码；`GameMode` 启动时自动协调释放显存 | **Stage 1**（服务控制与状态查询原子）→ **Stage 2**（`service_control` MCP）→ **Stage 3**（Game/VRAM 显存仲裁 + Spotlight/摘要联动） |
| **5. 跨设备连续性（个人全天候 Agent 节点）** | 通过现有 `relay_client` + Cloud Relay + macOS 端同步会话完成/审批推送，异地直接 PiP 接管沙箱现场 | **Stage 1**（统一 `agent_bus` 事件流）→ **Stage 3**（Relay 事件桥接与远程审批/PiP 联动） |

---

## 五、分阶段实施路线图（Atoms First：Stage 1 → 2 → 3）

按「**先底层原子原语 → 再标准协议出口 → 最后桌面业务交互**」顺序推进，确保架构稳定、单测先行、各端复用同一套事实来源：

| 阶段 | 交付目标 | 核心原子 / 涉及模块 | 验收标准 |
| :--- | :--- | :--- | :--- |
| **Stage 1：基础原子层 + CLI + 单元测试**<br>*(无 UI 改动)* | 打牢 4 组纯 C++ 原子能力与统一接入总线，在 `spinel-tests` 中完成并发、超时回收、状态联动与权限拦截测试 | • `agent_perms`（Tier 0–3 规则 + 审计日志 `mcp_audit.log`）<br>• `agent_bus`（`$XDG_RUNTIME_DIR/spinelos/agent.sock` + 异步审批原语）<br>• `agent_tracker`（状态目录监听 + `/proc` 探针 + PID/窗口绑定）<br>• `task_inbox`（`tasks.json` 原子读写与 `backlog→review` 状态机）<br>• `os_context` / `guarded_actions` 无头原子封装<br>• `spinel-agent-ctl` 上报与派单 CLI | `spinel-tests` 全量单测覆盖状态跃迁、PID 回收、Task 联动与 Tier 0–3 鉴权；终端用 `spinel-agent-ctl` 可直接完成会话上报、Todo 读写与权限校验 |
| **Stage 2：协议出口层 (`spinel-os-mcp`)** | 交付轻量独立的 C++20 MCP（`stdio`）网关，将 Stage 1 原子能力透出给所有支持 MCP 的商业/自研 Agent | • `spinel_os_mcp.cpp`、`CMakeLists.txt`<br>• 开放工具：`agent_report`、`todo_*`、`desktop_get_context`、`clipboard_recent`、`screenshot_capture`、`doctor_diagnose`、`logs_tail`、`snapshot_checkpoint`、`sandbox_launch`、`service_control`、`request_user_approval`<br>• 四端打包同步（`release-ota.sh` / `dev-deploy.sh` / `spinel-update` / `spinel-install`） | 在 Codex / Antigravity / Claude / DeepSeekHarness 配上一行 `spinel-os-mcp` 后，可真实调用 Todo 认领、窗口截图回传、Doctor 诊断、Btrfs 快照与平行沙箱 |
| **Stage 3：业务展示与协同闭环**<br>*(原生桌面 UI + 跨端联动)* | 在稳定的原子与协议底座之上构建轻量桌面交互、灵动岛、审批、权限设置与跨端协同 | • `top_bar.cpp` + `overlays.cpp`：顶栏「灵动岛」Pill/展宽动效、活动会话与 Task Inbox 展开 Popover、一键切窗/进沙箱 PiP、Tier 3 审批卡片<br>• `desktop/ui/`：设置「权限与安全」Tier 0–3 分级开关与审计记录查看<br>• `audio_power.cpp` / `app_store.cpp` / `relay_client.cpp`：GameMode ↔ `ollama`/`vllm` 显存仲裁、Cloud Relay 远程审批通知 | Agent 完成或待确认时灵动岛即时展宽提示并支持点击跳窗；高危 MCP 动作可在顶栏直接点批；游戏时自动协调本地重推理服务；跨端客户端可接收进度 |
