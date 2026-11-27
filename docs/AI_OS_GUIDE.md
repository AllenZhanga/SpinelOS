# Spinel AI OS 操作与验证手册 (`AI_OS_GUIDE`)

本手册面向 SpinelOS 用户与开发者，说明如何管理 **AI Agent 会话感知、灵动岛、Task Inbox、`spinel-os-mcp`、分级权限审计、GameMode 显存协同与远程连续性**，以及如何通过 `spinel-agent-ctl doctor` 快速自检整条链路。

---

## 一、图形化管理入口

1. **顶栏「灵动岛」胶囊**（位于顶栏右侧系统图标栏旁）
   - **状态显示**：
     - `◆ AI`：空闲待命
     - `● Agent` / `● N 运行中`：正在运行的 Coding Agent（自动来自 `/proc` 探针、`spinel-agent-ctl` 上报或 `spinel-os-mcp`）
     - `⚠ N 待确认`：存在 `waiting_user` 会话或 **Tier 3 交互审批请求**
     - 状态跃迁至完成/失败/待确认时，胶囊会横向展宽约 3.8 秒显示结论摘要。
   - **点击展开浮窗**：
     1. **待人工审批卡片（Tier 3）**：支持 **[允许]**、**[快照后允许]**（自动先创建 Btrfs 保护点再放行）、**[沙箱]**、**[拒绝]**。
     2. **活动 AI 会话**：点击普通会话直接跳转到对应工作区与窗口；点击沙箱会话直接弹出 `spinel-sandbox` 画中画接管。
     3. **底部快捷操作栏**：提供 **[复制当前现场]**（打包当前窗口/工作区与剪贴板）与 **[权限与审计]** 入口。

2. **系统设置 →「权限与安全」**（按 3 个独立子页签分组展示）
   - **AI Agent 权限**：分项切换 Tier 0–2 开关与 **Tier 3 审批策略**（`ask` / `allow` / `deny`）、一键复制 **Antigravity / Codex / Claude Code / DeepSeekHarness / Gemini CLI** 的 `spinel-os-mcp` 配置，以及快捷跳转至审计日志。
   - **平行沙箱桌面**：控制沙箱启停、网络/剪贴板开关、持久化目录与自启应用白名单。
   - **系统与软件授权**：管理管理员密码弹窗 (Polkit)、XDG 隐私弹窗记录与第三方应用权限。

3. **系统监视器 →「系统日志」→「AI / MCP 审计」**
   - 实时查看或清空 `~/.local/state/spinelos/mcp_audit.log` 审计记录（包含每次能力调用来源、动作与裁决）。

---

## 二、常用配置文件与运行状态路径

| 用途 | 路径 | 说明 |
| :--- | :--- | :--- |
| 权限配置文件 | `~/.config/spinelos/agent_perms.conf` | Tier 0–3 开关、`vram_yield_on_gamemode` |
| Task Inbox 存储 | `~/.local/share/spinelos/tasks.json` | `backlog → in_progress → review → done` 持久化 |
| MCP 审计日志 | `~/.local/state/spinelos/mcp_audit.log` | 每次能力调用来源、动作、裁决、时间 |
| `AF_UNIX` 调度总线 | `$XDG_RUNTIME_DIR/spinelos/agent.sock` | `spinel-shell` 常驻暴露的权限仲裁通道 |
| 会话 Drop-in 目录 | `$XDG_RUNTIME_DIR/spinelos/agents/*.json` | 各 Agent / `wrap` 写入的实时状态文件 |
| 无头截图暂存 | `$XDG_RUNTIME_DIR/spinelos/captures/` | `screenshot_capture` 输出 PNG（自动保留最近 20 张） |

---

## 三、一键自检 (`spinel-agent-ctl doctor`)

部署到桌面后，在终端直接执行：

```bash
spinel-agent-ctl doctor
```

或输出 JSON 格式供脚本消费：

```bash
spinel-agent-ctl doctor --json
```

自检覆盖 8 个关键节点：`agent_bus` Socket 连通性、`desktop_context` 合成器窗口/工作区钩子、`spinel-os-mcp` stdio 握手与 18 工具声明、`agent_tracker` 目录与 `/proc` 探针、`task_inbox` 读写、`perms_audit` 配置与日志、`os_primitives`（`grim` / `bwrap` / `spinel-sandbox` / `snapper`）及本地服务（`ollama` / `spinel-vllm` / `docker`）。

同时也可在桌面打开 **「Spinel Doctor」** 查看「**AI OS 总线与 MCP 网关**」体检条目。

---

## 四、`spinel-agent-ctl` 常用命令速查

```bash
# 1. 连通性与当前工作现场
spinel-agent-ctl ping
spinel-agent-ctl context

# 2. 会话状态上报与查看
spinel-agent-ctl report --agent Codex --status running --id s1 --title "重构建模" --summary "修改接口中"
spinel-agent-ctl report --agent Codex --status done --id s1 --summary "编译单测通过"
spinel-agent-ctl list
spinel-agent-ctl clear            # 清理已结束的 Done/Error 会话

# 3. Task Inbox 待办流转与零侵入命令行包装
spinel-agent-ctl todo add "补齐重试单测" --repo "$PWD" --agent Codex --priority high
spinel-agent-ctl todo list
spinel-agent-ctl wrap --agent Codex --task <task_id> -- <任意命令>
spinel-agent-ctl todo accept <task_id>

# 4. 权限设置与交互审批
spinel-agent-ctl perms
spinel-agent-ctl perms set tier1_clipboard=true tier1_screenshot=true tier3_policy=ask
spinel-agent-ctl approvals
spinel-agent-ctl approve <approval_id> allow   # allow | sandbox | deny

# 5. 生成 MCP 配置片段
spinel-agent-ctl mcp-snippet codex
spinel-agent-ctl mcp-snippet claude
```

---

## 五、`spinel-os-mcp` 开放工具列表（18 项）

- **会话与待办（Tier 0）**：`agent_report`、`agent_list`、`todo_list`、`todo_create`、`todo_claim`、`todo_complete`
- **桌面情境与代理（Tier 0 / Tier 1）**：`desktop_get_context`、`proxy_status`、`clipboard_recent`、`screenshot_capture`（支持 PNG Base64 多模态回传）、`doctor_diagnose`、`logs_tail`
- **安全沙箱与还原点（Tier 0 / Tier 2）**：`sandbox_status`、`sandbox_launch`、`snapshot_checkpoint`
- **宿主机与交互审批（Tier 3）**：`workspace_focus`、`service_control`（`ollama` / `vllm` / `docker` / `cups`，其中 `status` 为 Tier 0）、`request_user_approval`
