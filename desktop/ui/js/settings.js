(() => {
  const UI = window.SpinelUI;
  const { state, sendCommand, showFeedback, enhanceSelect } = UI;

  let activeLogSource = "apps";
  let procTimerId = null;
  let lastProcPayload = null;
  let procSortBy = "cpu";
  let currentSettingsTab = "";

  function switchSettingsTab(tab) {
    const titles = {
      wifi: "Wi-Fi 与网络",
      bt: "蓝牙手柄",
      relay: "云接力与远程沙箱",
      display: "显示器",
      devices: "设备管理器",
      audio: "声音与电源",
      shortcuts: "键盘与快捷键",
      perf: "游戏性能",
      appearance: "外观与输入法",
      updates: "系统更新",
      perms: "权限与安全",
      snapshots: "系统快照 (Snapper)"
    };
    const tabs = Object.keys(titles);
    const target = tabs.includes(tab) ? tab : "wifi";
    if (currentSettingsTab === "audio" && target !== "audio") {
      sendCommand("settings:audio-test-mic:stop");
      sendCommand("settings:audio-test-speaker:stop");
    }
    currentSettingsTab = target;
    tabs.forEach((t) => {
      const body = document.getElementById(`tab-${t}`);
      if (body) body.classList.toggle("hidden", t !== target);
    });
    const titleEl = document.getElementById("settings-page-title");
    if (titleEl) titleEl.textContent = titles[target] || "系统设置";
    document.querySelectorAll("#app-settings [data-cmd^='settings:tab:']").forEach((btn) => {
      btn.classList.toggle("active", btn.getAttribute("data-cmd") === `settings:tab:${target}`);
    });
    if (target === "wifi") sendCommand("settings:wifi-list");
    else if (target === "bt") sendCommand("settings:bt-status");
    else if (target === "relay") sendCommand("relay:state-get");
    else if (target === "display") sendCommand("settings:display-scan");
    else if (target === "devices") sendCommand("settings:devices-get");
    else if (target === "audio") sendCommand("settings:audio-get");
    else if (target === "shortcuts") sendCommand("settings:shortcuts-get");
    else if (target === "perf") sendCommand("perf:get");
    else if (target === "appearance") {
      sendCommand("theme:refresh");
      sendCommand("dock:config-get");
      sendCommand("ime:status");
    }
    else if (target === "updates") {
      sendCommand("ota:check");
      sendCommand("store:refresh");
    }
    else if (target === "perms") sendCommand("settings:perms-get");
    else if (target === "snapshots") sendCommand("settings:snapshots-get");
  }

  function switchMonitorTab(tab, logSource) {
    const target = tab === "logs" ? "logs" : "procs";
    if (logSource) activeLogSource = logSource;
    const procsView = document.getElementById("monitor-view-procs");
    const logsView = document.getElementById("monitor-view-logs");
    if (procsView) procsView.classList.toggle("hidden", target !== "procs");
    if (logsView) logsView.classList.toggle("hidden", target !== "logs");

    const titleEl = document.getElementById("monitor-page-title");
    if (titleEl) titleEl.textContent = target === "logs" ? "系统监视器 · 系统日志" : "系统监视器 · 进程监控";

    document.querySelectorAll("#monitor-tab-group [data-monitor-tab]").forEach((btn) => {
      btn.classList.toggle("active", btn.getAttribute("data-monitor-tab") === target);
    });

    if (procTimerId) {
      clearInterval(procTimerId);
      procTimerId = null;
    }
    if (target === "procs") {
      sendCommand("monitor:procs-get");
      procTimerId = setInterval(() => sendCommand("monitor:procs-get"), 2000);
    } else {
      sendCommand(`monitor:logs:${activeLogSource}`);
    }
  }

  document.querySelectorAll("#monitor-tab-group [data-monitor-tab]").forEach((btn) => {
    btn.addEventListener("click", () => {
      switchMonitorTab(btn.getAttribute("data-monitor-tab") || "procs");
    });
  });

  window.__spinelOpenSettings = (tab) => switchSettingsTab(tab);
  window.__spinelOpenMonitor = (tab, logSource) => switchMonitorTab(tab, logSource);

  /* ---------------- Dock Display Mode ---------------- */
  const dockModeDescs = {
    magnify: "悬浮放大（自适应间距防拥挤，底部常驻）",
    fixed: "固定尺寸（不放大，仅悬停显示应用名称）",
    autohide: "不悬浮默认隐藏（释放屏幕底部空间，鼠标移至底边自动滑出）"
  };

  window.__spinelDockConfig = (cfg) => {
    const mode = (cfg && cfg.mode) || "magnify";
    document.querySelectorAll("#dock-mode-group [data-dock-mode]").forEach((btn) => {
      btn.classList.toggle("active", btn.getAttribute("data-dock-mode") === mode);
    });
    const descEl = document.getElementById("dock-mode-desc");
    if (descEl) descEl.textContent = dockModeDescs[mode] || dockModeDescs.magnify;
  };

  document.querySelectorAll("#dock-mode-group [data-dock-mode]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const mode = btn.getAttribute("data-dock-mode") || "magnify";
      sendCommand(`dock:config-set:${mode}`);
    });
  });

  /* ---------------- Btrfs Snapper Snapshots ---------------- */
  let snapSudoNopass = false;
  let pendingSnapOp = null;

  function closeSnapModal() {
    pendingSnapOp = null;
    document.getElementById("snap-pass-modal")?.classList.add("hidden");
  }

  function executeSnapOp(op, password = "") {
    if (!op) return;
    if (op.action === "create") {
      sendCommand(`settings:snapshot-create\ndesc=${op.desc || "SpinelOS 手动还原点"}\npassword=${password}`);
    } else if (op.action === "rollback") {
      sendCommand(`settings:snapshot-rollback\nid=${op.id}\npassword=${password}`);
    } else if (op.action === "delete") {
      sendCommand(`settings:snapshot-delete\nid=${op.id}\npassword=${password}`);
    }
  }

  function requestSnapOp(op) {
    if (snapSudoNopass) {
      executeSnapOp(op, "");
      return;
    }
    pendingSnapOp = op;
    const modal = document.getElementById("snap-pass-modal");
    const title = document.getElementById("snap-pass-title");
    const desc = document.getElementById("snap-pass-desc");
    const input = document.getElementById("snap-pass-input");
    if (title) {
      title.textContent = op.action === "create"
        ? "创建 Btrfs 系统快照"
        : (op.action === "rollback" ? `回滚到系统快照 #${op.id}` : `删除快照 #${op.id}`);
    }
    if (desc) {
      desc.textContent = op.action === "rollback"
        ? "回滚将在下次重启后切换到该快照对应的只读/可写系统子卷，你的用户数据 (/home) 默认保留。"
        : "操作根分区 @snapshots 子卷需要当前用户 sudo 管理员权限。";
    }
    if (input) input.value = "";
    modal?.classList.remove("hidden");
    input?.focus();
  }

  document.getElementById("btn-snap-create")?.addEventListener("click", () => {
    const inp = document.getElementById("snap-desc-input");
    const desc = (inp?.value || "").trim() || "SpinelOS 手动还原点";
    requestSnapOp({ action: "create", desc });
  });

  document.getElementById("btn-snap-pass-cancel")?.addEventListener("click", closeSnapModal);
  document.getElementById("snap-pass-modal")?.addEventListener("click", (ev) => {
    if (ev.target.id === "snap-pass-modal") closeSnapModal();
  });
  document.getElementById("btn-snap-pass-confirm")?.addEventListener("click", () => {
    const pass = document.getElementById("snap-pass-input")?.value || "";
    if (!pass && !snapSudoNopass) {
      document.getElementById("snap-pass-input")?.focus();
      return;
    }
    const op = pendingSnapOp;
    closeSnapModal();
    executeSnapOp(op, pass);
  });
  document.getElementById("snap-pass-input")?.addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") {
      ev.preventDefault();
      document.getElementById("btn-snap-pass-confirm")?.click();
    }
  });

  window.__spinelSnapshotsFeedback = (ok, msg, rebootRequired = false) => {
    showFeedback("snap-feedback", rebootRequired ? `${msg}（建议立即重启）` : (msg || ""), !ok);
  };

  window.__spinelSnapshotsData = (data) => {
    if (!data) return;
    snapSudoNopass = Boolean(data.sudo_nopass);
    const chip = document.getElementById("snap-status-chip");
    const createBtn = document.getElementById("btn-snap-create");
    const listEl = document.getElementById("snap-list");
    const snaps = Array.isArray(data.snapshots) ? data.snapshots : [];

    const ready = !data.is_live && data.root_fs === "btrfs" && data.snapper_installed && data.config_ready;
    if (chip) {
      if (data.is_live) chip.textContent = "Live ISO 环境不可用";
      else if (data.root_fs !== "btrfs") chip.textContent = `根分区为 ${data.root_fs || "非 Btrfs"}`;
      else if (!data.snapper_installed) chip.textContent = "未安装 snapper";
      else if (!data.config_ready) chip.textContent = "未初始化 root 快照配置";
      else chip.textContent = `Btrfs 已就绪 · 共 ${snaps.length} 个快照`;
    }
    if (createBtn) createBtn.disabled = !ready;

    if (!listEl) return;
    listEl.innerHTML = "";
    if (!ready) {
      listEl.innerHTML = '<div class="disk-empty">当前仅在已安装到硬盘的 Btrfs 根目录（并配置 snapper root 子卷）中可管理快照。通过 Spinel OS 安装器格式化为 Btrfs 后将自动就绪。</div>';
      return;
    }
    if (snaps.length === 0) {
      listEl.innerHTML = '<div class="disk-empty">暂无历史快照记录，建议点击上方「创建系统快照」保存当前干净状态。</div>';
      return;
    }

    snaps.forEach((s) => {
      const item = document.createElement("div");
      item.className = "disk-item";
      const left = document.createElement("div");
      const title = document.createElement("div");
      title.className = "disk-title";
      title.textContent = `#${s.id} · ${s.description || "系统快照"}${s.is_current ? "（当前运行中）" : ""}`;
      const sub = document.createElement("div");
      sub.className = "disk-sub";
      sub.textContent = `时间: ${s.date || "-"} · 类型: ${s.type || "single"}${s.cleanup ? ` · 策略: ${s.cleanup}` : ""}`;
      left.append(title, sub);

      const actions = document.createElement("div");
      actions.className = "disk-tags";

      const rbBtn = document.createElement("button");
      rbBtn.type = "button";
      rbBtn.className = "btn small primary";
      rbBtn.textContent = "回滚到此状态";
      rbBtn.disabled = Boolean(s.is_current);
      rbBtn.addEventListener("click", () => requestSnapOp({ action: "rollback", id: s.id }));

      const delBtn = document.createElement("button");
      delBtn.type = "button";
      delBtn.className = "btn small danger";
      delBtn.textContent = "删除";
      delBtn.disabled = Boolean(s.is_current);
      delBtn.addEventListener("click", () => requestSnapOp({ action: "delete", id: s.id }));

      actions.append(rbBtn, delBtn);
      item.append(left, actions);
      listEl.appendChild(item);
    });
  };

  /* ---------------- Permissions & Security ---------------- */
  window.__spinelPermsFeedback = (ok, msg) => showFeedback("perms-feedback", msg || "", !ok);
  window.__spinelAiFeedback = (ok, msg) => showFeedback("ai-perms-feedback", msg || "", !ok);

  let activePermsSubtab = "ai";
  function switchPermsSubtab(sub) {
    const allowed = ["ai", "sandbox", "system"];
    activePermsSubtab = allowed.includes(sub) ? sub : "ai";
    allowed.forEach((k) => {
      document.getElementById(`perms-pane-${k}`)?.classList.toggle("hidden", k !== activePermsSubtab);
    });
    document.querySelectorAll("#perms-subtabs [data-perms-tab]").forEach((btn) => {
      btn.classList.toggle("active", btn.getAttribute("data-perms-tab") === activePermsSubtab);
    });
  }
  document.querySelectorAll("#perms-subtabs [data-perms-tab]").forEach((btn) => {
    btn.addEventListener("click", () => switchPermsSubtab(btn.getAttribute("data-perms-tab") || "ai"));
  });
  switchPermsSubtab("ai");

  let lastAiPerms = {
    tier0_observe: true,
    tier1_clipboard: false,
    tier1_screenshot: false,
    tier1_logs: false,
    tier2_sandbox: true,
    tier2_snapshot: true,
    vram_yield_on_gamemode: true,
    tier3_policy: "ask"
  };

  let lastAiMcpClients = {};

  function renderAiMcpSnippet() {
    const pre = document.getElementById("ai-mcp-snippet");
    const sel = document.getElementById("sel-ai-mcp-client");
    if (!pre || !sel) return;
    const t = sel.value || "antigravity";
    if (t === "codex") {
      pre.textContent =
        '[mcp_servers.spinel_os]\ncommand = "/usr/local/bin/spinel-os-mcp"\nenv = { SPINEL_AGENT_CLIENT = "Codex" }';
    } else {
      const cname = t === "antigravity"
        ? "Antigravity"
        : (t === "claude" ? "Claude" : (t === "deepseek" ? "DeepSeekHarness" : "GeminiCLI"));
      pre.textContent = JSON.stringify({
        mcpServers: {
          "spinel-os": {
            command: "/usr/local/bin/spinel-os-mcp",
            args: [],
            env: { SPINEL_AGENT_CLIENT: cname }
          }
        }
      }, null, 2);
    }

    const defaultPaths = {
      antigravity: "~/.gemini/config/mcp_config.json",
      codex: "~/.codex/config.toml",
      claude: "~/.claude.json",
      deepseek: "~/.config/deepseek/mcp_config.json",
      gemini: "~/.gemini/settings.json"
    };
    const st = lastAiMcpClients[t];
    const pathEl = document.getElementById("ai-mcp-path-hint");
    if (pathEl) {
      pathEl.textContent = `配置文件: ${(st && st.path) || defaultPaths[t] || ""}`;
    }
    const chipEl = document.getElementById("ai-mcp-install-chip");
    if (chipEl) {
      const ok = Boolean(st && st.installed);
      chipEl.textContent = ok ? "已接入" : "未写入";
      chipEl.classList.toggle("ok", ok);
      chipEl.classList.toggle("warn", !ok);
    }
    const instBtn = document.getElementById("btn-ai-install-mcp");
    if (instBtn) {
      instBtn.textContent = (st && st.installed) ? "更新配置" : "自动写入配置";
    }
  }

  window.__spinelAiState = (data) => {
    if (!data) return;
    if (data.perms) lastAiPerms = { ...lastAiPerms, ...data.perms };
    if (data.mcp_clients) lastAiMcpClients = data.mcp_clients;

    const countChip = document.getElementById("ai-agent-count-chip");
    if (countChip) {
      const sess = Array.isArray(data.sessions) ? data.sessions : [];
      const active = sess.filter((s) => s.status === "running" || s.status === "waiting_user").length;
      const pend = Array.isArray(data.approvals) ? data.approvals.length : 0;
      countChip.textContent = pend > 0 ? `${active} 活跃 · ${pend} 待审批` : `${active} 活跃会话`;
    }

    document.querySelectorAll("[data-ai-perm]").forEach((btn) => {
      const k = btn.getAttribute("data-ai-perm");
      const on = Boolean(lastAiPerms[k]);
      btn.classList.toggle("is-on", on);
      btn.setAttribute("aria-checked", String(on));
    });

    const t3 = document.getElementById("sel-ai-tier3-policy");
    if (t3 && lastAiPerms.tier3_policy) {
      t3.value = lastAiPerms.tier3_policy;
      if (typeof enhanceSelect === "function") enhanceSelect(t3);
    }
    renderAiMcpSnippet();
  };

  document.querySelectorAll("[data-ai-perm]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const k = btn.getAttribute("data-ai-perm");
      if (!k) return;
      const next = !Boolean(lastAiPerms[k]);
      sendCommand(`ai:perms-set\n${k}=${next ? "true" : "false"}`);
    });
  });

  document.getElementById("sel-ai-tier3-policy")?.addEventListener("change", (ev) => {
    const val = ev.target.value || "ask";
    sendCommand(`ai:perms-set\ntier3_policy=${val}`);
  });

  document.getElementById("sel-ai-mcp-client")?.addEventListener("change", renderAiMcpSnippet);
  document.getElementById("btn-ai-install-mcp")?.addEventListener("click", () => {
    const t = document.getElementById("sel-ai-mcp-client")?.value || "antigravity";
    sendCommand(`ai:install-mcp:${t}`);
  });
  document.getElementById("btn-ai-copy-mcp")?.addEventListener("click", () => {
    const t = document.getElementById("sel-ai-mcp-client")?.value || "antigravity";
    sendCommand(`ai:copy-snippet:${t}`);
  });
  renderAiMcpSnippet();

  window.__spinelPermsData = (data) => {
    if (!data) return;
    const polkitDesc = document.getElementById("perms-polkit-desc");
    if (polkitDesc) {
      if (!data.polkit_installed) polkitDesc.textContent = "未安装密码弹窗组件（请运行 ./scripts/dev-deploy.sh --full）";
      else polkitDesc.textContent = data.polkit_running ? "已开启 · 敏感操作会自动弹出输入密码框" : "未启动 · 点右侧按钮一键开启";
    }
    const portalDesc = document.getElementById("perms-portal-desc");
    if (portalDesc) {
      const ok = data.portal_running && data.portal_gtk && data.portal_conf;
      portalDesc.textContent = ok
        ? "已开启 · 外部软件申请拍照、录屏或打开文件时会弹窗询问"
        : `待就绪（网关:${data.portal_running ? "开" : "关"} / 弹窗:${data.portal_gtk ? "开" : "关"} / 配置:${data.portal_conf ? "有" : "缺"}）`;
    }

    const appList = document.getElementById("perms-app-list");
    if (appList) {
      appList.innerHTML = "";
      const apps = Array.isArray(data.apps) ? data.apps : [];
      if (apps.length === 0) {
        appList.innerHTML = '<div class="perms-empty-row">当前暂无 Flatpak 沙箱应用（原生安装包由系统弹窗保护守护）。</div>';
      } else {
        const permDefs = [
          { key: "home", label: "访问个人文件夹" },
          { key: "host", label: "访问全部硬盘" },
          { key: "network", label: "联网权限" },
          { key: "devices", label: "手柄与外设" },
          { key: "dri", label: "3D 显卡加速" },
          { key: "audio", label: "声音与麦克风" }
        ];
        apps.forEach((app) => {
          const block = document.createElement("div");
          block.className = "perms-app-block";

          const head = document.createElement("div");
          head.className = "section-head";
          head.style.marginBottom = "8px";
          const titleWrap = document.createElement("div");
          const strong = document.createElement("strong");
          strong.textContent = app.name || app.id;
          const small = document.createElement("span");
          small.className = "muted-hint";
          small.style.marginLeft = "8px";
          small.textContent = app.id;
          titleWrap.append(strong, small);

          const resetBtn = document.createElement("button");
          resetBtn.type = "button";
          resetBtn.className = "btn small ghost";
          resetBtn.textContent = "恢复默认";
          resetBtn.setAttribute("data-cmd", `perms:reset:${app.id}`);
          head.append(titleWrap, resetBtn);

          const pills = document.createElement("div");
          pills.className = "perms-toggle-grid";
          permDefs.forEach((pd) => {
            const on = Boolean(app[pd.key]);
            const row = document.createElement("div");
            row.className = "sp-switch-row";
            const lbl = document.createElement("span");
            lbl.textContent = pd.label;
            const sw = document.createElement("button");
            sw.type = "button";
            sw.className = "sp-switch" + (on ? " is-on" : "");
            sw.setAttribute("aria-label", `${app.name || app.id} ${pd.label}`);
            sw.setAttribute("aria-checked", String(on));
            const thumb = document.createElement("span");
            thumb.className = "sp-switch-thumb";
            sw.appendChild(thumb);
            sw.addEventListener("click", (ev) => {
              ev.stopPropagation();
              sendCommand(`perms:set\napp=${app.id}\nkey=${pd.key}\nval=${on ? "false" : "true"}`);
            });
            row.append(lbl, sw);
            pills.appendChild(row);
          });

          block.append(head, pills);
          appList.appendChild(block);
        });
      }
    }

    const portalList = document.getElementById("perms-portal-list");
    if (portalList) {
      portalList.innerHTML = "";
      const entries = Array.isArray(data.portal_entries) ? data.portal_entries : [];
      if (entries.length === 0) {
        portalList.innerHTML = '<div class="perms-empty-row">暂无已记住的弹窗记录（外部软件弹窗询问并选择“允许/拒绝”后会显示在这里）。</div>';
      } else {
        entries.forEach((pe) => {
          const row = document.createElement("div");
          row.className = "perf-row";
          const info = document.createElement("div");
          info.className = "perf-row-info";
          const title = document.createElement("strong");
          title.textContent = `${pe.app} · ${pe.table}/${pe.id}`;
          const sub = document.createElement("span");
          sub.className = "muted-hint";
          sub.textContent = `当前状态: ${pe.perms || "已记录"}`;
          info.append(title, sub);

          const rev = document.createElement("button");
          rev.type = "button";
          rev.className = "btn small danger";
          rev.textContent = "撤销重询";
          rev.addEventListener("click", (ev) => {
            ev.stopPropagation();
            sendCommand(`perms:portal-reset\ntable=${pe.table}\nid=${pe.id}\napp=${pe.app}`);
          });
          row.append(info, rev);
          portalList.appendChild(row);
        });
      }
    }
  };

  /* ---------------- Process Monitor ---------------- */
  const memoryCleanupButton = document.getElementById("btn-clear-memory");
  let memoryCleanupRequested = false;
  let memoryCleanupStartTimer = 0;

  function formatTempBytes(bytes) {
    if (bytes >= 1024 ** 3) return `${(bytes / 1024 ** 3).toFixed(2)} GiB`;
    if (bytes >= 1024 ** 2) return `${(bytes / 1024 ** 2).toFixed(1)} MiB`;
    if (bytes >= 1024) return `${(bytes / 1024).toFixed(0)} KiB`;
    return `${bytes} B`;
  }

  function renderProcessList() {
    const listEl = document.getElementById("procs-list");
    if (!listEl || !lastProcPayload) return;

    const cpuChip = document.getElementById("procs-cpu-chip");
    const memChip = document.getElementById("procs-mem-chip");
    const tmpfsChip = document.getElementById("procs-tmpfs-chip");
    const cntChip = document.getElementById("procs-cnt-chip");
    if (cpuChip) cpuChip.textContent = `CPU: ${lastProcPayload.cpu_pct ?? "--"}%`;
    if (memChip) memChip.textContent = `内存: ${lastProcPayload.mem || "--"}`;
    const tmpfsBytes = lastProcPayload.tmpfs_bytes;
    const hasTmpfs = Number.isFinite(tmpfsBytes);
    if (tmpfsChip) {
      tmpfsChip.classList.toggle("hidden", !hasTmpfs);
      if (hasTmpfs) tmpfsChip.textContent = `/tmp：${formatTempBytes(tmpfsBytes)}`;
    }
    if (memoryCleanupButton) {
      memoryCleanupButton.classList.toggle("hidden", typeof lastProcPayload.memory_cleanup_running !== "boolean");
      memoryCleanupButton.disabled = memoryCleanupRequested || Boolean(lastProcPayload.memory_cleanup_running);
    }
    if (cntChip) cntChip.textContent = `进程: ${lastProcPayload.total_procs ?? "--"}`;

    const q = (document.getElementById("procs-search-input")?.value || "").trim().toLowerCase();
    let procs = Array.isArray(lastProcPayload.procs) ? [...lastProcPayload.procs] : [];
    if (q) {
      procs = procs.filter((p) => `${p.name} ${p.cmd} ${p.pid}`.toLowerCase().includes(q));
    }
    procs.sort((a, b) => {
      if (procSortBy === "mem") return (b.mem_mb - a.mem_mb) || (b.cpu - a.cpu);
      return (b.cpu - a.cpu) || (b.mem_mb - a.mem_mb);
    });

    listEl.innerHTML = "";
    if (procs.length === 0) {
      listEl.innerHTML = '<div class="disk-empty">没有匹配的运行中进程</div>';
      return;
    }

    procs.forEach((p) => {
      const row = document.createElement("div");
      row.className = "proc-row";

      const nameCell = document.createElement("div");
      nameCell.className = "proc-name-cell";
      const st = document.createElement("strong");
      st.textContent = p.name + (p.is_core ? " (系统核心)" : "");
      const sm = document.createElement("small");
      sm.textContent = p.cmd || p.name;
      nameCell.append(st, sm);

      const pidCol = document.createElement("span");
      pidCol.className = "proc-col-num";
      pidCol.textContent = String(p.pid);

      const cpuCol = document.createElement("span");
      cpuCol.className = "proc-col-num";
      cpuCol.textContent = `${Number(p.cpu || 0).toFixed(1)}%`;

      const memCol = document.createElement("span");
      memCol.className = "proc-col-num";
      memCol.textContent = `${p.mem_mb} MB`;

      const actCol = document.createElement("div");
      actCol.className = "proc-col-act";
      if (!p.is_core && p.own) {
        const endBtn = document.createElement("button");
        endBtn.type = "button";
        endBtn.className = "btn small";
        endBtn.textContent = "结束";
        endBtn.setAttribute("data-cmd", `procs:kill:${p.pid}`);

        const killBtn = document.createElement("button");
        killBtn.type = "button";
        killBtn.className = "btn small danger";
        killBtn.textContent = "强杀";
        killBtn.setAttribute("data-cmd", `procs:force:${p.pid}`);
        actCol.append(endBtn, killBtn);
      } else {
        const tag = document.createElement("span");
        tag.className = "muted-hint";
        tag.textContent = p.is_core ? "受保护" : "系统级";
        actCol.appendChild(tag);
      }

      row.append(nameCell, pidCol, cpuCol, memCol, actCol);
      listEl.appendChild(row);
    });
  }

  window.__spinelProcessList = (data) => {
    lastProcPayload = data;
    renderProcessList();
  };
  window.__spinelProcsFeedback = (ok, msg) => showFeedback("procs-feedback", msg || "", !ok);
  memoryCleanupButton?.addEventListener("click", () => {
    memoryCleanupRequested = true;
    memoryCleanupButton.disabled = true;
    showFeedback("procs-feedback", "正在清理内存…");
    memoryCleanupStartTimer = setTimeout(() => {
      memoryCleanupRequested = false;
      memoryCleanupButton.disabled = Boolean(lastProcPayload?.memory_cleanup_running);
      if (!memoryCleanupButton.disabled) showFeedback("procs-feedback", "未收到清理结果，请重试。", true);
    }, 6000);
    sendCommand("monitor:memory-clean");
  });
  window.__spinelMemoryCleanup = (directories, bytes, incomplete, cacheCleared, heapReleased) => {
    clearTimeout(memoryCleanupStartTimer);
    memoryCleanupRequested = false;
    if (lastProcPayload) lastProcPayload.memory_cleanup_running = false;
    if (memoryCleanupButton) memoryCleanupButton.disabled = false;
    const messages = [];
    if (cacheCleared) messages.push("已清理 SpinelOS 界面缓存。");
    else messages.push("界面缓存清理失败。");
    if (heapReleased) messages.push("已归还监视器进程的空闲内存。");
    if (directories > 0) messages.push(`已清理 ${directories} 个临时目录。`);
    if (bytes > 0 && (directories > 0 || incomplete)) messages.push(`/tmp 占用减少 ${formatTempBytes(bytes)}。`);
    if (incomplete) messages.push("部分临时文件未能清理。");
    else if (directories === 0 && bytes !== null) messages.push("当前没有可清理的 SpinelOS 临时目录。");
    showFeedback("procs-feedback", messages.join(""), incomplete || !cacheCleared);
    sendCommand("monitor:procs-get");
  };

  document.getElementById("procs-search-input")?.addEventListener("input", renderProcessList);
  document.querySelectorAll("[data-proc-sort]").forEach((btn) => {
    btn.addEventListener("click", (ev) => {
      ev.stopPropagation();
      procSortBy = btn.getAttribute("data-proc-sort") || "cpu";
      document.querySelectorAll("[data-proc-sort]").forEach((b) => {
        b.classList.toggle("active", b === btn);
      });
      renderProcessList();
    });
  });

  /* ---------------- Audio & Power ---------------- */
  function renderAudioDeviceList(elId, items, cmdPrefix, emptyText) {
    const listEl = document.getElementById(elId);
    if (!listEl) return;
    listEl.innerHTML = "";
    if (!Array.isArray(items) || items.length === 0) {
      listEl.innerHTML = `<div class="disk-empty">${emptyText}</div>`;
      return;
    }
    items.forEach((dev) => {
      const item = document.createElement("div");
      item.className = "disk-item" + (dev.is_default ? " selected" : "");
      const left = document.createElement("div");
      left.className = "disk-left";
      const radio = document.createElement("input");
      radio.type = "radio";
      radio.checked = Boolean(dev.is_default);
      const title = document.createElement("div");
      title.className = "disk-title";
      title.textContent = dev.name;
      left.append(radio, title);
      const tag = document.createElement("span");
      tag.className = "tag";
      tag.textContent = dev.is_default ? "当前默认" : `ID ${dev.id}`;
      item.append(left, tag);
      item.addEventListener("click", () => sendCommand(`${cmdPrefix}:${dev.id}`));
      listEl.appendChild(item);
    });
  }

  window.__spinelMicLevel = (pct, active = true) => {
    const fill = document.getElementById("audio-mic-level-fill");
    const chip = document.getElementById("audio-mic-level-chip");
    const val = Math.max(0, Math.min(100, Number(pct) || 0));
    if (fill) fill.style.width = `${active ? val : 0}%`;
    if (chip) chip.textContent = active ? `电平 ${val}%` : "电平 --";
  };

  window.__spinelAudioPowerData = (data) => {
    if (!data) return;
    const outSlider = document.getElementById("audio-out-slider");
    const outPct = document.getElementById("audio-out-pct");
    if (outSlider && typeof data.volume === "number") outSlider.value = String(data.volume);
    if (outPct && typeof data.volume === "number") outPct.textContent = `${data.volume}%`;

    const micSlider = document.getElementById("audio-mic-slider");
    const micPct = document.getElementById("audio-mic-pct");
    const micMuteBtn = document.getElementById("btn-mic-mute");
    if (micSlider && typeof data.mic_volume === "number") micSlider.value = String(data.mic_volume);
    if (micPct && typeof data.mic_volume === "number") {
      micPct.textContent = data.mic_muted ? `${data.mic_volume}% (已静音)` : `${data.mic_volume}%`;
    }
    if (micMuteBtn) micMuteBtn.textContent = data.mic_muted ? "取消静音" : "静音麦克风";

    renderAudioDeviceList("audio-sink-list", data.sinks, "settings:audio-sink", "未检测到音频输出设备");
    renderAudioDeviceList("audio-source-list", data.sources, "settings:audio-source", "未检测到麦克风输入设备");

    const spkMode = data.speaker_testing || "idle";
    document.querySelectorAll("#tab-audio [data-speaker-test]").forEach((btn) => {
      btn.classList.toggle("active", btn.getAttribute("data-speaker-test") === spkMode);
    });
    const spkStatus = document.getElementById("audio-speaker-test-status");
    if (spkStatus) {
      if (spkMode === "left") spkStatus.textContent = "正在播放左声道测试音（仅左声道发声）...";
      else if (spkMode === "right") spkStatus.textContent = "正在播放右声道测试音（仅右声道发声）...";
      else if (spkMode === "stereo") spkStatus.textContent = "正在播放立体声测试音（双声道均衡）...";
      else spkStatus.textContent = data.audio_test_hint || "点击测试左/右声道或立体声扬声器输出";
    }

    const micTesting = Boolean(data.mic_testing);
    const micTestBtn = document.getElementById("btn-mic-test");
    const micTestStatus = document.getElementById("audio-mic-test-status");
    if (micTestBtn) {
      micTestBtn.classList.toggle("is-testing", micTesting);
      micTestBtn.textContent = micTesting ? "停止麦克风测试" : "测试麦克风（回听自己声音）";
    }
    if (micTestStatus) {
      if (micTesting) {
        micTestStatus.textContent = data.mic_muted
          ? "正在监听（麦克风当前已静音，取消静音即可听到声音）"
          : (data.audio_test_hint || "正在实时监听：对着麦克风说话即可在当前输出设备中听到自己声音");
      } else {
        micTestStatus.textContent = "点击开启实时监听与音量电平检测（建议使用耳机防止啸叫）";
      }
    }
    if (!micTesting) window.__spinelMicLevel(0, false);

    const supported = Array.isArray(data.supported_profiles) && data.supported_profiles.length > 0
      ? data.supported_profiles
      : ["power-saver", "balanced", "performance"];
    document.querySelectorAll("#tab-audio [data-power-profile]").forEach((btn) => {
      const p = btn.getAttribute("data-power-profile");
      const isSup = supported.includes(p);
      btn.classList.toggle("active", p === data.power_profile);
      btn.disabled = !data.ppd_available || !isSup;
      btn.title = (!isSup && data.ppd_available) ? "当前硬件/虚拟机电源驱动不支持该档位" : "";
    });
    const ppHint = document.getElementById("power-profile-hint");
    if (ppHint) {
      ppHint.textContent = (!supported.includes("performance") && data.ppd_available)
        ? "当前驱动仅支持「省电 / 平衡」档位（虚拟机或占位驱动不支持性能档）"
        : "调节 CPU 调度倾向（笔记本续航或插电游戏全速运行）";
    }
    const idleOn = document.getElementById("power-idle-lock-on");
    const idleOff = document.getElementById("power-idle-lock-off");
    if (idleOn && idleOff) {
      const enabled = Boolean(data.idle_lock);
      idleOn.classList.toggle("active", enabled);
      idleOff.classList.toggle("active", !enabled);
    }
    const sumEl = document.getElementById("power-hw-summary");
    if (sumEl) {
      const batText = (typeof data.battery_pct === "number" && data.battery_pct >= 0)
        ? `电池 ${data.battery_pct}%（${data.battery_charging ? "充电中" : "使用电池"}）`
        : "台式机/外接电源";
      const zramText = data.zram_active ? "zram 内存压缩已启用" : "zram 未启用";
      sumEl.textContent = `${batText} · ${zramText}`;
    }
  };

  document.getElementById("audio-out-slider")?.addEventListener("input", (ev) => {
    const v = ev.target.value;
    const pct = document.getElementById("audio-out-pct");
    if (pct) pct.textContent = `${v}%`;
    sendCommand(`system:audio-set:${v}`);
  });
  document.getElementById("audio-mic-slider")?.addEventListener("input", (ev) => {
    const v = ev.target.value;
    const pct = document.getElementById("audio-mic-pct");
    if (pct) pct.textContent = `${v}%`;
    sendCommand(`settings:mic-vol:${v}`);
  });

  /* ---------------- Keyboard & Shortcuts ---------------- */
  let recordingShortcutField = null;

  function formatLabwcKey(raw) {
    if (!raw) return "--";
    return String(raw)
      .split("-")
      .map((part) => {
        if (part === "C") return "Ctrl";
        if (part === "S") return "Shift";
        if (part === "A") return "Alt";
        if (part === "W") return "Super";
        if (part === "Return") return "Enter";
        if (part === "space") return "Space";
        if (part.length === 1) return part.toUpperCase();
        return part;
      })
      .join(" + ");
  }

  function eventToLabwcKey(ev) {
    const modKeys = new Set(["Control", "Shift", "Alt", "Meta", "Super", "OS"]);
    if (modKeys.has(ev.key)) return null;
    let keyName = "";
    const c = ev.code || "";
    if (/^Key[A-Z]$/.test(c)) keyName = c.slice(3).toLowerCase();
    else if (/^Digit[0-9]$/.test(c)) keyName = c.slice(5);
    else if (/^F([1-9]|1[0-2])$/.test(c)) keyName = c;
    else if (c === "Enter" || c === "NumpadEnter") keyName = "Return";
    else if (c === "Space") keyName = "space";
    else if (c === "Tab") keyName = "Tab";
    else if (c === "Backspace") keyName = "BackSpace";
    else if (c === "Delete") keyName = "Delete";
    else if (c === "PrintScreen" || ev.key === "Print") keyName = "Print";
    else if (c === "Minus") keyName = "minus";
    else if (c === "Equal") keyName = "equal";
    else if (c === "Slash") keyName = "slash";
    else if (/^[a-zA-Z0-9]$/.test(ev.key)) keyName = ev.key.toLowerCase();
    if (!keyName) return null;

    const mods = [];
    if (ev.ctrlKey) mods.push("C");
    if (ev.altKey) mods.push("A");
    if (ev.shiftKey) mods.push("S");
    if (ev.metaKey) mods.push("W");
    const allowBare = (keyName === "Print" || /^F([1-9]|1[0-2])$/.test(keyName));
    if (mods.length === 0 && !allowBare) return null;
    return mods.length > 0 ? `${mods.join("-")}-${keyName}` : keyName;
  }

  function resetRecordButtons() {
    recordingShortcutField = null;
    document.querySelectorAll("[data-record-key]").forEach((btn) => {
      btn.textContent = "录制快捷键";
      btn.classList.add("primary");
    });
  }

  window.__spinelShortcutsFeedback = (ok, msg) => showFeedback("shortcuts-feedback", msg || "", !ok);

  window.__spinelShortcutsData = (data) => {
    if (!data) return;
    ["screenshot_area", "screenshot_full", "lock_screen", "open_terminal"].forEach((k) => {
      const el = document.getElementById(`kbd-val-${k}`);
      if (el && data[k]) el.textContent = formatLabwcKey(data[k]);
    });
  };

  document.querySelectorAll("[data-record-key]").forEach((btn) => {
    btn.addEventListener("click", (ev) => {
      ev.stopPropagation();
      const field = btn.getAttribute("data-record-key");
      if (recordingShortcutField === field) {
        resetRecordButtons();
        return;
      }
      resetRecordButtons();
      recordingShortcutField = field;
      btn.textContent = "按下组合键 (Esc取消)";
      btn.classList.remove("primary");
    });
  });

  document.addEventListener("keydown", (ev) => {
    if (!recordingShortcutField) return;
    ev.preventDefault();
    ev.stopPropagation();
    if (ev.key === "Escape") {
      resetRecordButtons();
      return;
    }
    const labwcKey = eventToLabwcKey(ev);
    if (!labwcKey) {
      if (!["Control", "Shift", "Alt", "Meta"].includes(ev.key)) {
        showFeedback("shortcuts-feedback", "请至少搭配 Ctrl / Alt / Super 等修饰键（或使用 Print / F1~F12）。", true);
      }
      return;
    }
    const targetField = recordingShortcutField;
    resetRecordButtons();
    sendCommand(`settings:shortcuts-set\n${targetField}=${labwcKey}`);
  }, true);

  /* ---------------- IME, Logs & Spinel Doctor ---------------- */
  window.__spinelImeState = (data) => {
    if (!data) return;
    const chip = document.getElementById("ime-status-chip");
    if (chip) {
      if (!data.installed) chip.textContent = "未安装（需 --full 更新）";
      else {
        const modeText = data.active_zh ? (data.traditional ? "中文 · 繁体" : "中文 · 简体") : "英文";
        const dictText = data.rime_ice ? "雾凇词库" : "内置拼音";
        chip.textContent = (data.running ? "运行中 · " : "未启动 · ") + `${modeText} (${dictText})`;
      }
    }
    const enBtn = document.getElementById("btn-ime-en");
    const zhBtn = document.getElementById("btn-ime-zh");
    if (enBtn) enBtn.classList.toggle("active", !data.active_zh);
    if (zhBtn) zhBtn.classList.toggle("active", Boolean(data.active_zh));

    const simpBtn = document.getElementById("btn-ime-simp");
    const tradBtn = document.getElementById("btn-ime-trad");
    if (simpBtn) simpBtn.classList.toggle("active", !data.traditional);
    if (tradBtn) tradBtn.classList.toggle("active", Boolean(data.traditional));

    const iceBtn = document.getElementById("btn-ime-ice");
    if (iceBtn) {
      iceBtn.textContent = data.rime_ice ? "更新雾凇词库" : "安装雾凇词库（增强联想）";
      iceBtn.classList.toggle("primary", !data.rime_ice);
    }
  };

  window.__spinelImeFeedback = (ok, msg) => showFeedback("ime-feedback", msg || "", !ok);

  window.__spinelLogsData = (data) => {
    if (!data) return;
    if (data.source) activeLogSource = data.source;
    document.querySelectorAll("#logs-source-tabs .pill-btn").forEach((b) => {
      const cmd = b.getAttribute("data-cmd") || "";
      b.classList.toggle("active", cmd === `monitor:logs:${activeLogSource}` || cmd === `settings:logs:${activeLogSource}`);
    });
    const clrBtn = document.getElementById("btn-logs-clear");
    if (clrBtn) {
      clrBtn.textContent = activeLogSource === "mcp" ? "清空审计日志" : "清空应用日志";
    }
    const box = document.getElementById("sys-logs-box");
    if (box) {
      box.textContent = data.text || "(暂无日志内容)";
      box.scrollTop = box.scrollHeight;
    }
  };

  window.__spinelDoctorData = (data) => {
    if (!data) return;
    const chip = document.getElementById("doc-summary-chip");
    if (chip) {
      const ok = Number(data.ok_count || 0);
      const warn = Number(data.warn_count || 0);
      const fail = Number(data.fail_count || 0);
      chip.textContent = fail > 0 ? `${ok} 正常 · ${warn} 提示 · ${fail} 异常` : (warn > 0 ? `${ok} 正常 · ${warn} 提示` : `${ok} 项全部健康`);
    }
    const grid = document.getElementById("doc-check-grid");
    if (!grid) return;
    grid.innerHTML = "";
    (Array.isArray(data.checks) ? data.checks : []).forEach((c) => {
      const row = document.createElement("div");
      row.className = "doctor-card";
      const top = document.createElement("div");
      top.className = "doctor-card-head";
      const name = document.createElement("strong");
      name.textContent = c.name || c.id;
      const badge = document.createElement("span");
      badge.className = `doc-badge doc-${c.status || "warn"}`;
      badge.textContent = c.status === "ok" ? "正常" : (c.status === "fail" ? "异常" : "提示");
      top.append(name, badge);

      const det = document.createElement("div");
      det.className = "muted-hint";
      det.textContent = c.detail || "";
      row.append(top, det);

      if (c.fix_hint) {
        const hint = document.createElement("div");
        hint.className = "doctor-hint";
        hint.textContent = `建议：${c.fix_hint}`;
        row.appendChild(hint);
      }
      grid.appendChild(row);
    });
  };

  window.__spinelDoctorExported = (ok, path) => {
    showFeedback("doc-feedback", ok ? `已导出脱敏诊断报告到桌面：${path}（已自动隐藏用户目录、密码与代理凭据）` : "导出诊断报告失败", !ok);
  };

  document.getElementById("btn-logs-refresh")?.addEventListener("click", () => {
    sendCommand(`monitor:logs:${activeLogSource}`);
  });
  document.getElementById("btn-logs-clear")?.addEventListener("click", () => {
    sendCommand(`monitor:logs-clear:${activeLogSource}`);
  });
  document.getElementById("btn-logs-copy")?.addEventListener("click", () => {
    const text = document.getElementById("sys-logs-box")?.textContent || "";
    const btn = document.getElementById("btn-logs-copy");
    navigator.clipboard?.writeText(text).then(() => {
      if (btn) {
        btn.textContent = "已复制!";
        setTimeout(() => { btn.textContent = "复制全部"; }, 1400);
      }
    }).catch(() => {});
  });

  /* ---------------- Network, Proxy, Wi-Fi, Bluetooth & Display ---------------- */
  let detectedGateway = "";

  window.__spinelProxyConfig = (data) => {
    if (!data) return;
    detectedGateway = data.gateway || "";
    const chip = document.getElementById("proxy-status-chip");
    if (chip) {
      chip.textContent = data.enabled
        ? "已启用"
        : (data.external_proxy ? `外部代理生效 (${data.external_proxy})` : "已关闭");
    }
    const hint = document.getElementById("proxy-gw-hint");
    if (hint) {
      hint.textContent = detectedGateway
        ? `当前默认网关: ${detectedGateway}（NAT 模式下一般即为宿主机代理可达网关，需开启宿主机的“允许局域网连接”）。`
        : "适用于 NAT 模式连接 Windows/Mac 宿主机代理软件（如 Clash / v2rayN，需开启“允许局域网连接”）。";
    }
    const httpEl = document.getElementById("proxy-http-input");
    const socksEl = document.getElementById("proxy-socks-input");
    const noEl = document.getElementById("proxy-noproxy-input");
    if (httpEl && typeof data.http_proxy === "string") httpEl.value = data.http_proxy;
    if (socksEl && typeof data.socks_proxy === "string") socksEl.value = data.socks_proxy;
    if (noEl && data.no_proxy) noEl.value = data.no_proxy;
  };

  window.__spinelProxyAction = (ok, msg) => showFeedback("proxy-feedback", msg || (ok ? "代理设置已保存" : "保存失败"), !ok);

  document.getElementById("btn-proxy-gw")?.addEventListener("click", () => {
    const gw = detectedGateway || "192.168.0.1";
    const httpEl = document.getElementById("proxy-http-input");
    if (httpEl) httpEl.value = `http://${gw}:7890`;
  });

  document.getElementById("btn-proxy-save")?.addEventListener("click", () => {
    const httpVal = (document.getElementById("proxy-http-input")?.value || "").trim();
    const socksVal = (document.getElementById("proxy-socks-input")?.value || "").trim();
    const noVal = (document.getElementById("proxy-noproxy-input")?.value || "").trim();
    if (!httpVal && !socksVal) {
      showFeedback("proxy-feedback", "请至少填写 HTTP 或 SOCKS5 代理地址。", true);
      return;
    }
    sendCommand([
      "settings:proxy-save",
      "enabled=true",
      `http_proxy=${httpVal}`,
      `socks_proxy=${socksVal}`,
      `no_proxy=${noVal}`
    ].join("\n"));
  });

  document.getElementById("btn-proxy-clear")?.addEventListener("click", () => {
    sendCommand("settings:proxy-clear");
  });

  window.__spinelWifiState = (data) => {
    const rescanBtn = document.getElementById("btn-wifi-rescan");
    if (rescanBtn) { rescanBtn.disabled = false; rescanBtn.textContent = "扫描周边 Wi-Fi"; }
    const ifaceEl = document.getElementById("wifi-iface-list");
    if (ifaceEl) {
      ifaceEl.innerHTML = "";
      const ifaces = Array.isArray(data.interfaces) ? data.interfaces : [];
      if (ifaces.length === 0) {
        ifaceEl.innerHTML = `<div class="disk-empty">未检测到网络接口</div>`;
      } else {
        ifaces.forEach((ifx) => {
          const item = document.createElement("div");
          item.className = "disk-item";
          const left = document.createElement("div");
          const t = document.createElement("div"); t.className = "disk-title"; t.textContent = `${ifx.device} (${ifx.type})`;
          const modeLabel = ifx.method === "manual" ? "手动静态 IP" : "DHCP 自动";
          const addrDesc = ifx.ip ? `${ifx.ip}/${ifx.prefix || "24"}` : "未分配 IP";
          const gwDesc = ifx.gateway ? ` · 网关 ${ifx.gateway}` : "";
          const dnsDesc = ifx.dns ? ` · DNS ${ifx.dns}` : "";
          const sub = document.createElement("div"); sub.className = "disk-sub";
          sub.textContent = ifx.connection
            ? `${ifx.connection} · ${modeLabel} · ${addrDesc}${gwDesc}${dnsDesc}`
            : `状态: ${ifx.state}`;
          left.append(t, sub);
          const right = document.createElement("div"); right.className = "disk-tags";
          const tag = document.createElement("span"); tag.className = "tag";
          tag.textContent = ifx.ip ? (ifx.method === "manual" ? "静态" : "DHCP") : ifx.state;
          right.appendChild(tag);

          const cfgBtn = document.createElement("button");
          cfgBtn.type = "button";
          cfgBtn.className = "btn small primary";
          cfgBtn.textContent = "配置 IP / DNS";
          cfgBtn.addEventListener("click", (ev) => {
            ev.stopPropagation();
            openNetIpConfig(ifx);
          });
          right.appendChild(cfgBtn);

          if (ifx.state === "connected") {
            const disc = document.createElement("button");
            disc.className = "btn small danger"; disc.textContent = "断开";
            disc.setAttribute("data-cmd", `settings:wifi-disconnect:${ifx.device}`);
            right.appendChild(disc);
          }
          item.append(left, right);
          ifaceEl.appendChild(item);
        });
      }
    }

    const listEl = document.getElementById("wifi-network-list");
    if (!listEl) return;
    listEl.innerHTML = "";
    const nets = Array.isArray(data.networks) ? data.networks : [];
    if (nets.length === 0) {
      listEl.innerHTML = `<div class="disk-empty">暂无扫描到的 Wi-Fi 热点</div>`;
      return;
    }
    nets.sort((a, b) => Number(b.active) - Number(a.active) || b.signal - a.signal);
    nets.forEach((n) => {
      const item = document.createElement("div");
      item.className = "disk-item" + (n.active ? " selected" : "");
      const left = document.createElement("div");
      const title = document.createElement("div"); title.className = "disk-title"; title.textContent = n.ssid;
      const sub = document.createElement("div"); sub.className = "disk-sub"; sub.textContent = `信号 ${n.signal}% · ${n.security || "开放网络"}`;
      left.append(title, sub);
      const tags = document.createElement("div"); tags.className = "disk-tags";
      if (n.active) {
        const tag = document.createElement("span"); tag.className = "tag"; tag.textContent = "已连接";
        tags.appendChild(tag);
      } else {
        const btn = document.createElement("button"); btn.className = "btn small"; btn.textContent = "连接";
        btn.addEventListener("click", (ev) => {
          ev.stopPropagation();
          state.selectedWifiSsid = n.ssid;
          document.getElementById("wifi-target-ssid").textContent = `连接到: ${n.ssid}`;
          document.getElementById("wifi-password-input").value = "";
          document.getElementById("wifi-connect-box").classList.remove("hidden");
          showFeedback("wifi-feedback", "");
        });
        tags.appendChild(btn);
      }
      item.append(left, tags);
      listEl.appendChild(item);
    });
  };

  let activeNetCfgIface = null;

  function syncNetMethodInputs() {
    const method = document.getElementById("net-ip-method")?.value || "auto";
    const isManual = method === "manual";
    ["net-ip-addr", "net-ip-mask", "net-ip-gw"].forEach((id) => {
      const el = document.getElementById(id);
      if (el) el.disabled = !isManual;
    });
  }

  function openNetIpConfig(ifx) {
    activeNetCfgIface = ifx;
    const box = document.getElementById("net-ip-config-box");
    const titleEl = document.getElementById("net-cfg-title");
    const methodEl = document.getElementById("net-ip-method");
    const addrEl = document.getElementById("net-ip-addr");
    const maskEl = document.getElementById("net-ip-mask");
    const gwEl = document.getElementById("net-ip-gw");
    const dnsEl = document.getElementById("net-ip-dns");

    if (titleEl) titleEl.textContent = `配置网络接口: ${ifx.device}${ifx.connection ? ` (${ifx.connection})` : ""}`;
    if (methodEl) methodEl.value = ifx.method === "manual" ? "manual" : "auto";
    if (addrEl) addrEl.value = ifx.ip || "";
    if (maskEl) maskEl.value = ifx.prefix || "24";
    if (gwEl) gwEl.value = ifx.gateway || "";
    if (dnsEl) dnsEl.value = ifx.dns || "";
    UI.syncAllCustomSelects?.();
    syncNetMethodInputs();
    box?.classList.remove("hidden");
    showFeedback("wifi-feedback", "");
  }

  document.getElementById("net-ip-method")?.addEventListener("change", syncNetMethodInputs);
  document.getElementById("btn-net-ip-cancel")?.addEventListener("click", () => {
    activeNetCfgIface = null;
    document.getElementById("net-ip-config-box")?.classList.add("hidden");
  });
  document.querySelectorAll("[data-dns-preset]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const el = document.getElementById("net-ip-dns");
      if (el) el.value = btn.getAttribute("data-dns-preset") || "";
    });
  });
  document.getElementById("btn-net-ip-save")?.addEventListener("click", () => {
    if (!activeNetCfgIface) return;
    const method = document.getElementById("net-ip-method")?.value || "auto";
    const ip = (document.getElementById("net-ip-addr")?.value || "").trim();
    const mask = (document.getElementById("net-ip-mask")?.value || "").trim();
    const gw = (document.getElementById("net-ip-gw")?.value || "").trim();
    const dns = (document.getElementById("net-ip-dns")?.value || "").trim();
    sendCommand([
      "settings:net-ip-apply",
      `device=${activeNetCfgIface.device}`,
      `connection=${activeNetCfgIface.connection || ""}`,
      `method=${method}`,
      `ip=${ip}`,
      `mask=${mask}`,
      `gateway=${gw}`,
      `dns=${dns}`
    ].join("\n"));
  });

  window.__spinelWifiAction = (ok, msg) => {
    showFeedback("wifi-feedback", msg || (ok ? "操作成功" : "操作失败"), !ok);
    if (ok) {
      document.getElementById("wifi-connect-box")?.classList.add("hidden");
      document.getElementById("net-ip-config-box")?.classList.add("hidden");
    }
  };

  window.__spinelBtState = (data) => {
    state.btPowered = Boolean(data.powered);
    const badge = document.getElementById("bt-power-badge");
    const powerBtn = document.getElementById("btn-bt-power");
    const scanBtn = document.getElementById("btn-bt-scan");
    if (badge) badge.textContent = state.btPowered ? "已开启" : "已关闭";
    if (powerBtn) powerBtn.textContent = state.btPowered ? "关闭蓝牙" : "开启蓝牙";
    if (scanBtn) { scanBtn.disabled = false; scanBtn.textContent = "扫描手柄 (6s)"; }

    const listEl = document.getElementById("bt-device-list");
    if (!listEl) return;
    listEl.innerHTML = "";
    const devs = Array.isArray(data.devices) ? data.devices : [];
    if (devs.length === 0) {
      listEl.innerHTML = `<div class="disk-empty">${state.btPowered ? "暂无发现的蓝牙设备，请将手柄置于配对模式后扫描。" : "蓝牙已关闭，请先开启蓝牙。"}</div>`;
      return;
    }
    devs.sort((a, b) => Number(b.connected) - Number(a.connected) || Number(b.paired) - Number(a.paired));
    devs.forEach((dev) => {
      const item = document.createElement("div");
      item.className = "disk-item" + (dev.connected ? " selected" : "");
      const left = document.createElement("div");
      const title = document.createElement("div"); title.className = "disk-title"; title.textContent = dev.name;
      const sub = document.createElement("div"); sub.className = "disk-sub"; sub.textContent = `${dev.mac}${dev.icon ? ` · ${dev.icon}` : ""}`;
      left.append(title, sub);
      const tags = document.createElement("div"); tags.className = "disk-tags";
      if (dev.connected) {
        const tag = document.createElement("span"); tag.className = "tag"; tag.textContent = "已连接"; tags.appendChild(tag);
        const disc = document.createElement("button"); disc.className = "btn small"; disc.textContent = "断开";
        disc.setAttribute("data-cmd", `settings:bt-disconnect:${dev.mac}`); tags.appendChild(disc);
      } else {
        if (dev.paired) { const tag = document.createElement("span"); tag.className = "tag"; tag.textContent = "已配对"; tags.appendChild(tag); }
        const conn = document.createElement("button"); conn.className = "btn primary small";
        conn.textContent = dev.paired ? "连接" : "配对并连接";
        conn.setAttribute("data-cmd", `settings:bt-connect:${dev.mac}`); tags.appendChild(conn);
      }
      if (dev.paired) {
        const rem = document.createElement("button"); rem.className = "btn small danger"; rem.textContent = "移除";
        rem.setAttribute("data-cmd", `settings:bt-remove:${dev.mac}`); tags.appendChild(rem);
      }
      item.append(left, tags);
      listEl.appendChild(item);
    });
  };

  window.__spinelBtAction = (ok, msg) => showFeedback("bt-feedback", msg || (ok ? "蓝牙操作完成" : "蓝牙操作失败"), !ok);

  window.__spinelDisplayState = (outputs) => {
    const listEl = document.getElementById("display-output-list");
    if (!listEl) return;
    listEl.innerHTML = "";
    const outs = Array.isArray(outputs) ? outputs : [];
    if (outs.length === 0) {
      listEl.innerHTML = `<div class="disk-empty">未读取到 Wayland 显示器输出。</div>`;
      return;
    }
    outs.forEach((out) => {
      const item = document.createElement("div");
      item.className = "disk-item";
      const left = document.createElement("div");
      const title = document.createElement("div"); title.className = "disk-title";
      title.textContent = `${out.name}${out.model ? ` (${out.model})` : ""}`;
      const curMode = Array.isArray(out.modes) ? out.modes.find((m) => m.current) : null;
      const sub = document.createElement("div"); sub.className = "disk-sub";
      sub.textContent = curMode
        ? `当前: ${curMode.width}x${curMode.height} @ ${Number(curMode.refresh).toFixed(2)} Hz · 缩放 ${out.scale || 1}`
        : `缩放: ${out.scale || 1}`;
      left.append(title, sub);

      const controls = document.createElement("div");
      controls.className = "display-controls";
      const modeSel = document.createElement("select");
      const seenMode = new Set();
      (Array.isArray(out.modes) ? out.modes : []).forEach((m) => {
        const val = `${m.width}x${m.height}@${Number(m.refresh).toFixed(3)}Hz`;
        if (seenMode.has(val)) return;
        seenMode.add(val);
        const opt = document.createElement("option");
        opt.value = val;
        opt.textContent = `${m.width}x${m.height} @ ${Number(m.refresh).toFixed(2)} Hz`;
        if (m.current) opt.selected = true;
        modeSel.appendChild(opt);
      });
      const scaleSel = document.createElement("select");
      ["1", "1.25", "1.5", "2"].forEach((sc) => {
        const opt = document.createElement("option");
        opt.value = sc;
        opt.textContent = `缩放 ${sc}x`;
        if (Math.abs(Number(out.scale || 1) - Number(sc)) < 0.05) opt.selected = true;
        scaleSel.appendChild(opt);
      });
      const applyBtn = document.createElement("button");
      applyBtn.className = "btn primary small";
      applyBtn.textContent = "应用并保存";
      applyBtn.addEventListener("click", () => {
        sendCommand(["settings:display-apply", `output=${out.name}`, `mode=${modeSel.value}`, `scale=${scaleSel.value}`].join("\n"));
      });
      controls.append(modeSel, scaleSel, applyBtn);
      item.append(left, controls);
      listEl.appendChild(item);
      enhanceSelect(modeSel);
      enhanceSelect(scaleSel);
    });
  };

  window.__spinelDisplayAction = (ok, msg) => showFeedback("display-feedback", msg || (ok ? "已应用显示设置" : "应用失败"), !ok);

  /* ---------------- Device Manager ---------------- */
  let pendingFstabOp = null;
  let devicesSudoNopass = false;

  function suggestMntFolderName(d) {
    const ascii = String(d.label || "")
      .replace(/[^A-Za-z0-9._-]+/g, "_")
      .replace(/^_+|_+$/g, "")
      .slice(0, 28);
    if (ascii.length >= 3) return ascii.toLowerCase();
    return String(d.name || "disk").replace(/[^A-Za-z0-9._-]+/g, "_");
  }

  function closeFstabModal() {
    pendingFstabOp = null;
    document.getElementById("dev-fstab-modal")?.classList.add("hidden");
  }

  function openFstabModal(mode, d) {
    pendingFstabOp = { mode, dev: d.path, label: d.label || d.name };
    const modal = document.getElementById("dev-fstab-modal");
    const title = document.getElementById("dev-fstab-title");
    const desc = document.getElementById("dev-fstab-desc");
    const nameWrap = document.getElementById("dev-fstab-name-wrap");
    const modeWrap = document.getElementById("dev-fstab-mode-wrap");
    const passWrap = document.getElementById("dev-fstab-pass-wrap");
    const nameInput = document.getElementById("dev-fstab-name-input");
    const modeSelect = document.getElementById("dev-fstab-mode-select");
    const passInput = document.getElementById("dev-fstab-pass-input");
    const confirmBtn = document.getElementById("btn-dev-fstab-confirm");

    const isRemove = mode === "remove";
    if (title) title.textContent = isRemove ? `取消开机挂载: ${d.label || d.name}` : `固定挂载到 /mnt: ${d.label || d.name}`;
    if (desc) {
      desc.textContent = isRemove
        ? `将从 /etc/fstab 中移除 ${d.path} (${d.fstab_mount}) 的开机自动挂载配置，不会删除磁盘数据。`
        : `将 ${d.path} 写入 /etc/fstab，实现每次开机自动挂载到 /mnt 目录（自动附加 nofail 保护，防止拔盘卡死）。`;
    }
    nameWrap?.classList.toggle("hidden", isRemove);
    modeWrap?.classList.toggle("hidden", isRemove);
    passWrap?.classList.toggle("hidden", devicesSudoNopass);
    if (confirmBtn) confirmBtn.textContent = isRemove ? "确认移除" : "确认应用";
    if (nameInput && !isRemove) nameInput.value = suggestMntFolderName(d);
    if (modeSelect && !isRemove) modeSelect.value = (d.fstype === "ntfs" ? "ro" : "rw");
    if (passInput) passInput.value = "";
    modal?.classList.remove("hidden");
    if (!isRemove) nameInput?.focus();
    else passInput?.focus();
  }

  function submitFstabModal() {
    if (!pendingFstabOp) return;
    const pass = document.getElementById("dev-fstab-pass-input")?.value || "";
    if (!devicesSudoNopass && !pass) {
      document.getElementById("dev-fstab-pass-input")?.focus();
      return;
    }
    if (pendingFstabOp.mode === "remove") {
      sendCommand([
        "devices:fstab-remove",
        `dev=${pendingFstabOp.dev}`,
        `password=${pass}`
      ].join("\n"));
    } else {
      const name = (document.getElementById("dev-fstab-name-input")?.value || "").trim();
      const modeVal = document.getElementById("dev-fstab-mode-select")?.value || "ro";
      if (!/^[A-Za-z0-9._-]{1,48}$/.test(name) || name === "." || name === "..") {
        showFeedback("devices-feedback", "挂载目录名仅支持 1~48 位英文、数字、下划线、点或连字符。", true);
        return;
      }
      sendCommand([
        "devices:fstab-set",
        `dev=${pendingFstabOp.dev}`,
        `name=${name}`,
        `ro=${modeVal === "ro" ? "1" : "0"}`,
        `password=${pass}`
      ].join("\n"));
    }
    closeFstabModal();
  }

  document.getElementById("btn-dev-fstab-cancel")?.addEventListener("click", closeFstabModal);
  document.getElementById("btn-dev-fstab-confirm")?.addEventListener("click", submitFstabModal);
  document.getElementById("dev-fstab-modal")?.addEventListener("click", (ev) => {
    if (ev.target.id === "dev-fstab-modal") closeFstabModal();
  });
  document.getElementById("dev-fstab-pass-input")?.addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") { ev.preventDefault(); submitFstabModal(); }
  });

  window.__spinelDevicesAction = (ok, msg) => showFeedback("devices-feedback", msg || (ok ? "外设操作完成" : "外设操作失败"), !ok);

  window.__spinelDevicesData = (data) => {
    if (!data) return;
    devicesSudoNopass = Boolean(data.sudo_nopass);
    const disks = Array.isArray(data.disks) ? data.disks : [];
    const usb = Array.isArray(data.usb) ? data.usb : [];
    const inputs = Array.isArray(data.inputs) ? data.inputs : [];
    const audioDevs = Array.isArray(data.audio_devs) ? data.audio_devs : [];

    const summaryEl = document.getElementById("dev-summary-chip");
    if (summaryEl) {
      summaryEl.textContent = `存储 ${disks.length} · USB 外设 ${usb.length} · 音频 ${audioDevs.length} · 输入 ${inputs.length}`;
    }

    const diskList = document.getElementById("dev-disk-list");
    if (diskList) {
      diskList.innerHTML = "";
      if (disks.length === 0) {
        diskList.innerHTML = '<div class="disk-empty">未检测到外置 U 盘或扩展数据分区（插入外置磁盘后点右上角「重新扫描外设」）。</div>';
      } else {
        disks.forEach((d) => {
          const item = document.createElement("div");
          item.className = "disk-item";
          const left = document.createElement("div");
          const title = document.createElement("div");
          title.className = "disk-title";
          title.textContent = `${d.label || d.name} (${d.path})`;
          const busLabel = d.removable ? "USB/外置" : (d.tran ? d.tran.toUpperCase() : "扩展磁盘");
          const mountDesc = d.mountpoint ? `已挂载: ${d.mountpoint}${d.mounted_ro ? " (只读)" : ""}` : "未挂载";
          const fstabDesc = d.fstab_mount ? ` · 开机自动挂载: ${d.fstab_mount} (${d.fstab_ro ? "只读" : "读写"})` : "";
          const sub = document.createElement("div");
          sub.className = "disk-sub";
          sub.textContent = `${busLabel} · ${d.size || "-"} · ${d.fstype || "-"} · ${mountDesc}${fstabDesc}`;
          left.append(title, sub);

          const actions = document.createElement("div");
          actions.className = "disk-tags";
          if (d.mountpoint) {
            const openBtn = document.createElement("button");
            openBtn.type = "button";
            openBtn.className = "btn small primary";
            openBtn.textContent = "打开目录";
            openBtn.setAttribute("data-cmd", `devices:open:${d.mountpoint}`);
            const umountBtn = document.createElement("button");
            umountBtn.type = "button";
            umountBtn.className = "btn small";
            umountBtn.textContent = "卸载";
            umountBtn.setAttribute("data-cmd", `devices:unmount:${d.path}`);
            actions.append(openBtn, umountBtn);
          } else {
            const mountBtn = document.createElement("button");
            mountBtn.type = "button";
            mountBtn.className = "btn small primary";
            mountBtn.textContent = "挂载使用";
            mountBtn.setAttribute("data-cmd", `devices:mount:${d.path}`);
            const roBtn = document.createElement("button");
            roBtn.type = "button";
            roBtn.className = "btn small";
            roBtn.textContent = "只读挂载";
            roBtn.title = "仅读取数据（适合 Windows 休眠/快速启动盘或重要资料备份）";
            roBtn.setAttribute("data-cmd", `devices:mount-ro:${d.path}`);
            actions.append(mountBtn, roBtn);
          }
          if (d.uuid && d.fstype && d.fstype !== "未格式化") {
            const fstabBtn = document.createElement("button");
            fstabBtn.type = "button";
            fstabBtn.className = d.fstab_mount ? "btn small" : "btn small";
            fstabBtn.textContent = d.fstab_mount ? "取消开机挂载" : "固定到 /mnt";
            fstabBtn.title = d.fstab_mount
              ? "从 /etc/fstab 移除开机自动挂载"
              : "写入 /etc/fstab，每次开机自动挂载到 /mnt 目录";
            fstabBtn.addEventListener("click", () => {
              if (d.fstab_mount && devicesSudoNopass) {
                sendCommand(`devices:fstab-remove\ndev=${d.path}\npassword=`);
              } else {
                openFstabModal(d.fstab_mount ? "remove" : "set", d);
              }
            });
            actions.appendChild(fstabBtn);
          }
          if (d.removable && d.parent_disk) {
            const ejectBtn = document.createElement("button");
            ejectBtn.type = "button";
            ejectBtn.className = "btn small danger";
            ejectBtn.textContent = "安全弹出";
            ejectBtn.setAttribute("data-cmd", `devices:eject:${d.parent_disk}`);
            actions.appendChild(ejectBtn);
          }
          item.append(left, actions);
          diskList.appendChild(item);
        });
      }
    }

    const usbList = document.getElementById("dev-usb-list");
    if (usbList) {
      usbList.innerHTML = "";
      if (usb.length === 0) {
        usbList.innerHTML = '<div class="disk-empty">未检测到外接 USB 外设。</div>';
      } else {
        usb.forEach((u) => {
          const item = document.createElement("div");
          item.className = "disk-item";
          const left = document.createElement("div");
          const title = document.createElement("div"); title.className = "disk-title";
          title.textContent = u.name || u.vid_pid;
          const sub = document.createElement("div"); sub.className = "disk-sub";
          sub.textContent = `接口 ${u.bus_id} · ID ${u.vid_pid}${u.speed ? ` · ${u.speed}` : ""}`;
          left.append(title, sub);
          const tags = document.createElement("div"); tags.className = "disk-tags";
          const drv = document.createElement("span");
          drv.className = "tag";
          drv.textContent = u.driver ? `驱动: ${u.driver}` : "已连接";
          tags.appendChild(drv);
          item.append(left, tags);
          usbList.appendChild(item);
        });
      }
    }

    const audioList = document.getElementById("dev-audio-list");
    if (audioList) {
      audioList.innerHTML = "";
      if (audioDevs.length === 0) {
        audioList.innerHTML = '<div class="disk-empty">未检测到音频输入或输出设备。</div>';
      } else {
        audioDevs.forEach((ad) => {
          const item = document.createElement("div");
          item.className = "disk-item";
          const left = document.createElement("div");
          const title = document.createElement("div"); title.className = "disk-title";
          title.textContent = ad.label || ad.node;
          const sub = document.createElement("div"); sub.className = "disk-sub";
          const dirText = ad.kind === "source" ? "音频输入（麦克风）" : "音频输出（音响 / 耳机 / HDMI）";
          const stText = !ad.enabled ? "已禁用" : (ad.is_default ? "启用中 · 当前默认" : "启用中");
          sub.textContent = `${dirText} · ${stText}`;
          left.append(title, sub);

          const actions = document.createElement("div"); actions.className = "disk-tags";
          const stTag = document.createElement("span");
          stTag.className = "tag";
          stTag.textContent = ad.enabled ? (ad.kind === "source" ? "输入" : "输出") : "已禁用";
          const toggleBtn = document.createElement("button");
          toggleBtn.type = "button";
          if (ad.enabled) {
            toggleBtn.className = "btn small danger";
            toggleBtn.textContent = "禁用";
            toggleBtn.addEventListener("click", (ev) => {
              ev.stopPropagation();
              toggleBtn.disabled = true;
              toggleBtn.textContent = "应用中...";
              sendCommand([
                "devices:audio-disable",
                `node=${ad.node}`,
                `kind=${ad.kind}`,
                `label=${ad.label || ad.node}`
              ].join("\n"));
            });
          } else {
            toggleBtn.className = "btn small primary";
            toggleBtn.textContent = "启用";
            toggleBtn.addEventListener("click", (ev) => {
              ev.stopPropagation();
              toggleBtn.disabled = true;
              toggleBtn.textContent = "应用中...";
              sendCommand(`devices:audio-enable:${ad.node}`);
            });
          }
          actions.append(stTag, toggleBtn);
          item.append(left, actions);
          audioList.appendChild(item);
        });
      }
    }

    const inputList = document.getElementById("dev-input-list");
    if (inputList) {
      inputList.innerHTML = "";
      if (inputs.length === 0) {
        inputList.innerHTML = '<div class="disk-empty">未读取到外接键盘、鼠标或手柄。</div>';
      } else {
        inputs.forEach((inDev) => {
          const item = document.createElement("div");
          item.className = "disk-item";
          const left = document.createElement("div");
          const title = document.createElement("div"); title.className = "disk-title";
          title.textContent = inDev.name;
          const sub = document.createElement("div"); sub.className = "disk-sub";
          sub.textContent = `连接方式: ${inDev.bus}`;
          left.append(title, sub);
          const tags = document.createElement("div"); tags.className = "disk-tags";
          const kind = document.createElement("span");
          kind.className = "tag";
          kind.textContent = inDev.kind;
          tags.appendChild(kind);
          item.append(left, tags);
          inputList.appendChild(item);
        });
      }
    }
  };

  UI.hooks.switchSettingsTab = switchSettingsTab;
  UI.hooks.switchMonitorTab = switchMonitorTab;

  /* ---------------- Agent Parallel Sandbox ---------------- */
  let lastSandboxState = null;

  function renderSandboxState(data) {
    if (!data) return;
    lastSandboxState = data;
    const chip = document.getElementById("sandbox-status-chip");
    const homeInfo = document.getElementById("sandbox-home-info");
    const swNet = document.getElementById("sw-sandbox-net");
    const swClip = document.getElementById("sw-sandbox-clip");
    const selPerf = document.getElementById("sel-sandbox-perf");
    const dirList = document.getElementById("sandbox-dir-list");

    if (chip) {
      if (!data.bwrap_installed) chip.textContent = "未安装 bubblewrap";
      else if (data.running) chip.textContent = data.allow_network ? "运行中 · 共享网络" : "运行中 · 离线隔离";
      else chip.textContent = "已停止（按需自启）";
    }
    if (homeInfo) {
      homeInfo.textContent = `沙箱临时主目录: ${data.home_path || "--"} · 已占 ${data.home_size_mb || 0} MB`;
    }
    if (swNet) swNet.classList.toggle("is-on", Boolean(data.allow_network));
    if (swClip) swClip.classList.toggle("is-on", Boolean(data.sync_clipboard));
    if (selPerf && data.perf_mode) {
      selPerf.value = data.perf_mode;
      if (typeof enhanceSelect === "function") enhanceSelect(selPerf);
    }

    if (dirList) {
      dirList.innerHTML = "";
      const entries = [];
      (Array.isArray(data.rw_dirs) ? data.rw_dirs : []).forEach((p) => entries.push({ path: p, rw: true }));
      (Array.isArray(data.ro_dirs) ? data.ro_dirs : []).forEach((p) => entries.push({ path: p, rw: false }));
      if (entries.length === 0) {
        dirList.innerHTML = '<div class="perms-empty-row">暂无白名单目录（在文件管理器右键文件夹也可“在平行沙箱中打开工作区”）</div>';
      } else {
        entries.forEach((ent) => {
          const item = document.createElement("div");
          item.className = "perf-row";
          const left = document.createElement("div");
          left.className = "perf-row-info";
          const title = document.createElement("strong");
          title.textContent = ent.path;
          const sub = document.createElement("span");
          sub.className = "muted-hint";
          sub.textContent = ent.rw ? "可读写 (RW) · 沙箱内改动将持久保留到宿主对应目录" : "仅只读 (RO) · 沙箱内不可修改此目录文件";
          left.append(title, sub);
          const actions = document.createElement("div");
          actions.className = "footer-actions";
          const tag = document.createElement("span");
          tag.className = "tag";
          tag.textContent = ent.rw ? "可写" : "只读";
          const rmBtn = document.createElement("button");
          rmBtn.type = "button";
          rmBtn.className = "btn small danger";
          rmBtn.textContent = "移除";
          rmBtn.addEventListener("click", () => sendCommand(`sandbox:remove-dir\npath=${ent.path}`));
          actions.append(tag, rmBtn);
          item.append(left, actions);
          dirList.appendChild(item);
        });
      }
    }

    const appList = document.getElementById("sandbox-app-list");
    if (appList) {
      appList.innerHTML = "";
      const apps = Array.isArray(data.apps) ? data.apps : [];
      if (apps.length === 0) {
        appList.innerHTML = '<div class="perms-empty-row">暂未配置可启动应用，可在上方选择系统应用添加。</div>';
      } else {
        apps.forEach((app) => {
          const item = document.createElement("div");
          item.className = "perf-row";
          const left = document.createElement("div");
          left.className = "perf-row-info";
          const title = document.createElement("strong");
          title.textContent = app.name || app.cmd;
          const sub = document.createElement("span");
          sub.className = "muted-hint";
          sub.textContent = app.cmd;
          left.append(title, sub);

          const actions = document.createElement("div");
          actions.className = "footer-actions";
          const autoLbl = document.createElement("span");
          autoLbl.className = "muted-hint";
          autoLbl.style.marginRight = "4px";
          autoLbl.textContent = "默认自启";
          const autoSw = document.createElement("button");
          autoSw.type = "button";
          autoSw.className = "sp-switch" + (app.auto_start ? " is-on" : "");
          autoSw.innerHTML = '<span class="sp-switch-thumb"></span>';
          autoSw.addEventListener("click", (ev) => {
            ev.stopPropagation();
            sendCommand(`sandbox:app-autostart\ncmd=${app.cmd}\nauto_start=${app.auto_start ? "false" : "true"}`);
          });
          const runBtn = document.createElement("button");
          runBtn.type = "button";
          runBtn.className = "btn small";
          runBtn.textContent = "运行";
          runBtn.addEventListener("click", () => sendCommand(`sandbox:app-launch\ncmd=${app.cmd}`));
          const rmBtn = document.createElement("button");
          rmBtn.type = "button";
          rmBtn.className = "btn small danger";
          rmBtn.textContent = "移除";
          rmBtn.addEventListener("click", () => sendCommand(`sandbox:app-remove\ncmd=${app.cmd}`));
          actions.append(autoLbl, autoSw, runBtn, rmBtn);
          item.append(left, actions);
          appList.appendChild(item);
        });
      }
    }
  }

  window.__spinelSandboxCatalog = (items) => {
    const sel = document.getElementById("sel-sandbox-app-add");
    if (!sel || !Array.isArray(items)) return;
    const prev = sel.value;
    sel.innerHTML = "";
    items.forEach((it) => {
      const opt = document.createElement("option");
      opt.value = it.cmd;
      opt.textContent = it.name;
      opt.dataset.appName = it.name;
      sel.appendChild(opt);
    });
    if (prev && items.some((x) => x.cmd === prev)) sel.value = prev;
    if (typeof enhanceSelect === "function") enhanceSelect(sel);
  };

  window.__spinelSandboxData = renderSandboxState;
  window.__spinelSandboxFeedback = (ok, msg) => showFeedback("sandbox-feedback", msg || "", !ok);

  const btnSbStart = document.getElementById("btn-sandbox-start");
  if (btnSbStart) btnSbStart.addEventListener("click", () => sendCommand("sandbox:start"));
  const btnSbStop = document.getElementById("btn-sandbox-stop");
  if (btnSbStop) btnSbStop.addEventListener("click", () => sendCommand("sandbox:stop"));
  const btnSbReset = document.getElementById("btn-sandbox-reset");
  if (btnSbReset) btnSbReset.addEventListener("click", () => sendCommand("sandbox:reset"));

  const swSbNet = document.getElementById("sw-sandbox-net");
  if (swSbNet) {
    swSbNet.addEventListener("click", () => {
      const next = !(lastSandboxState && lastSandboxState.allow_network);
      sendCommand(`sandbox:config-set\nallow_network=${next ? "true" : "false"}`);
    });
  }
  const swSbClip = document.getElementById("sw-sandbox-clip");
  if (swSbClip) {
    swSbClip.addEventListener("click", () => {
      const next = !(lastSandboxState && lastSandboxState.sync_clipboard);
      sendCommand(`sandbox:config-set\nsync_clipboard=${next ? "true" : "false"}`);
    });
  }
  const selSbPerf = document.getElementById("sel-sandbox-perf");
  if (selSbPerf) {
    selSbPerf.addEventListener("change", () => {
      sendCommand(`sandbox:config-set\nperf_mode=${selSbPerf.value}`);
    });
  }
  const btnAddRw = document.getElementById("btn-sandbox-add-rw");
  if (btnAddRw) btnAddRw.addEventListener("click", () => sendCommand("sandbox:add-dir\nwritable=true"));
  const btnAddRo = document.getElementById("btn-sandbox-add-ro");
  if (btnAddRo) btnAddRo.addEventListener("click", () => sendCommand("sandbox:add-dir\nwritable=false"));
  const btnAppAdd = document.getElementById("btn-sandbox-app-add");
  if (btnAppAdd) {
    btnAppAdd.addEventListener("click", () => {
      const sel = document.getElementById("sel-sandbox-app-add");
      if (!sel || !sel.value) return;
      const opt = sel.options[sel.selectedIndex];
      const name = (opt && opt.dataset && opt.dataset.appName) || (opt && opt.textContent) || sel.value;
      sendCommand(`sandbox:app-add\nname=${name}\ncmd=${sel.value}\nauto_start=false`);
    });
  }

  /* ---------------- Cloud Relay & Multi-Device Continuity ---------------- */
  let lastRelayState = null;

  window.__spinelRelayFeedback = (ok, msg) => showFeedback("relay-feedback", msg || "", !ok);
  window.__spinelRelayPairCode = (code) => {
    const badge = document.getElementById("relay-paircode-display");
    if (!badge) return;
    badge.textContent = code ? `配对码: ${code}` : "";
    badge.classList.toggle("hidden", !code);
  };

  window.__spinelRelayData = (data, peers) => {
    if (!data) return;
    lastRelayState = data;
    const chip = document.getElementById("relay-status-chip");
    const authPanel = document.getElementById("relay-auth-panel");
    const onlinePanel = document.getElementById("relay-online-panel");
    const srvInput = document.getElementById("relay-server-input");
    const udpInput = document.getElementById("relay-udp-input");
    const fpInput = document.getElementById("relay-fp-input");
    const accInput = document.getElementById("relay-account-input");
    const devNameInput = document.getElementById("relay-device-name-input");
    const accLbl = document.getElementById("relay-account-label");
    const devLbl = document.getElementById("relay-device-label");
    const swAllow = document.getElementById("sw-relay-allow-remote");
    const hostDesc = document.getElementById("relay-hostd-desc");
    const listEl = document.getElementById("relay-device-list");

    const logged = Boolean(data.logged_in);
    if (chip) chip.textContent = logged ? `已登录 · ${data.account}` : "未登录";
    if (authPanel) authPanel.classList.toggle("hidden", logged);
    if (onlinePanel) onlinePanel.classList.toggle("hidden", !logged);

    if (srvInput && !srvInput.value && data.server_addr) srvInput.value = data.server_addr;
    if (udpInput && data.udp_port) udpInput.value = String(data.udp_port);
    if (fpInput && data.server_fp) fpInput.value = data.server_fp;
    if (accInput && !accInput.value && data.account) accInput.value = data.account;

    if (accLbl) accLbl.textContent = `当前账号: ${data.account || "--"} · 服务器 ${data.server_addr || "--"}`;
    if (devLbl) devLbl.textContent = `本机名: ${data.device_name || "--"} · 设备ID: ${data.device_id || "--"}`;
    if (devNameInput && document.activeElement !== devNameInput && data.device_name) {
      devNameInput.value = data.device_name;
    }
    if (swAllow) swAllow.classList.toggle("is-on", Boolean(data.allow_remote));
    if (hostDesc) {
      hostDesc.textContent = data.allow_remote
        ? (data.hostd_running ? "宿主服务在线待命中 · 其他同账号设备可一键远程画中画接管本机沙箱" : "已开启远程权限 · 正在后台连接服务器")
        : "关闭中 · 仅本机使用，不对其他同账号设备暴露";
    }

    if (!listEl) return;
    listEl.innerHTML = "";
    const devArr = Array.isArray(peers) ? peers : [];
    if (!logged) {
      listEl.innerHTML = '<div class="disk-empty">登录后这里会显示同一账号下的所有 SpinelOS 设备</div>';
      return;
    }
    if (devArr.length === 0) {
      listEl.innerHTML = '<div class="disk-empty">暂无设备记录（点击「刷新同账号设备」同步列表）</div>';
      return;
    }
    devArr.forEach((d) => {
      const isSelf = d.device_id === data.device_id;
      const item = document.createElement("div");
      item.className = "disk-item";
      const left = document.createElement("div");
      const title = document.createElement("div");
      title.className = "disk-title";
      title.textContent = `${d.name || d.device_id}${isSelf ? "（本机）" : ""}`;
      const sub = document.createElement("div");
      sub.className = "disk-sub";
      const netMeta = `LAN:${d.lan_count || 0} / IPv6:${d.ipv6_count || 0}${d.udp_mapped ? " / STUN已探测" : ""}`;
      sub.textContent = `${d.online ? "在线" : "离线"} · ${d.allow_remote ? "允许远程接管" : "仅主控端"} · ${netMeta}`;
      left.append(title, sub);

      const actions = document.createElement("div");
      actions.className = "disk-tags";
      if (!isSelf) {
        const connBtn = document.createElement("button");
        connBtn.type = "button";
        connBtn.className = "btn small primary";
        connBtn.textContent = "远程画中画接管";
        connBtn.disabled = !(d.online && d.allow_remote);
        connBtn.addEventListener("click", () => sendCommand(`relay:connect:${d.device_id}`));
        const rmBtn = document.createElement("button");
        rmBtn.type = "button";
        rmBtn.className = "btn small danger";
        rmBtn.textContent = "解绑";
        rmBtn.addEventListener("click", () => sendCommand(`relay:device-remove\ndevice_id=${d.device_id}`));
        actions.append(connBtn, rmBtn);
      } else {
        const selfTag = document.createElement("span");
        selfTag.className = "tag";
        selfTag.textContent = "当前设备";
        actions.appendChild(selfTag);
      }
      item.append(left, actions);
      listEl.appendChild(item);
    });
  };

  function collectRelayAuthPayload() {
    const s = (document.getElementById("relay-server-input")?.value || "").trim();
    const u = (document.getElementById("relay-udp-input")?.value || "7444").trim();
    const f = (document.getElementById("relay-fp-input")?.value || "").trim();
    const a = (document.getElementById("relay-account-input")?.value || "").trim();
    const p = document.getElementById("relay-password-input")?.value || "";
    return `server=${s}\nudp_port=${u}\nfp=${f}\naccount=${a}\npassword=${p}`;
  }

  document.getElementById("btn-relay-login")?.addEventListener("click", () => {
    sendCommand(`relay:login\n${collectRelayAuthPayload()}`);
  });
  document.getElementById("btn-relay-register")?.addEventListener("click", () => {
    sendCommand(`relay:register\n${collectRelayAuthPayload()}`);
  });
  document.getElementById("btn-relay-pair-claim")?.addEventListener("click", () => {
    const s = (document.getElementById("relay-server-input")?.value || "").trim();
    const u = (document.getElementById("relay-udp-input")?.value || "7444").trim();
    const f = (document.getElementById("relay-fp-input")?.value || "").trim();
    const c = (document.getElementById("relay-paircode-input")?.value || "").trim();
    sendCommand(`relay:pair-claim\nserver=${s}\nudp_port=${u}\nfp=${f}\ncode=${c}`);
  });
  document.getElementById("btn-relay-reset-fp")?.addEventListener("click", () => {
    const fpEl = document.getElementById("relay-fp-input");
    if (fpEl) fpEl.value = "";
    sendCommand("relay:reset-fp");
  });
  document.getElementById("btn-relay-logout")?.addEventListener("click", () => sendCommand("relay:logout"));
  document.getElementById("btn-relay-pair-create")?.addEventListener("click", () => sendCommand("relay:pair-create"));
  document.getElementById("btn-relay-refresh")?.addEventListener("click", () => sendCommand("relay:state-get"));
  document.getElementById("sw-relay-allow-remote")?.addEventListener("click", () => {
    const next = !(lastRelayState && lastRelayState.allow_remote);
    sendCommand(`relay:set-allow-remote\nallow=${next ? "true" : "false"}`);
  });
  const submitRelayRename = () => {
    const nm = (document.getElementById("relay-device-name-input")?.value || "").trim();
    if (!nm) return;
    sendCommand(`relay:set-device-name\nname=${nm}`);
  };
  document.getElementById("btn-relay-rename")?.addEventListener("click", submitRelayRename);
  document.getElementById("relay-device-name-input")?.addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") {
      ev.preventDefault();
      submitRelayRename();
    }
  });
})();
