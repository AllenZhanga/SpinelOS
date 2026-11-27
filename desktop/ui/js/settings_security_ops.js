(() => {
  const UI = window.SpinelUI;
  const { sendCommand, showFeedback, setSwitchState, createSwitchButton, syncSelectValue, bindModalDismiss } = UI;
  const { buildKvCommand, buildMcpConfigSnippet, formatSnapStatusChip } = UI.helpers;

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
      sendCommand(buildKvCommand("settings:snapshot-create", {
        desc: op.desc || "SpinelOS 手动还原点",
        password
      }));
    } else if (op.action === "rollback") {
      sendCommand(buildKvCommand("settings:snapshot-rollback", { id: op.id, password }));
    } else if (op.action === "delete") {
      sendCommand(buildKvCommand("settings:snapshot-delete", { id: op.id, password }));
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

  bindModalDismiss("snap-pass-modal", "btn-snap-pass-cancel", closeSnapModal);
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

    const st = formatSnapStatusChip(data);
    if (chip) {
      chip.textContent = st.text;
      chip.classList.toggle("ok", st.ready);
      chip.classList.toggle("warn", !st.ready);
    }
    if (createBtn) createBtn.disabled = !st.ready;

    if (!listEl) return;
    listEl.innerHTML = "";
    if (!st.ready) {
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

  /* ---------------- Permissions, AI MCP & Security ---------------- */
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
    const info = buildMcpConfigSnippet(t, lastAiMcpClients[t]);
    pre.textContent = info.snippet;

    const pathEl = document.getElementById("ai-mcp-path-hint");
    if (pathEl) pathEl.textContent = `配置文件: ${info.path}`;
    const chipEl = document.getElementById("ai-mcp-install-chip");
    if (chipEl) {
      chipEl.textContent = info.installed ? "已接入" : "未写入";
      chipEl.classList.toggle("ok", info.installed);
      chipEl.classList.toggle("warn", !info.installed);
    }
    const instBtn = document.getElementById("btn-ai-install-mcp");
    if (instBtn) instBtn.textContent = info.buttonText;
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
      setSwitchState(btn, Boolean(lastAiPerms[k]));
    });

    syncSelectValue(document.getElementById("sel-ai-tier3-policy"), lastAiPerms.tier3_policy);
    renderAiMcpSnippet();
  };

  document.querySelectorAll("[data-ai-perm]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const k = btn.getAttribute("data-ai-perm");
      if (!k) return;
      const next = !Boolean(lastAiPerms[k]);
      sendCommand(buildKvCommand("ai:perms-set", { [k]: next ? "true" : "false" }));
    });
  });

  document.getElementById("sel-ai-tier3-policy")?.addEventListener("change", (ev) => {
    sendCommand(buildKvCommand("ai:perms-set", { tier3_policy: ev.target.value || "ask" }));
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
            const sw = createSwitchButton(on, `${app.name || app.id} ${pd.label}`, (ev) => {
              ev.stopPropagation();
              sendCommand(buildKvCommand("perms:set", {
                app: app.id,
                key: pd.key,
                val: on ? "false" : "true"
              }));
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
            sendCommand(buildKvCommand("perms:portal-reset", { table: pe.table, id: pe.id, app: pe.app }));
          });
          row.append(info, rev);
          portalList.appendChild(row);
        });
      }
    }
  };

  /* ---------------- Agent Parallel Sandbox ---------------- */
  let lastSandboxState = null;

  function renderSandboxState(data) {
    if (!data) return;
    lastSandboxState = data;
    const chip = document.getElementById("sandbox-status-chip");
    const homeInfo = document.getElementById("sandbox-home-info");
    const dirList = document.getElementById("sandbox-dir-list");

    if (chip) {
      if (!data.bwrap_installed) chip.textContent = "未安装 bubblewrap";
      else if (data.running) chip.textContent = data.allow_network ? "运行中 · 共享网络" : "运行中 · 离线隔离";
      else chip.textContent = "已停止（按需自启）";
      chip.classList.toggle("ok", Boolean(data.bwrap_installed && data.running));
      chip.classList.toggle("warn", !data.bwrap_installed);
    }
    if (homeInfo) {
      homeInfo.textContent = `沙箱临时主目录: ${data.home_path || "--"} · 已占 ${data.home_size_mb || 0} MB`;
    }
    setSwitchState(document.getElementById("sw-sandbox-net"), data.allow_network);
    setSwitchState(document.getElementById("sw-sandbox-clip"), data.sync_clipboard);
    syncSelectValue(document.getElementById("sel-sandbox-perf"), data.perf_mode);

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
          const autoSw = createSwitchButton(Boolean(app.auto_start), `${app.name || app.cmd} 默认自启`, (ev) => {
            ev.stopPropagation();
            sendCommand(buildKvCommand("sandbox:app-autostart", {
              cmd: app.cmd,
              auto_start: app.auto_start ? "false" : "true"
            }));
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
    syncSelectValue(sel, prev && items.some((x) => x.cmd === prev) ? prev : "");
  };

  window.__spinelSandboxData = renderSandboxState;
  window.__spinelSandboxFeedback = (ok, msg) => showFeedback("sandbox-feedback", msg || "", !ok);

  document.getElementById("btn-sandbox-start")?.addEventListener("click", () => sendCommand("sandbox:start"));
  document.getElementById("btn-sandbox-stop")?.addEventListener("click", () => sendCommand("sandbox:stop"));
  document.getElementById("btn-sandbox-reset")?.addEventListener("click", () => sendCommand("sandbox:reset"));
  document.getElementById("sw-sandbox-net")?.addEventListener("click", () => {
    const next = !(lastSandboxState && lastSandboxState.allow_network);
    sendCommand(buildKvCommand("sandbox:config-set", { allow_network: next ? "true" : "false" }));
  });
  document.getElementById("sw-sandbox-clip")?.addEventListener("click", () => {
    const next = !(lastSandboxState && lastSandboxState.sync_clipboard);
    sendCommand(buildKvCommand("sandbox:config-set", { sync_clipboard: next ? "true" : "false" }));
  });
  const selSbPerf = document.getElementById("sel-sandbox-perf");
  selSbPerf?.addEventListener("change", () => {
    sendCommand(buildKvCommand("sandbox:config-set", { perf_mode: selSbPerf.value }));
  });
  document.getElementById("btn-sandbox-add-rw")?.addEventListener("click", () => sendCommand("sandbox:add-dir\nwritable=true"));
  document.getElementById("btn-sandbox-add-ro")?.addEventListener("click", () => sendCommand("sandbox:add-dir\nwritable=false"));
  document.getElementById("btn-sandbox-app-add")?.addEventListener("click", () => {
    const sel = document.getElementById("sel-sandbox-app-add");
    if (!sel || !sel.value) return;
    const opt = sel.options[sel.selectedIndex];
    const name = (opt && opt.dataset && opt.dataset.appName) || (opt && opt.textContent) || sel.value;
    sendCommand(buildKvCommand("sandbox:app-add", { name, cmd: sel.value, auto_start: "false" }));
  });

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
    const hostDesc = document.getElementById("relay-hostd-desc");
    const listEl = document.getElementById("relay-device-list");

    const logged = Boolean(data.logged_in);
    if (chip) {
      chip.textContent = logged ? `已登录 · ${data.account}` : "未登录";
      chip.classList.toggle("ok", logged);
    }
    authPanel?.classList.toggle("hidden", logged);
    onlinePanel?.classList.toggle("hidden", !logged);

    if (srvInput && !srvInput.value && data.server_addr) srvInput.value = data.server_addr;
    if (udpInput && data.udp_port) udpInput.value = String(data.udp_port);
    if (fpInput && data.server_fp) fpInput.value = data.server_fp;
    if (accInput && !accInput.value && data.account) accInput.value = data.account;

    if (accLbl) accLbl.textContent = `当前账号: ${data.account || "--"} · 服务器 ${data.server_addr || "--"}`;
    if (devLbl) devLbl.textContent = `本机名: ${data.device_name || "--"} · 设备ID: ${data.device_id || "--"}`;
    if (devNameInput && document.activeElement !== devNameInput && data.device_name) {
      devNameInput.value = data.device_name;
    }
    setSwitchState(document.getElementById("sw-relay-allow-remote"), data.allow_remote);
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

  function collectRelayAuthFields() {
    return {
      server: (document.getElementById("relay-server-input")?.value || "").trim(),
      udp_port: (document.getElementById("relay-udp-input")?.value || "7444").trim(),
      fp: (document.getElementById("relay-fp-input")?.value || "").trim(),
      account: (document.getElementById("relay-account-input")?.value || "").trim(),
      password: document.getElementById("relay-password-input")?.value || ""
    };
  }

  document.getElementById("btn-relay-login")?.addEventListener("click", () => {
    sendCommand(buildKvCommand("relay:login", collectRelayAuthFields()));
  });
  document.getElementById("btn-relay-register")?.addEventListener("click", () => {
    sendCommand(buildKvCommand("relay:register", collectRelayAuthFields()));
  });
  document.getElementById("btn-relay-pair-claim")?.addEventListener("click", () => {
    const auth = collectRelayAuthFields();
    sendCommand(buildKvCommand("relay:pair-claim", {
      server: auth.server,
      udp_port: auth.udp_port,
      fp: auth.fp,
      code: (document.getElementById("relay-paircode-input")?.value || "").trim()
    }));
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
    sendCommand(buildKvCommand("relay:set-allow-remote", { allow: next ? "true" : "false" }));
  });
  const submitRelayRename = () => {
    const nm = (document.getElementById("relay-device-name-input")?.value || "").trim();
    if (nm) sendCommand(`relay:set-device-name\nname=${nm}`);
  };
  document.getElementById("btn-relay-rename")?.addEventListener("click", submitRelayRename);
  document.getElementById("relay-device-name-input")?.addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") {
      ev.preventDefault();
      submitRelayRename();
    }
  });
})();
