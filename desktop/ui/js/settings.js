(() => {
  const UI = window.SpinelUI;
  const { state, sendCommand, showFeedback, enhanceSelect } = UI;

  let activeLogSource = "apps";
  let procTimerId = null;
  let lastProcPayload = null;
  let procSortBy = "cpu";

  function switchSettingsTab(tab) {
    const titles = {
      wifi: "Wi-Fi 与网络",
      bt: "蓝牙手柄",
      display: "显示器",
      audio: "声音与电源",
      shortcuts: "键盘与快捷键",
      perf: "游戏性能",
      appearance: "外观与输入法",
      perms: "权限与安全",
      procs: "进程监控",
      logs: "系统日志"
    };
    const tabs = Object.keys(titles);
    const target = tabs.includes(tab) ? tab : "wifi";
    tabs.forEach((t) => {
      const body = document.getElementById(`tab-${t}`);
      if (body) body.classList.toggle("hidden", t !== target);
    });
    const titleEl = document.getElementById("settings-page-title");
    if (titleEl) titleEl.textContent = titles[target] || "系统设置";
    document.querySelectorAll("#app-settings [data-cmd^='settings:tab:']").forEach((btn) => {
      btn.classList.toggle("active", btn.getAttribute("data-cmd") === `settings:tab:${target}`);
    });
    if (procTimerId) {
      clearInterval(procTimerId);
      procTimerId = null;
    }
    if (target === "wifi") sendCommand("settings:wifi-list");
    else if (target === "bt") sendCommand("settings:bt-status");
    else if (target === "display") sendCommand("settings:display-scan");
    else if (target === "audio") sendCommand("settings:audio-get");
    else if (target === "shortcuts") sendCommand("settings:shortcuts-get");
    else if (target === "perf") sendCommand("perf:get");
    else if (target === "appearance") {
      sendCommand("theme:refresh");
      sendCommand("ime:status");
    }
    else if (target === "perms") sendCommand("settings:perms-get");
    else if (target === "procs") {
      sendCommand("settings:procs-get");
      procTimerId = setInterval(() => sendCommand("settings:procs-get"), 2000);
    }
    else if (target === "logs") {
      sendCommand("settings:doctor-scan");
      sendCommand(`settings:logs:${activeLogSource}`);
    }
  }

  window.__spinelOpenSettings = (tab) => switchSettingsTab(tab);

  /* ---------------- Permissions & Security ---------------- */
  window.__spinelPermsFeedback = (ok, msg) => showFeedback("perms-feedback", msg || "", !ok);

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
        appList.innerHTML = '<div class="disk-empty">目前没有需要单独限权的沙箱应用（从「应用商店」安装浏览器、Discord、OBS、Heroic 等软件后会在这里出现开关）。</div>';
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
          const card = document.createElement("div");
          card.className = "part-section";
          card.style.marginBottom = "10px";

          const head = document.createElement("div");
          head.className = "section-head";
          head.style.marginBottom = "8px";
          const titleWrap = document.createElement("div");
          const strong = document.createElement("strong");
          strong.textContent = app.name || app.id;
          const small = document.createElement("div");
          small.className = "muted-hint";
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

          card.append(head, pills);
          appList.appendChild(card);
        });
      }
    }

    const portalList = document.getElementById("perms-portal-list");
    if (portalList) {
      portalList.innerHTML = "";
      const entries = Array.isArray(data.portal_entries) ? data.portal_entries : [];
      if (entries.length === 0) {
        portalList.innerHTML = '<div class="disk-empty">暂无已记住的弹窗记录（外部软件弹窗询问并选择“允许/拒绝”后会显示在这里）。</div>';
      } else {
        entries.forEach((pe) => {
          const row = document.createElement("div");
          row.className = "status-row";
          const info = document.createElement("div");
          const title = document.createElement("strong");
          title.textContent = `${pe.app} · ${pe.table}/${pe.id}`;
          const sub = document.createElement("div");
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
  function renderProcessList() {
    const listEl = document.getElementById("procs-list");
    if (!listEl || !lastProcPayload) return;

    const cpuChip = document.getElementById("procs-cpu-chip");
    const memChip = document.getElementById("procs-mem-chip");
    const cntChip = document.getElementById("procs-cnt-chip");
    if (cpuChip) cpuChip.textContent = `CPU: ${lastProcPayload.cpu_pct ?? "--"}%`;
    if (memChip) memChip.textContent = `内存: ${lastProcPayload.mem || "--"}`;
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
        const engText = data.engine === "rime"
          ? (data.rime_ice ? "Rime 雾凇拼音" : "Rime 明月拼音 (简体)")
          : (data.engine === "pinyin" ? "内置中文拼音" : "未配置中文方案");
        const rssText = (data.running && data.rss_mb > 0) ? ` · ${data.rss_mb} MB` : "";
        chip.textContent = (data.running ? "运行中 · " : "未启动 · ") + engText + rssText;
      }
    }
    const pinyinBtn = document.getElementById("btn-ime-pinyin");
    if (pinyinBtn) {
      pinyinBtn.classList.toggle("active", data.engine === "pinyin" || (data.engine === "rime" && !data.rime_ice));
      pinyinBtn.textContent = data.has_pinyin ? "内置中文拼音 (默认)" : "Rime 明月拼音·简体 (内置)";
    }
    const rimeBtn = document.getElementById("btn-ime-rime");
    if (rimeBtn) {
      rimeBtn.classList.toggle("active", data.engine === "rime" && Boolean(data.rime_ice));
      rimeBtn.classList.toggle("hidden", !data.rime_ice);
    }
    document.getElementById("btn-ime-config")?.classList.toggle("hidden", !data.has_configtool);
    const iceBtn = document.getElementById("btn-ime-ice");
    if (iceBtn) {
      iceBtn.textContent = data.rime_ice ? "更新雾凇拼音词库" : "安装并启用雾凇拼音 (rime-ice)";
      iceBtn.classList.toggle("primary", !data.rime_ice);
    }
  };

  window.__spinelImeFeedback = (ok, msg) => showFeedback("ime-feedback", msg || "", !ok);

  window.__spinelLogsData = (data) => {
    if (!data) return;
    if (data.source) activeLogSource = data.source;
    document.querySelectorAll("#logs-source-tabs .pill-btn").forEach((b) => {
      b.classList.toggle("active", b.getAttribute("data-cmd") === `settings:logs:${activeLogSource}`);
    });
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
    sendCommand(`settings:logs:${activeLogSource}`);
  });
  document.getElementById("btn-logs-clear")?.addEventListener("click", () => {
    sendCommand(`settings:logs-clear:${activeLogSource}`);
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
    if (chip) chip.textContent = data.enabled ? "已启用" : "已关闭";
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
          const sub = document.createElement("div"); sub.className = "disk-sub"; sub.textContent = ifx.connection ? `当前连接: ${ifx.connection}` : `状态: ${ifx.state}`;
          left.append(t, sub);
          const right = document.createElement("div"); right.className = "disk-tags";
          const tag = document.createElement("span"); tag.className = "tag"; tag.textContent = ifx.state;
          right.appendChild(tag);
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

  window.__spinelWifiAction = (ok, msg) => {
    showFeedback("wifi-feedback", msg || (ok ? "操作成功" : "操作失败"), !ok);
    if (ok) document.getElementById("wifi-connect-box")?.classList.add("hidden");
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

  UI.hooks.switchSettingsTab = switchSettingsTab;
})();
