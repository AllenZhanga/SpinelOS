(() => {
  const UI = window.SpinelUI;
  const {
    mode,
    initialTab,
    activeRootEl,
    state,
    hooks,
    sendCommand,
    formatBytes,
    showFeedback,
    syncAllCustomSelects,
    renderDesktopShortcuts,
    renderDock
  } = UI;

  /* ---------------- 6. Graphical Disk Installer & Welcome OOBE ---------------- */
  function showFormError(msg) {
    const el = document.getElementById("inst-form-error");
    if (!el) return;
    el.classList.toggle("hidden", !msg);
    el.textContent = msg || "";
  }

  function setInstallMode(nextMode) {
    state.installMode = nextMode === "partition" ? "partition" : "disk";
    document.getElementById("inst-disk-section")?.classList.toggle("hidden", state.installMode !== "disk");
    document.getElementById("inst-part-section")?.classList.toggle("hidden", state.installMode !== "partition");
    document.getElementById("btn-mode-disk")?.classList.toggle("active", state.installMode === "disk");
    document.getElementById("btn-mode-part")?.classList.toggle("active", state.installMode === "partition");
    const confirmLabel = document.getElementById("inst-confirm-label");
    if (confirmLabel) {
      confirmLabel.textContent = state.installMode === "disk"
        ? "我已确认所选硬盘无重要数据，允许清空整块硬盘并安装 SpinelOS（测试建议优先使用空盘或副盘）。"
        : "我已确认所选根分区 (/) 无重要数据，允许将其格式化为 ext4（其余分区与原 EFI 文件将保留）。";
    }
    showFormError("");
  }

  let installerSizes = new Map();

  function submitInstaller() {
    showFormError("");
    const rootPart = document.getElementById("inst-root-part")?.value || "";
    const efiPart = document.getElementById("inst-efi-part")?.value || "";
    const osProber = document.getElementById("inst-os-prober")?.checked ?? true;
    const fsType = document.getElementById("inst-fs")?.value || "ext4";
    const MIN_BYTES = 20 * 1024 * 1024 * 1024;

    if (state.installMode === "disk") {
      if (!state.selectedDisk) { showFormError("请先选择一个目标硬盘。"); return; }
      const sz = installerSizes.get(state.selectedDisk) || 0;
      if (sz > 0 && sz < MIN_BYTES) { showFormError("所选硬盘容量不足 20 GiB（建议 ≥ 25 GB），无法安装 SpinelOS。"); return; }
    } else {
      if (!rootPart) { showFormError("请选择要格式化为根分区 (/) 的目标分区。"); return; }
      const sz = installerSizes.get(rootPart) || 0;
      if (sz > 0 && sz < MIN_BYTES) { showFormError("所选根分区容量不足 20 GiB（建议 ≥ 25 GB），无法安装 SpinelOS。"); return; }
      if (state.isUefiMode && !efiPart) { showFormError("UEFI 模式下请选择一个已有的 FAT32 EFI 分区。"); return; }
      if (state.isUefiMode && rootPart === efiPart) { showFormError("根分区和 EFI 分区不能选择同一个分区。"); return; }
    }

    const username = document.getElementById("inst-username").value.trim();
    const hostname = document.getElementById("inst-hostname").value.trim();
    const password = document.getElementById("inst-password").value;
    const password2 = document.getElementById("inst-password2").value;
    const timezone = document.getElementById("inst-timezone").value;
    const autologin = document.getElementById("inst-autologin").checked;
    const confirmWipe = document.getElementById("inst-confirm-wipe").checked;

    if (!/^[a-z_][a-z0-9_-]{0,30}$/.test(username)) { showFormError("用户名格式无效。"); return; }
    if (!/^[a-zA-Z0-9][a-zA-Z0-9-]{0,62}$/.test(hostname)) { showFormError("主机名格式无效。"); return; }
    if (password.length < 4 || /[\r\n]/.test(password)) { showFormError("密码长度至少为 4 位。"); return; }
    if (password !== password2) { showFormError("两次输入的密码不一致。"); return; }
    if (!confirmWipe) { showFormError("请勾选确认框以允许格式化目标位置并开始安装。"); return; }

    state.installRunning = true;
    document.getElementById("inst-form-view").classList.add("hidden");
    document.getElementById("inst-progress-view").classList.remove("hidden");
    document.getElementById("btn-inst-back").classList.add("hidden");
    document.getElementById("btn-inst-reboot").classList.add("hidden");
    document.getElementById("inst-step-text").textContent = "正在启动安装程序...";
    document.getElementById("inst-pct-text").textContent = "0%";
    document.getElementById("inst-progress-bar").style.width = "0%";
    document.getElementById("inst-log-box").textContent = "";
    document.getElementById("inst-status-hint").textContent = "正在执行安装，请勿断电或拔出安装介质...";

    sendCommand([
      "installer:start",
      `mode=${state.installMode}`,
      `fs=${fsType}`,
      `disk=${state.selectedDisk}`,
      `root_part=${rootPart}`,
      `efi_part=${efiPart}`,
      `os_prober=${osProber ? "true" : "false"}`,
      `username=${username}`,
      `hostname=${hostname}`,
      `timezone=${timezone}`,
      `autologin=${autologin ? "true" : "false"}`,
      `password=${password}`
    ].join("\n"));
  }

  window.__spinelInstallerEnv = (env) => {
    state.installRunning = Boolean(env.installing);
    state.isUefiMode = Boolean(env.uefi);
    const badge = document.getElementById("inst-env-badge");
    if (badge) badge.textContent = `${env.is_live ? "Live ISO" : "已安装环境"} · ${env.uefi ? "UEFI GPT" : "Legacy BIOS"}`;
    document.getElementById("inst-efi-wrap")?.classList.toggle("hidden", !state.isUefiMode);

    const listEl = document.getElementById("inst-disk-list");
    if (!listEl) return;
    const rawDevices = env.lsblk && Array.isArray(env.lsblk.blockdevices) ? env.lsblk.blockdevices : [];
    const disks = rawDevices.filter((d) => d.type === "disk" && !/^loop|^zram/.test(d.name || ""));
    installerSizes = new Map();
    disks.forEach((d) => {
      installerSizes.set(`/dev/${d.name}`, Number(d.size) || 0);
      if (Array.isArray(d.children)) {
        d.children.forEach((p) => installerSizes.set(p.path || `/dev/${p.name}`, Number(p.size) || 0));
      }
    });

    const rootSel = document.getElementById("inst-root-part");
    const efiSel = document.getElementById("inst-efi-part");
    if (rootSel) rootSel.innerHTML = "";
    if (efiSel) efiSel.innerHTML = "";

    const nonBootParts = [];
    disks.forEach((d) => {
      if (env.boot_disk && d.name === env.boot_disk) return;
      if (!Array.isArray(d.children)) return;
      d.children.forEach((p) => { if (p.type === "part") nonBootParts.push(p); });
    });

    if (rootSel) {
      if (nonBootParts.length === 0) {
        const opt = document.createElement("option"); opt.value = ""; opt.textContent = "未检测到可用分区"; rootSel.appendChild(opt);
      } else {
        nonBootParts.forEach((p) => {
          const path = p.path || `/dev/${p.name}`;
          const opt = document.createElement("option");
          opt.value = path;
          opt.textContent = `${path} · ${formatBytes(p.size)} · ${p.fstype || "未格式化"}`;
          rootSel.appendChild(opt);
        });
      }
    }
    if (efiSel) {
      const vfat = nonBootParts.filter((p) => (p.fstype || "").toLowerCase() === "vfat");
      if (vfat.length === 0) {
        const opt = document.createElement("option"); opt.value = ""; opt.textContent = "未检测到 FAT32 EFI 分区"; efiSel.appendChild(opt);
      } else {
        vfat.forEach((p, idx) => {
          const path = p.path || `/dev/${p.name}`;
          const opt = document.createElement("option");
          opt.value = path;
          opt.textContent = `${path} · ${formatBytes(p.size)} · FAT32`;
          if (idx === 0) opt.selected = true;
          efiSel.appendChild(opt);
        });
      }
    }
    syncAllCustomSelects();

    if (disks.length === 0) {
      listEl.innerHTML = `<div class="disk-empty">未检测到可用磁盘设备</div>`;
      state.selectedDisk = "";
      return;
    }

    listEl.innerHTML = "";
    let stillValid = false;
    disks.forEach((d) => {
      const devPath = `/dev/${d.name}`;
      const isBoot = env.boot_disk && d.name === env.boot_disk;
      const isRemovable = d.rm === true || d.rm === 1 || d.rm === "1" || d.tran === "usb";
      if (!isBoot && state.selectedDisk === devPath) stillValid = true;

      const item = document.createElement("div");
      item.className = "disk-item" + (isBoot ? " disabled" : "") + (state.selectedDisk === devPath && !isBoot ? " selected" : "");
      const left = document.createElement("div"); left.className = "disk-left";
      const radio = document.createElement("input"); radio.type = "radio"; radio.name = "target_disk";
      radio.disabled = Boolean(isBoot); radio.checked = state.selectedDisk === devPath && !isBoot;
      const info = document.createElement("div");
      const t = document.createElement("div"); t.className = "disk-title"; t.textContent = devPath;
      const s = document.createElement("div"); s.className = "disk-sub"; s.textContent = (d.model || "未知型号").trim();
      info.append(t, s); left.append(radio, info);

      const tags = document.createElement("div"); tags.className = "disk-tags";
      const addTag = (txt, warn = false) => { const sp = document.createElement("span"); sp.className = warn ? "tag warn" : "tag"; sp.textContent = txt; tags.appendChild(sp); };
      addTag(formatBytes(d.size));
      addTag((d.tran || "disk").toUpperCase());
      if (isBoot) addTag("当前启动盘（禁止）", true);
      else if (isRemovable) addTag("USB 可移动设备", true);

      item.append(left, tags);
      if (!isBoot) {
        item.addEventListener("click", () => {
          state.selectedDisk = devPath;
          listEl.querySelectorAll(".disk-item").forEach((el) => el.classList.remove("selected"));
          item.classList.add("selected");
          radio.checked = true;
        });
      }
      listEl.appendChild(item);
    });
    if (!stillValid) state.selectedDisk = "";
  };

  window.__spinelInstallerProgress = (pct, label) => {
    const c = Math.max(0, Math.min(100, Number(pct) || 0));
    const stepEl = document.getElementById("inst-step-text");
    const pctEl = document.getElementById("inst-pct-text");
    const barEl = document.getElementById("inst-progress-bar");
    if (stepEl && label) stepEl.textContent = label;
    if (pctEl) pctEl.textContent = `${c}%`;
    if (barEl) barEl.style.width = `${c}%`;
  };

  window.__spinelInstallerLog = (line) => {
    const box = document.getElementById("inst-log-box");
    if (!box) return;
    box.textContent += `${line}\n`;
    box.scrollTop = box.scrollHeight;
  };

  window.__spinelInstallerDone = (code) => {
    state.installRunning = false;
    const hint = document.getElementById("inst-status-hint");
    if (code === 0) {
      window.__spinelInstallerProgress(100, "安装完成！");
      if (hint) hint.textContent = "SpinelOS 已成功写入硬盘。请拔出安装介质后点击“立即重启电脑”。";
      document.getElementById("btn-inst-reboot")?.classList.remove("hidden");
    } else {
      const stepEl = document.getElementById("inst-step-text");
      if (stepEl) stepEl.textContent = `安装失败（退出码 ${code}）`;
      if (hint) hint.textContent = "请检查上方日志输出后重试。";
      document.getElementById("btn-inst-back")?.classList.remove("hidden");
    }
  };

  function setWelcomeStep(step) {
    state.welcomeStep = Math.max(1, Math.min(4, Number(step) || 1));
    [1, 2, 3, 4].forEach((i) => {
      document.getElementById(`welcome-step-${i}`)?.classList.toggle("hidden", i !== state.welcomeStep);
    });
    document.querySelectorAll("#welcome-step-dots [data-wdot]").forEach((dot) => {
      dot.classList.toggle("active", Number(dot.getAttribute("data-wdot")) === state.welcomeStep);
    });
    const prevBtn = document.getElementById("btn-wlc-prev");
    if (prevBtn) prevBtn.disabled = state.welcomeStep <= 1;
    const nextBtn = document.getElementById("btn-wlc-next");
    if (nextBtn) {
      nextBtn.textContent = state.welcomeStep >= 4 ? "开始使用 SpinelOS" : "下一步";
      nextBtn.setAttribute("data-cmd", state.welcomeStep >= 4 ? "welcome:complete" : "welcome:next");
    }
  }

  window.__spinelWelcomeStatus = (data) => {
    if (!data) return;
    const set = (id, v) => { const el = document.getElementById(id); if (el) el.textContent = v; };
    set("wlc-net-status", data.net || "已就绪");
    set("wlc-proxy-status", data.proxy_enabled ? "已开启系统代理并自动同步" : "未配置系统代理（如不需要可跳过）");
    set("wlc-gpu-status", data.gpu || "已检测图形适配");
    set("wlc-env-chip", data.is_live ? "Live 体验模式" : "本地系统盘模式");
    const steamBtn = document.getElementById("wlc-steam-btn");
    if (data.steam_installed) {
      set("wlc-steam-status", "Steam 客户端已安装，可随时启动或进入主机大屏");
      if (steamBtn) {
        steamBtn.textContent = "启动 Steam";
        steamBtn.setAttribute("data-cmd", "launch:steam");
      }
    } else {
      set("wlc-steam-status", "尚未安装专有 Steam 客户端（可点击右侧去应用商店一键安装）");
      if (steamBtn) {
        steamBtn.textContent = "一键安装 Steam";
        steamBtn.setAttribute("data-cmd", "store:open:steam");
      }
    }
  };

  /* ---------------- 7. Events, Context Menu, Gamepad & Boot ---------------- */
  if (mode === "desktop") {
    const menu = document.getElementById("desktop-ctx-menu");
    const hideMenu = () => menu?.classList.add("hidden");
    const showMenu = (x, y) => {
      if (!menu) return;
      menu.classList.remove("hidden");
      const rect = menu.getBoundingClientRect();
      const mw = rect.width || 210;
      const mh = rect.height || 260;
      menu.style.left = `${Math.max(8, Math.min(x, window.innerWidth - mw - 12))}px`;
      menu.style.top = `${Math.max(36, Math.min(y, window.innerHeight - mh - 86))}px`;
    };
    document.addEventListener("contextmenu", (ev) => {
      ev.preventDefault();
      showMenu(ev.clientX, ev.clientY);
    });
    document.addEventListener("click", (ev) => {
      if (!ev.target.closest("#desktop-ctx-menu")) hideMenu();
    });
    window.addEventListener("blur", hideMenu);
    document.addEventListener("keydown", (ev) => {
      if (ev.key === "Escape") hideMenu();
    });
  }

  document.addEventListener("keydown", (ev) => {
    if (!ev.metaKey || ev.ctrlKey || ev.altKey) return;
    const key = ev.key.toLowerCase();
    const active = document.activeElement;
    const isEditable = active && (active.tagName === "INPUT" || active.tagName === "TEXTAREA");
    if (key === "a" && isEditable) {
      ev.preventDefault();
      active.select();
    } else if (key === "c") {
      const sel = isEditable
        ? active.value.slice(active.selectionStart || 0, active.selectionEnd || 0)
        : (window.getSelection()?.toString() || "");
      if (sel) {
        ev.preventDefault();
        navigator.clipboard?.writeText(sel).catch(() => {});
      }
    } else if (key === "v" && isEditable) {
      ev.preventDefault();
      navigator.clipboard?.readText().then((txt) => {
        if (!txt) return;
        const start = active.selectionStart ?? active.value.length;
        const end = active.selectionEnd ?? active.value.length;
        active.setRangeText(txt, start, end, "end");
        active.dispatchEvent(new Event("input", { bubbles: true }));
      }).catch(() => {});
    }
  });

  document.addEventListener("click", (ev) => {
    const storeCatTarget = ev.target.closest("[data-store-cat]");
    if (storeCatTarget) {
      ev.stopPropagation();
      hooks.setStoreCategory?.(storeCatTarget.getAttribute("data-store-cat") || "all");
      document.querySelectorAll("[data-store-cat]").forEach((btn) => {
        btn.classList.toggle("active", btn === storeCatTarget);
      });
      hooks.renderStoreApps?.();
      return;
    }

    const perfTarget = ev.target.closest("[data-perf-key]");
    if (perfTarget) {
      ev.stopPropagation();
      const k = perfTarget.getAttribute("data-perf-key");
      const v = perfTarget.getAttribute("data-perf-val");
      hooks.sendPerfUpdate?.({ [k]: k === "fps_limit" ? Number(v) : v });
      return;
    }

    const target = ev.target.closest("[data-cmd]");
    if (!target) return;
    ev.stopPropagation();
    document.getElementById("desktop-ctx-menu")?.classList.add("hidden");
    const cmd = target.getAttribute("data-cmd");

    if (cmd === "installer:mode:disk") setInstallMode("disk");
    else if (cmd === "installer:mode:partition") setInstallMode("partition");
    else if (cmd === "installer:submit") submitInstaller();
    else if (cmd === "installer:back") {
      if (!state.installRunning) {
        document.getElementById("inst-progress-view").classList.add("hidden");
        document.getElementById("inst-form-view").classList.remove("hidden");
      }
    } else if (cmd.startsWith("settings:tab:") && mode === "settings") {
      hooks.switchSettingsTab?.(cmd.split(":")[2]);
    } else if (cmd === "settings:wifi-scan") {
      const btn = document.getElementById("btn-wifi-rescan");
      if (btn) { btn.disabled = true; btn.textContent = "扫描中..."; }
      sendCommand("settings:wifi-scan");
    } else if (cmd === "settings:wifi-cancel") {
      document.getElementById("wifi-connect-box")?.classList.add("hidden");
    } else if (cmd === "settings:wifi-do-connect") {
      const pwd = document.getElementById("wifi-password-input").value;
      showFeedback("wifi-feedback", `正在连接 ${state.selectedWifiSsid}...`);
      sendCommand(`settings:wifi-connect\nssid=${state.selectedWifiSsid}\npassword=${pwd}`);
    } else if (cmd === "settings:bt-toggle") {
      sendCommand(state.btPowered ? "settings:bt-power:off" : "settings:bt-power:on");
    } else if (cmd === "settings:bt-scan") {
      const btn = document.getElementById("btn-bt-scan");
      if (btn) { btn.disabled = true; btn.textContent = "扫描中 (6s)..."; }
      sendCommand("settings:bt-scan");
    } else if (cmd === "perf:toggle-fsr") {
      hooks.sendPerfUpdate?.({ fsr: !state.perfState.fsr });
    } else if (cmd === "perf:toggle-gamemode") {
      hooks.sendPerfUpdate?.({ gamemode: !state.perfState.gamemode });
    } else if (cmd === "perf:cycle-mangohud") {
      const order = ["off", "compact", "full"];
      const next = order[(order.indexOf(state.perfState.mangohud) + 1) % order.length];
      hooks.sendPerfUpdate?.({ mangohud: next });
    } else if (cmd === "store:custom-install") {
      const backend = document.getElementById("store-custom-backend")?.value || "pacman";
      const pkg = (document.getElementById("store-custom-pkg")?.value || "").trim();
      if (pkg) hooks.requestStoreOp?.("install", { id: "", name: pkg, backend, pkg });
    } else if (cmd === "store:pass-cancel") {
      state.pendingStoreOp = null;
      document.getElementById("store-pass-box")?.classList.add("hidden");
    } else if (cmd === "store:pass-confirm") {
      const inputEl = document.getElementById("store-pass-input");
      const pwd = inputEl?.value || "";
      if (!pwd) {
        inputEl?.focus();
        return;
      }
      document.getElementById("store-pass-box")?.classList.add("hidden");
      if (state.pendingStoreOp) {
        const cur = state.pendingStoreOp;
        state.pendingStoreOp = null;
        hooks.executeStoreOp?.(cur.action, cur.op, pwd);
      }
    } else if (cmd === "store:cancel") {
      const cancelBtn = document.getElementById("btn-store-cancel");
      if (cancelBtn) {
        cancelBtn.disabled = true;
        cancelBtn.textContent = "正在终止...";
      }
      sendCommand("store:cancel");
    } else if (cmd === "store:log-hide") {
      document.getElementById("store-log-wrap")?.classList.add("hidden");
    } else if (cmd === "ov:close") {
      document.getElementById("game-ov-modal")?.classList.add("hidden");
    } else if (cmd === "ov:toggle-fsr" && state.ovEditState) {
      state.ovEditState.fsr = !state.ovEditState.fsr;
      hooks.renderGameOverrideModal?.();
    } else if (cmd === "ov:toggle-gamemode" && state.ovEditState) {
      state.ovEditState.gamemode = !state.ovEditState.gamemode;
      hooks.renderGameOverrideModal?.();
    } else if (cmd === "ov:save" && state.ovEditState) {
      sendCommand([
        "steam:override-set",
        `appid=${state.ovEditState.appid}`,
        `mangohud=${state.ovEditState.mangohud}`,
        `fps_limit=${state.ovEditState.fps_limit}`,
        `fsr=${state.ovEditState.fsr ? "true" : "false"}`,
        `gamemode=${state.ovEditState.gamemode ? "true" : "false"}`,
        `render_res=${state.ovEditState.render_res}`
      ].join("\n"));
      document.getElementById("game-ov-modal")?.classList.add("hidden");
    } else if (cmd === "ov:clear" && state.ovEditState) {
      sendCommand(`steam:override-clear:${state.ovEditState.appid}`);
      document.getElementById("game-ov-modal")?.classList.add("hidden");
    } else if (cmd === "welcome:prev") {
      setWelcomeStep(state.welcomeStep - 1);
    } else if (cmd === "welcome:next") {
      setWelcomeStep(state.welcomeStep + 1);
    } else {
      sendCommand(cmd);
    }
  });

  function initGamepadNavigation() {
    if ((mode !== "desktop" && mode !== "games" && mode !== "store" && mode !== "settings" && mode !== "installer") ||
        typeof navigator.getGamepads !== "function") {
      return;
    }

    let gpFocusedEl = null;
    let lastDir = "";
    let lastDirTime = 0;
    let dirRepeatCount = 0;
    const prevButtons = new Map();

    const updatePadChip = () => {
      const pads = Array.from(navigator.getGamepads() || []).filter(Boolean);
      const chip = document.getElementById("desk-gamepad");
      if (chip) {
        chip.textContent = pads.length === 0
          ? "未连接"
          : (pads[0].id || "Controller").replace(/\s*\(.*?\)\s*/g, "").slice(0, 22);
      }
    };

    window.addEventListener("gamepadconnected", updatePadChip);
    window.addEventListener("gamepaddisconnected", updatePadChip);
    window.addEventListener("pointerdown", () => {
      if (gpFocusedEl) { gpFocusedEl.classList.remove("gp-focused"); gpFocusedEl = null; }
    });

    function getFocusables() {
      return Array.from(activeRootEl.querySelectorAll("button:not(:disabled), input:not(:disabled), select:not(:disabled):not(.sp-native-hidden)")).filter((el) => {
        if (el.closest(".hidden")) return false;
        const r = el.getBoundingClientRect();
        return r.width > 0 && r.height > 0;
      });
    }

    function setFocus(el) {
      if (gpFocusedEl && gpFocusedEl !== el) gpFocusedEl.classList.remove("gp-focused");
      gpFocusedEl = el || null;
      if (gpFocusedEl) {
        gpFocusedEl.classList.add("gp-focused");
        gpFocusedEl.scrollIntoView({ block: "nearest", inline: "nearest" });
      }
    }

    function moveFocus(dir) {
      const items = getFocusables();
      if (items.length === 0) return;
      if (!gpFocusedEl || !items.includes(gpFocusedEl)) { setFocus(items[0]); return; }
      const cur = gpFocusedEl.getBoundingClientRect();
      const cx = cur.left + cur.width / 2;
      const cy = cur.top + cur.height / 2;
      let best = null;
      let bestScore = Infinity;
      items.forEach((cand) => {
        if (cand === gpFocusedEl) return;
        const r = cand.getBoundingClientRect();
        const dx = r.left + r.width / 2 - cx;
        const dy = r.top + r.height / 2 - cy;
        let primary = 0, ortho = 0;
        if (dir === "right" && dx > 4) { primary = dx; ortho = Math.abs(dy); }
        else if (dir === "left" && dx < -4) { primary = -dx; ortho = Math.abs(dy); }
        else if (dir === "down" && dy > 4) { primary = dy; ortho = Math.abs(dx); }
        else if (dir === "up" && dy < -4) { primary = -dy; ortho = Math.abs(dx); }
        else return;
        const score = primary + ortho * 2.5;
        if (score < bestScore) { bestScore = score; best = cand; }
      });
      if (best) setFocus(best);
    }

    function tick(now) {
      const pad = Array.from(navigator.getGamepads() || []).find(Boolean);
      if (pad) {
        const pressed = (i) => Boolean(pad.buttons[i] && pad.buttons[i].pressed);
        const just = (i) => {
          const cur = pressed(i);
          const prev = prevButtons.get(i) || false;
          prevButtons.set(i, cur);
          return cur && !prev;
        };
        const ax = pad.axes[0] || 0;
        const ay = pad.axes[1] || 0;
        let dir = "";
        if (pressed(12) || ay < -0.55) dir = "up";
        else if (pressed(13) || ay > 0.55) dir = "down";
        else if (pressed(14) || ax < -0.55) dir = "left";
        else if (pressed(15) || ax > 0.55) dir = "right";
        if (dir) {
          const delay = dirRepeatCount === 0 ? 210 : 115;
          if (dir !== lastDir || now - lastDirTime >= delay) {
            moveFocus(dir);
            dirRepeatCount = dir === lastDir ? dirRepeatCount + 1 : 1;
            lastDir = dir;
            lastDirTime = now;
          }
        } else {
          lastDir = "";
          dirRepeatCount = 0;
        }
        if (just(0)) {
          const items = getFocusables();
          if (!gpFocusedEl || !items.includes(gpFocusedEl)) { if (items[0]) setFocus(items[0]); }
          else if (gpFocusedEl.tagName === "SELECT" && gpFocusedEl.options.length > 0) {
            gpFocusedEl.selectedIndex = (gpFocusedEl.selectedIndex + 1) % gpFocusedEl.options.length;
          } else {
            gpFocusedEl.click();
          }
        }
        if (just(9)) sendCommand("launch:steam-gamepadui");
      }
      window.requestAnimationFrame(tick);
    }
    updatePadChip();
    window.requestAnimationFrame(tick);
  }

  function updateClock() {
    const el = document.getElementById("bar-clock");
    if (!el) return;
    const now = new Date();
    const days = ["周日", "周一", "周二", "周三", "周四", "周五", "周六"];
    const m = now.getMonth() + 1;
    const d = now.getDate();
    const hh = String(now.getHours()).padStart(2, "0");
    const mm = String(now.getMinutes()).padStart(2, "0");
    el.textContent = `${m}月${d}日 ${days[now.getDay()]} ${hh}:${mm}`;
  }

  window.__spinelCcOpen = (isOpen) => {
    document.getElementById("bar-cc-btn")?.classList.toggle("cc-open", Boolean(isOpen));
  };

  window.__spinelControlState = (data) => {
    if (!data) return;
    if (typeof data.volume === "number") {
      const vs = document.getElementById("cc-vol-slider");
      const vp = document.getElementById("cc-vol-pct");
      if (vs) vs.value = String(data.volume);
      if (vp) vp.textContent = `${data.volume}%`;
    }
    const bs = document.getElementById("cc-bright-slider");
    const bp = document.getElementById("cc-bright-pct");
    if (bs && bp) {
      if (typeof data.brightness === "number" && data.brightness >= 0) {
        bs.disabled = false;
        bs.value = String(data.brightness);
        bp.textContent = `${data.brightness}%`;
      } else {
        bs.disabled = true;
        bp.textContent = "外接/VM";
      }
    }
  };

  if (mode === "cc") {
    const volSlider = document.getElementById("cc-vol-slider");
    const volPct = document.getElementById("cc-vol-pct");
    volSlider?.addEventListener("input", () => {
      if (volPct) volPct.textContent = `${volSlider.value}%`;
      sendCommand(`system:audio-set:${volSlider.value}`);
    });
    const brSlider = document.getElementById("cc-bright-slider");
    const brPct = document.getElementById("cc-bright-pct");
    brSlider?.addEventListener("input", () => {
      if (brPct) brPct.textContent = `${brSlider.value}%`;
      sendCommand(`system:brightness-set:${brSlider.value}`);
    });
    document.addEventListener("keydown", (ev) => {
      if (ev.key === "Escape") sendCommand("cc:close");
    });
  }

  window.__spinelUpdateTelemetry = (data) => {
    const set = (id, val) => { const el = document.getElementById(id); if (el && val) el.textContent = val; };
    const barNetEl = document.getElementById("bar-net");
    if (barNetEl && data.net) barNetEl.title = `网络: ${data.net}`;
    const barMemBtn = document.getElementById("bar-mem");
    if (barMemBtn && data.mem) barMemBtn.title = `内存占用: ${data.mem}`;
    const barMemText = document.getElementById("bar-mem-text");
    if (barMemText && data.mem) {
      const match = String(data.mem).match(/^(\d+)\s*\/\s*(\d+)/);
      barMemText.textContent = match ? `${match[1]}M` : data.mem;
    }
    set("cc-net-val", data.net);
    set("cc-mem-val", data.mem ? `内存 ${data.mem}` : "");
    set("cc-gpu-short", data.gpu);
    set("desk-gpu", data.gpu);
    set("desk-mem", data.mem);
    set("desk-net", data.net);
    set("desk-kernel", data.kernel);
    set("about-gpu", data.gpu);
    set("about-kernel", data.kernel);

    if (typeof data.low_fx === "boolean") {
      document.body.classList.toggle("low-fx", data.low_fx);
    }

    if (typeof data.is_live === "boolean") {
      const changed = state.isLiveEnv !== data.is_live;
      state.isLiveEnv = data.is_live;
      const modeChip = document.getElementById("desktop-mode-chip");
      if (modeChip) modeChip.textContent = state.isLiveEnv ? "Live ISO 模式" : "本地硬盘模式";
      const aboutMode = document.getElementById("about-mode");
      if (aboutMode) aboutMode.textContent = state.isLiveEnv ? "Live USB / ISO 体验模式" : "本地硬盘持久化系统";
      document.getElementById("btn-desk-install")?.classList.toggle("hidden", !state.isLiveEnv);
      if (changed) {
        renderDesktopShortcuts();
        renderDock();
      }
    }
  };

  syncAllCustomSelects();
  if (mode === "settings") hooks.switchSettingsTab?.(initialTab);
  if (mode === "doctor") sendCommand("settings:doctor-scan");
  if (mode === "store") sendCommand("store:refresh");
  if (mode === "installer") sendCommand("installer:scan-disks");
  if (mode === "games") sendCommand("steam:scan-caches");
  if (mode === "welcome") setWelcomeStep(1);
  if (mode === "desktop") renderDesktopShortcuts();
  if (mode === "dock") renderDock();

  hooks.renderPerfState?.();
  initGamepadNavigation();
  updateClock();
  setInterval(updateClock, 15000);
  sendCommand("telemetry:refresh");
})();
