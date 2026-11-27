(() => {
  const UI = window.SpinelUI;
  const { state, sendCommand, showFeedback } = UI;
  const { formatLabwcKey, eventToLabwcKey, buildKvCommand } = UI.helpers;

  let procTimerId = null;
  let currentSettingsTab = "";

  /* ---------------- Settings & Monitor Navigation ---------------- */
  function switchSettingsTab(tab) {
    const meta = {
      wifi: { title: "Wi-Fi 与网络", sub: "管理网络接口、无线热点与全局代理" },
      bt: { title: "蓝牙手柄", sub: "配对并管理手柄、耳机等蓝牙外设" },
      relay: { title: "云接力与远程沙箱", sub: "Spinel ID 跨设备互联与平行沙箱远程接管" },
      display: { title: "显示器", sub: "设置输出分辨率、刷新率与桌面缩放比例" },
      devices: { title: "设备管理器", sub: "挂载磁盘分区、查看 USB、输入与音频硬件" },
      audio: { title: "声音与电源", sub: "调节主音量、麦克风增益、性能档位与锁屏" },
      shortcuts: { title: "键盘与快捷键", sub: "自定义全局组合键并查看内置桌面快捷键" },
      perf: { title: "游戏性能", sub: "配置 MangoHud、帧率上限、FSR 与 GameMode" },
      appearance: { title: "外观与输入法", sub: "主题风格、桌面壁纸、Dock 栏与中文输入法" },
      updates: { title: "系统更新", sub: "Spinel OS 在线/离线升级与软件包镜像维护" },
      perms: { title: "权限与安全", sub: "AI Agent 能力边界、平行沙箱与系统授权保护" },
      snapshots: { title: "系统快照", sub: "通过 Btrfs Snapper 创建与回滚系统还原点" }
    };
    const tabs = Object.keys(meta);
    const target = tabs.includes(tab) ? tab : "wifi";
    if (currentSettingsTab === "audio" && target !== "audio") {
      sendCommand("settings:audio-test-mic:stop");
      sendCommand("settings:audio-test-speaker:stop");
    }
    currentSettingsTab = target;
    tabs.forEach((t) => {
      const body = document.getElementById(`tab-${t}`);
      if (body) {
        body.classList.toggle("hidden", t !== target);
        if (t === target) body.scrollTop = 0;
      }
    });
    const titleEl = document.getElementById("settings-page-title");
    if (titleEl) titleEl.textContent = meta[target]?.title || "系统设置";
    const subEl = document.getElementById("settings-page-sub");
    if (subEl) subEl.textContent = meta[target]?.sub || "";
    document.querySelectorAll("#app-settings [data-cmd^='settings:tab:']").forEach((btn) => {
      const active = btn.getAttribute("data-cmd") === `settings:tab:${target}`;
      btn.classList.toggle("active", active);
      btn.setAttribute("aria-selected", String(active));
      if (active) btn.scrollIntoView({ block: "nearest" });
    });

    const tabRefreshCmds = {
      wifi: ["settings:wifi-list"],
      bt: ["settings:bt-status"],
      relay: ["relay:state-get"],
      display: ["settings:display-scan"],
      devices: ["settings:devices-get"],
      audio: ["settings:audio-get"],
      shortcuts: ["settings:shortcuts-get"],
      perf: ["perf:get"],
      appearance: ["theme:refresh", "dock:config-get", "ime:status"],
      updates: ["ota:check", "store:refresh"],
      perms: ["settings:perms-get"],
      snapshots: ["settings:snapshots-get"]
    };
    (tabRefreshCmds[target] || []).forEach((cmd) => sendCommand(cmd));
  }

  function switchMonitorTab(tab, logSource) {
    const target = tab === "logs" ? "logs" : "procs";
    if (logSource) state.activeLogSource = logSource;
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
      sendCommand(`monitor:logs:${state.activeLogSource}`);
    }
  }

  document.querySelectorAll("#monitor-tab-group [data-monitor-tab]").forEach((btn) => {
    btn.addEventListener("click", () => {
      switchMonitorTab(btn.getAttribute("data-monitor-tab") || "procs");
    });
  });

  window.__spinelOpenSettings = (tab) => switchSettingsTab(tab);
  window.__spinelOpenMonitor = (tab, logSource) => switchMonitorTab(tab, logSource);
  UI.hooks.switchSettingsTab = switchSettingsTab;
  UI.hooks.switchMonitorTab = switchMonitorTab;

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
      const spkLabels = {
        left: "正在播放左声道测试音（仅左声道发声）...",
        right: "正在播放右声道测试音（仅右声道发声）...",
        stereo: "正在播放立体声测试音（双声道均衡）..."
      };
      spkStatus.textContent = spkLabels[spkMode] || data.audio_test_hint || "点击测试左/右声道或立体声扬声器输出";
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
    sendCommand(buildKvCommand("settings:shortcuts-set", { [targetField]: labwcKey }));
  }, true);

  /* ---------------- Simplified EN/ZH IME ---------------- */
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
      chip.classList.toggle("ok", Boolean(data.installed && data.running));
      chip.classList.toggle("warn", !data.installed || !data.running);
    }
    document.getElementById("btn-ime-en")?.classList.toggle("active", !data.active_zh);
    document.getElementById("btn-ime-zh")?.classList.toggle("active", Boolean(data.active_zh));
    document.getElementById("btn-ime-simp")?.classList.toggle("active", !data.traditional);
    document.getElementById("btn-ime-trad")?.classList.toggle("active", Boolean(data.traditional));

    const iceBtn = document.getElementById("btn-ime-ice");
    if (iceBtn) {
      iceBtn.textContent = data.rime_ice ? "更新雾凇词库" : "安装雾凇词库（增强联想）";
      iceBtn.classList.toggle("primary", !data.rime_ice);
    }
  };

  window.__spinelImeFeedback = (ok, msg) => showFeedback("ime-feedback", msg || "", !ok);
})();
