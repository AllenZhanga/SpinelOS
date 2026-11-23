(() => {
  const params = new URLSearchParams(window.location.search);
  const mode = params.get("mode") || "hub";

  const topBar = document.getElementById("top-bar");
  const hub = document.getElementById("desktop-hub");

  if (mode === "bar") {
    topBar.classList.remove("hidden");
  } else {
    hub.classList.remove("hidden");
  }

  let selectedDisk = "";
  let installRunning = false;
  let selectedWifiSsid = "";
  let btPowered = false;
  let perfState = {
    mangohud: "compact",
    fps_limit: 0,
    fsr: false,
    gamemode: true,
    render_res: "native"
  };

  function sendCommand(cmd) {
    if (!cmd) return;
    if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.sysbridge) {
      window.webkit.messageHandlers.sysbridge.postMessage(cmd);
    } else {
      console.log("[sysbridge mock]", cmd);
    }
  }

  function formatBytes(bytes) {
    const num = Number(bytes || 0);
    if (!num) return "未知容量";
    const tb = num / 1e12;
    if (tb >= 1) return `${tb.toFixed(2)} TB`;
    return `${(num / 1e9).toFixed(1)} GB`;
  }

  function showFeedback(id, msg, isError = false) {
    const el = document.getElementById(id);
    if (!el) return;
    if (!msg) {
      el.classList.add("hidden");
      el.classList.remove("error");
      el.textContent = "";
      return;
    }
    el.classList.remove("hidden");
    el.classList.toggle("error", Boolean(isError));
    el.textContent = msg;
  }

  /* ---------------- Performance Control Strip ---------------- */
  function buildLaunchOptionString(cfg) {
    const parts = [];
    if (cfg.mangohud !== "off") parts.push("MANGOHUD=1");
    if (cfg.gamemode) parts.push("gamemoderun");

    const gs = ["gamescope", "-f"];
    if (cfg.render_res === "720p") gs.push("-w", "1280", "-h", "720");
    else if (cfg.render_res === "900p") gs.push("-w", "1600", "-h", "900");
    else if (cfg.render_res === "1080p") gs.push("-w", "1920", "-h", "1080");
    if (Number(cfg.fps_limit) > 0) gs.push("-r", String(cfg.fps_limit));
    if (cfg.fsr) gs.push("-F", "fsr");
    if (cfg.mangohud !== "off") gs.push("--mangoapp");
    gs.push("--", "%command%");

    return [...parts, ...gs].join(" ");
  }

  function renderPerfState() {
    document.querySelectorAll("[data-perf-key]").forEach((btn) => {
      const k = btn.getAttribute("data-perf-key");
      const v = btn.getAttribute("data-perf-val");
      btn.classList.toggle("active", String(perfState[k]) === String(v));
    });

    const fsrBtn = document.getElementById("btn-toggle-fsr");
    if (fsrBtn) {
      fsrBtn.textContent = `FSR 锐化: ${perfState.fsr ? "开" : "关"}`;
      fsrBtn.classList.toggle("active", Boolean(perfState.fsr));
    }
    const gmBtn = document.getElementById("btn-toggle-gamemode");
    if (gmBtn) {
      gmBtn.textContent = `GameMode: ${perfState.gamemode ? "开" : "关"}`;
      gmBtn.classList.toggle("active", Boolean(perfState.gamemode));
    }
    const preview = document.getElementById("perf-cmd-preview");
    if (preview) {
      preview.textContent = buildLaunchOptionString(perfState);
    }
  }

  function sendPerfUpdate(patch) {
    perfState = { ...perfState, ...patch };
    renderPerfState();
    const payload = [
      "perf:set",
      `mangohud=${perfState.mangohud}`,
      `fps_limit=${perfState.fps_limit}`,
      `fsr=${perfState.fsr ? "true" : "false"}`,
      `gamemode=${perfState.gamemode ? "true" : "false"}`,
      `render_res=${perfState.render_res}`
    ].join("\n");
    sendCommand(payload);
  }

  window.__spinelPerfConfig = (cfg) => {
    perfState = { ...perfState, ...cfg };
    renderPerfState();
  };

  /* ---------------- Steam Library Wall ---------------- */
  window.__spinelSteamGames = (games) => {
    const grid = document.getElementById("steam-game-grid");
    const countBadge = document.getElementById("steam-count-badge");
    if (!grid) return;

    const list = Array.isArray(games) ? games : [];
    if (countBadge) {
      countBadge.textContent = `${list.length} 款游戏`;
    }

    grid.innerHTML = "";
    if (list.length === 0) {
      const empty = document.createElement("div");
      empty.className = "lib-empty";
      const msg = document.createElement("p");
      msg.textContent = "暂未检测到本地已安装的 Steam 游戏。登录后下载任意游戏，点击“扫描游戏库”即可在这里一键启动。";
      const actions = document.createElement("div");
      actions.className = "footer-actions";
      const b1 = document.createElement("button");
      b1.className = "btn primary small";
      b1.setAttribute("data-cmd", "launch:steam");
      b1.textContent = "启动 Steam 下载游戏";
      const b2 = document.createElement("button");
      b2.className = "btn glow small";
      b2.setAttribute("data-cmd", "launch:steam-gamepadui");
      b2.textContent = "进入主机大屏模式";
      actions.append(b1, b2);
      empty.append(msg, actions);
      grid.appendChild(empty);
      return;
    }

    list.forEach((g) => {
      const card = document.createElement("article");
      card.className = "game-card";

      const poster = document.createElement("div");
      poster.className = "game-poster";

      const fallback = document.createElement("div");
      fallback.className = "game-poster-fallback hidden";
      fallback.textContent = g.name;

      const img = document.createElement("img");
      img.alt = g.name;
      const cdnCover = `https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/${g.appid}/library_600x900.jpg`;
      const cdnHeader = `https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/${g.appid}/header.jpg`;
      let step = 0;
      const sources = [];
      if (g.cover_uri) sources.push(g.cover_uri);
      sources.push(cdnCover, cdnHeader);
      img.src = sources[0];
      img.onerror = () => {
        step += 1;
        if (step < sources.length) {
          img.src = sources[step];
        } else {
          img.classList.add("hidden");
          fallback.classList.remove("hidden");
        }
      };

      poster.append(img, fallback);

      const meta = document.createElement("div");
      meta.className = "game-meta";
      const title = document.createElement("div");
      title.className = "game-title";
      title.textContent = g.name;
      const sub = document.createElement("div");
      sub.className = "game-sub";
      sub.textContent = `AppID ${g.appid} · ${formatBytes(g.size_bytes)}`;

      const launchBtn = document.createElement("button");
      launchBtn.className = "btn primary small game-launch-btn";
      launchBtn.textContent = "立即启动";
      launchBtn.setAttribute("data-cmd", `steam:launch:${g.appid}`);

      meta.append(title, sub, launchBtn);
      card.append(poster, meta);
      grid.appendChild(card);
    });
  };

  /* ---------------- Graphical Settings Center ---------------- */
  function switchSettingsTab(tab) {
    const tabs = ["wifi", "bt", "display"];
    const target = tabs.includes(tab) ? tab : "wifi";
    tabs.forEach((t) => {
      const body = document.getElementById(`tab-${t}`);
      if (body) body.classList.toggle("hidden", t !== target);
    });
    document.querySelectorAll(".tab-btn").forEach((btn) => {
      btn.classList.toggle("active", btn.getAttribute("data-cmd") === `settings:tab:${target}`);
    });

    if (target === "wifi") sendCommand("settings:wifi-list");
    else if (target === "bt") sendCommand("settings:bt-status");
    else if (target === "display") sendCommand("settings:display-scan");
  }

  window.__spinelOpenSettings = (tab) => {
    const modal = document.getElementById("settings-modal");
    if (!modal) return;
    modal.classList.remove("hidden");
    switchSettingsTab(tab || "wifi");
  };

  window.__spinelWifiState = (data) => {
    const rescanBtn = document.getElementById("btn-wifi-rescan");
    if (rescanBtn) {
      rescanBtn.disabled = false;
      rescanBtn.textContent = "扫描周边 Wi-Fi";
    }

    const ifaceEl = document.getElementById("wifi-iface-list");
    if (ifaceEl) {
      ifaceEl.innerHTML = "";
      const ifaces = Array.isArray(data.interfaces) ? data.interfaces : [];
      if (ifaces.length === 0) {
        const d = document.createElement("div");
        d.className = "disk-empty";
        d.textContent = "未检测到网络接口";
        ifaceEl.appendChild(d);
      } else {
        ifaces.forEach((ifx) => {
          const item = document.createElement("div");
          item.className = "disk-item";
          const left = document.createElement("div");
          const t = document.createElement("div");
          t.className = "disk-title";
          t.textContent = `${ifx.device} (${ifx.type})`;
          const sub = document.createElement("div");
          sub.className = "disk-sub";
          sub.textContent = ifx.connection ? `当前连接: ${ifx.connection}` : `状态: ${ifx.state}`;
          left.append(t, sub);

          const right = document.createElement("div");
          right.className = "disk-tags";
          const tag = document.createElement("span");
          tag.className = "tag";
          tag.textContent = ifx.state;
          right.appendChild(tag);

          if (ifx.state === "connected") {
            const disc = document.createElement("button");
            disc.className = "btn small danger";
            disc.textContent = "断开";
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
      const d = document.createElement("div");
      d.className = "disk-empty";
      d.textContent = "暂无扫描到的 Wi-Fi 热点，请点击“扫描周边 Wi-Fi”。";
      listEl.appendChild(d);
      return;
    }

    nets.sort((a, b) => Number(b.active) - Number(a.active) || b.signal - a.signal);
    nets.forEach((n) => {
      const item = document.createElement("div");
      item.className = "disk-item" + (n.active ? " selected" : "");

      const left = document.createElement("div");
      const title = document.createElement("div");
      title.className = "disk-title";
      title.textContent = n.ssid;
      const sub = document.createElement("div");
      sub.className = "disk-sub";
      sub.textContent = `信号 ${n.signal}% · ${n.security || "开放网络"}`;
      left.append(title, sub);

      const tags = document.createElement("div");
      tags.className = "disk-tags";
      if (n.active) {
        const tag = document.createElement("span");
        tag.className = "tag";
        tag.textContent = "已连接";
        tags.appendChild(tag);
      } else {
        const btn = document.createElement("button");
        btn.className = "btn small";
        btn.textContent = "连接";
        btn.addEventListener("click", (ev) => {
          ev.stopPropagation();
          selectedWifiSsid = n.ssid;
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
    if (ok) {
      document.getElementById("wifi-connect-box").classList.add("hidden");
    }
  };

  window.__spinelBtState = (data) => {
    btPowered = Boolean(data.powered);
    const badge = document.getElementById("bt-power-badge");
    const powerBtn = document.getElementById("btn-bt-power");
    const scanBtn = document.getElementById("btn-bt-scan");
    if (badge) badge.textContent = btPowered ? "已开启" : "已关闭";
    if (powerBtn) powerBtn.textContent = btPowered ? "关闭蓝牙" : "开启蓝牙";
    if (scanBtn) {
      scanBtn.disabled = false;
      scanBtn.textContent = "扫描手柄 (6s)";
    }

    const listEl = document.getElementById("bt-device-list");
    if (!listEl) return;
    listEl.innerHTML = "";
    const devs = Array.isArray(data.devices) ? data.devices : [];
    if (devs.length === 0) {
      const d = document.createElement("div");
      d.className = "disk-empty";
      d.textContent = btPowered
        ? "暂无发现的蓝牙设备。请将手柄设为配对模式后点击“扫描手柄”。"
        : "蓝牙已关闭，请先开启蓝牙适配器。";
      listEl.appendChild(d);
      return;
    }

    devs.sort((a, b) => Number(b.connected) - Number(a.connected) || Number(b.paired) - Number(a.paired));
    devs.forEach((dev) => {
      const item = document.createElement("div");
      item.className = "disk-item" + (dev.connected ? " selected" : "");

      const left = document.createElement("div");
      const title = document.createElement("div");
      title.className = "disk-title";
      title.textContent = dev.name;
      const sub = document.createElement("div");
      sub.className = "disk-sub";
      sub.textContent = `${dev.mac}${dev.icon ? ` · ${dev.icon}` : ""}`;
      left.append(title, sub);

      const tags = document.createElement("div");
      tags.className = "disk-tags";
      if (dev.connected) {
        const tag = document.createElement("span");
        tag.className = "tag";
        tag.textContent = "已连接";
        tags.appendChild(tag);
        const disc = document.createElement("button");
        disc.className = "btn small";
        disc.textContent = "断开";
        disc.setAttribute("data-cmd", `settings:bt-disconnect:${dev.mac}`);
        tags.appendChild(disc);
      } else {
        if (dev.paired) {
          const tag = document.createElement("span");
          tag.className = "tag";
          tag.textContent = "已配对";
          tags.appendChild(tag);
        }
        const conn = document.createElement("button");
        conn.className = "btn primary small";
        conn.textContent = dev.paired ? "连接" : "配对并连接";
        conn.setAttribute("data-cmd", `settings:bt-connect:${dev.mac}`);
        tags.appendChild(conn);
      }
      if (dev.paired) {
        const rem = document.createElement("button");
        rem.className = "btn small danger";
        rem.textContent = "移除";
        rem.setAttribute("data-cmd", `settings:bt-remove:${dev.mac}`);
        tags.appendChild(rem);
      }

      item.append(left, tags);
      listEl.appendChild(item);
    });
  };

  window.__spinelBtAction = (ok, msg) => {
    showFeedback("bt-feedback", msg || (ok ? "蓝牙操作完成" : "蓝牙操作失败"), !ok);
  };

  window.__spinelDisplayState = (outputs) => {
    const listEl = document.getElementById("display-output-list");
    if (!listEl) return;
    listEl.innerHTML = "";
    const outs = Array.isArray(outputs) ? outputs : [];
    if (outs.length === 0) {
      const d = document.createElement("div");
      d.className = "disk-empty";
      d.textContent = "未读取到 Wayland 显示器输出（需在 labwc Wayland 会话中运行 wlr-randr）。";
      listEl.appendChild(d);
      return;
    }

    outs.forEach((out) => {
      const item = document.createElement("div");
      item.className = "disk-item";

      const left = document.createElement("div");
      const title = document.createElement("div");
      title.className = "disk-title";
      title.textContent = `${out.name}${out.model ? ` (${out.model})` : ""}`;
      const curMode = Array.isArray(out.modes) ? out.modes.find((m) => m.current) : null;
      const sub = document.createElement("div");
      sub.className = "disk-sub";
      sub.textContent = curMode
        ? `当前: ${curMode.width}x${curMode.height} @ ${Number(curMode.refresh).toFixed(2)} Hz · 缩放 ${out.scale || 1}`
        : `缩放: ${out.scale || 1}`;
      left.append(title, sub);

      const controls = document.createElement("div");
      controls.className = "display-controls";

      const modeSel = document.createElement("select");
      const modes = Array.isArray(out.modes) ? out.modes : [];
      const seenMode = new Set();
      modes.forEach((m) => {
        const hz = Number(m.refresh).toFixed(3);
        const val = `${m.width}x${m.height}@${hz}Hz`;
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
        const payload = [
          "settings:display-apply",
          `output=${out.name}`,
          `mode=${modeSel.value}`,
          `scale=${scaleSel.value}`
        ].join("\n");
        sendCommand(payload);
      });

      controls.append(modeSel, scaleSel, applyBtn);
      item.append(left, controls);
      listEl.appendChild(item);
    });
  };

  window.__spinelDisplayAction = (ok, msg) => {
    showFeedback("display-feedback", msg || (ok ? "已应用显示设置" : "应用失败"), !ok);
  };

  /* ---------------- Graphical Disk Installer ---------------- */
  function showFormError(msg) {
    const el = document.getElementById("inst-form-error");
    if (!el) return;
    if (!msg) {
      el.classList.add("hidden");
      el.textContent = "";
    } else {
      el.classList.remove("hidden");
      el.textContent = msg;
    }
  }

  function openInstallerModal() {
    const modal = document.getElementById("installer-modal");
    if (!modal) return;
    modal.classList.remove("hidden");
    showFormError("");
    if (!installRunning) {
      document.getElementById("inst-form-view").classList.remove("hidden");
      document.getElementById("inst-progress-view").classList.add("hidden");
      sendCommand("installer:scan-disks");
    }
  }

  function closeInstallerModal() {
    const modal = document.getElementById("installer-modal");
    if (!modal) return;
    modal.classList.add("hidden");
  }

  function submitInstaller() {
    showFormError("");
    if (!selectedDisk) {
      showFormError("请先选择一个目标硬盘。");
      return;
    }

    const username = document.getElementById("inst-username").value.trim();
    const hostname = document.getElementById("inst-hostname").value.trim();
    const password = document.getElementById("inst-password").value;
    const password2 = document.getElementById("inst-password2").value;
    const timezone = document.getElementById("inst-timezone").value;
    const autologin = document.getElementById("inst-autologin").checked;
    const confirmWipe = document.getElementById("inst-confirm-wipe").checked;

    if (!/^[a-z_][a-z0-9_-]{0,30}$/.test(username)) {
      showFormError("用户名必须以小写字母或下划线开头，且仅包含小写字母、数字、_ 或 -。");
      return;
    }
    if (!/^[a-zA-Z0-9][a-zA-Z0-9-]{0,62}$/.test(hostname)) {
      showFormError("主机名仅支持字母、数字和连字符。");
      return;
    }
    if (password.length < 4 || /[\r\n]/.test(password)) {
      showFormError("密码长度至少为 4 位，且不能包含换行符。");
      return;
    }
    if (password !== password2) {
      showFormError("两次输入的密码不一致。");
      return;
    }
    if (!confirmWipe) {
      showFormError("请勾选确认框以允许清空目标硬盘数据。");
      return;
    }

    installRunning = true;
    document.getElementById("inst-form-view").classList.add("hidden");
    document.getElementById("inst-progress-view").classList.remove("hidden");
    document.getElementById("btn-inst-back").classList.add("hidden");
    document.getElementById("btn-inst-reboot").classList.add("hidden");
    document.getElementById("inst-step-text").textContent = "正在启动安装程序...";
    document.getElementById("inst-pct-text").textContent = "0%";
    document.getElementById("inst-progress-bar").style.width = "0%";
    document.getElementById("inst-log-box").textContent = "";
    document.getElementById("inst-status-hint").textContent = "正在执行安装，请勿断电或拔出安装介质...";

    const payload = [
      "installer:start",
      `disk=${selectedDisk}`,
      `username=${username}`,
      `hostname=${hostname}`,
      `timezone=${timezone}`,
      `autologin=${autologin ? "true" : "false"}`,
      `password=${password}`
    ].join("\n");

    sendCommand(payload);
  }

  window.__spinelInstallerEnv = (env) => {
    installRunning = Boolean(env.installing);
    const badge = document.getElementById("inst-env-badge");
    if (badge) {
      const modeText = env.is_live ? "Live ISO" : "已安装环境";
      const bootText = env.uefi ? "UEFI GPT" : "Legacy BIOS";
      badge.textContent = `${modeText} · ${bootText}`;
    }

    const listEl = document.getElementById("inst-disk-list");
    if (!listEl) return;

    const rawDevices = env.lsblk && Array.isArray(env.lsblk.blockdevices) ? env.lsblk.blockdevices : [];
    const disks = rawDevices.filter(
      (d) => d.type === "disk" && !/^loop|^zram/.test(d.name || "")
    );

    if (disks.length === 0) {
      listEl.innerHTML = `<div class="disk-empty">未检测到可用磁盘设备，请点击“刷新磁盘”重试。</div>`;
      selectedDisk = "";
      return;
    }

    listEl.innerHTML = "";
    let stillValid = false;

    disks.forEach((d) => {
      const devPath = `/dev/${d.name}`;
      const isBootDisk = env.boot_disk && d.name === env.boot_disk;
      const isRemovable = d.rm === true || d.rm === 1 || d.rm === "1" || d.tran === "usb";

      if (!isBootDisk && selectedDisk === devPath) {
        stillValid = true;
      }

      const item = document.createElement("div");
      item.className = "disk-item" + (isBootDisk ? " disabled" : "");
      if (selectedDisk === devPath && !isBootDisk) {
        item.classList.add("selected");
      }

      const model = (d.model || "未知型号").trim();
      const tran = (d.tran || "disk").toUpperCase();
      const sizeStr = formatBytes(d.size);

      const left = document.createElement("div");
      left.className = "disk-left";

      const radio = document.createElement("input");
      radio.type = "radio";
      radio.name = "target_disk";
      radio.disabled = Boolean(isBootDisk);
      radio.checked = selectedDisk === devPath && !isBootDisk;

      const info = document.createElement("div");
      const titleEl = document.createElement("div");
      titleEl.className = "disk-title";
      titleEl.textContent = devPath;
      const subEl = document.createElement("div");
      subEl.className = "disk-sub";
      subEl.textContent = model;
      info.append(titleEl, subEl);
      left.append(radio, info);

      const tags = document.createElement("div");
      tags.className = "disk-tags";
      const addTag = (text, warn = false) => {
        const span = document.createElement("span");
        span.className = warn ? "tag warn" : "tag";
        span.textContent = text;
        tags.appendChild(span);
      };
      addTag(sizeStr);
      addTag(tran);
      if (isBootDisk) addTag("当前启动盘（禁止选择）", true);
      else if (isRemovable) addTag("USB 可移动设备", true);

      item.append(left, tags);

      if (!isBootDisk) {
        item.addEventListener("click", () => {
          selectedDisk = devPath;
          listEl.querySelectorAll(".disk-item").forEach((el) => el.classList.remove("selected"));
          item.classList.add("selected");
          radio.checked = true;
        });
      }

      listEl.appendChild(item);
    });

    if (!stillValid) {
      selectedDisk = "";
    }
  };

  window.__spinelInstallerProgress = (pct, label) => {
    const clamped = Math.max(0, Math.min(100, Number(pct) || 0));
    const stepEl = document.getElementById("inst-step-text");
    const pctEl = document.getElementById("inst-pct-text");
    const barEl = document.getElementById("inst-progress-bar");
    if (stepEl && label) stepEl.textContent = label;
    if (pctEl) pctEl.textContent = `${clamped}%`;
    if (barEl) barEl.style.width = `${clamped}%`;
  };

  window.__spinelInstallerLog = (line) => {
    const box = document.getElementById("inst-log-box");
    if (!box) return;
    box.textContent += `${line}\n`;
    box.scrollTop = box.scrollHeight;
  };

  window.__spinelInstallerDone = (code) => {
    installRunning = false;
    const hint = document.getElementById("inst-status-hint");
    const backBtn = document.getElementById("btn-inst-back");
    const rebootBtn = document.getElementById("btn-inst-reboot");
    const stepEl = document.getElementById("inst-step-text");

    if (code === 0) {
      window.__spinelInstallerProgress(100, "安装完成！");
      if (hint) hint.textContent = "SpinelOS 已成功写入硬盘。请拔出 U 盘后点击“立即重启电脑”。";
      if (rebootBtn) rebootBtn.classList.remove("hidden");
    } else {
      if (stepEl) stepEl.textContent = `安装失败（退出码 ${code}）`;
      if (hint) hint.textContent = "请检查上方日志输出，修改配置后重试。";
      if (backBtn) backBtn.classList.remove("hidden");
    }
  };

  /* ---------------- Global Click Router & Telemetry ---------------- */
  document.addEventListener("click", (ev) => {
    const perfTarget = ev.target.closest("[data-perf-key]");
    if (perfTarget) {
      ev.stopPropagation();
      const k = perfTarget.getAttribute("data-perf-key");
      const v = perfTarget.getAttribute("data-perf-val");
      sendPerfUpdate({ [k]: k === "fps_limit" ? Number(v) : v });
      return;
    }

    const target = ev.target.closest("[data-cmd]");
    if (!target) return;
    ev.stopPropagation();
    const cmd = target.getAttribute("data-cmd");

    if (cmd === "installer:open") {
      openInstallerModal();
    } else if (cmd === "installer:close") {
      closeInstallerModal();
    } else if (cmd === "installer:submit") {
      submitInstaller();
    } else if (cmd === "installer:back") {
      if (!installRunning) {
        document.getElementById("inst-progress-view").classList.add("hidden");
        document.getElementById("inst-form-view").classList.remove("hidden");
      }
    } else if (cmd.startsWith("settings:open:") && mode === "hub") {
      window.__spinelOpenSettings(cmd.split(":")[2]);
    } else if (cmd.startsWith("settings:tab:")) {
      switchSettingsTab(cmd.split(":")[2]);
    } else if (cmd === "settings:close") {
      document.getElementById("settings-modal").classList.add("hidden");
    } else if (cmd === "settings:wifi-scan") {
      const btn = document.getElementById("btn-wifi-rescan");
      if (btn) { btn.disabled = true; btn.textContent = "扫描中..."; }
      sendCommand("settings:wifi-scan");
    } else if (cmd === "settings:wifi-cancel") {
      document.getElementById("wifi-connect-box").classList.add("hidden");
    } else if (cmd === "settings:wifi-do-connect") {
      const pwd = document.getElementById("wifi-password-input").value;
      showFeedback("wifi-feedback", `正在连接 ${selectedWifiSsid}...`);
      sendCommand(`settings:wifi-connect\nssid=${selectedWifiSsid}\npassword=${pwd}`);
    } else if (cmd === "settings:bt-toggle") {
      sendCommand(btPowered ? "settings:bt-power:off" : "settings:bt-power:on");
    } else if (cmd === "settings:bt-scan") {
      const btn = document.getElementById("btn-bt-scan");
      if (btn) { btn.disabled = true; btn.textContent = "扫描中 (6s)..."; }
      sendCommand("settings:bt-scan");
    } else if (cmd === "perf:toggle-fsr") {
      sendPerfUpdate({ fsr: !perfState.fsr });
    } else if (cmd === "perf:toggle-gamemode") {
      sendPerfUpdate({ gamemode: !perfState.gamemode });
    } else if (cmd === "perf:copy") {
      const str = buildLaunchOptionString(perfState);
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(str).then(() => {
          const btn = document.getElementById("btn-copy-launch");
          if (btn) {
            btn.textContent = "已复制!";
            setTimeout(() => { btn.textContent = "复制 Steam 启动参数"; }, 1500);
          }
        }).catch(() => {});
      }
    } else {
      sendCommand(cmd);
    }
  });

  function updateClock() {
    const el = document.getElementById("bar-clock");
    if (!el) return;
    const now = new Date();
    const hh = String(now.getHours()).padStart(2, "0");
    const mm = String(now.getMinutes()).padStart(2, "0");
    el.textContent = `${hh}:${mm}`;
  }

  window.__spinelUpdateTelemetry = (data) => {
    const set = (id, val) => {
      const el = document.getElementById(id);
      if (el && val) el.textContent = val;
    };
    set("bar-net", `网络: ${data.net}`);
    set("bar-mem", `内存: ${data.mem}`);
    set("hub-gpu", data.gpu);
    set("hub-mem", data.mem);
    set("hub-net", data.net);
    set("hub-kernel", data.kernel);

    const desc = document.getElementById("install-card-desc");
    if (desc && typeof data.is_live === "boolean") {
      desc.textContent = data.is_live
        ? "当前为 Live 体验模式。测试硬件与 Steam 正常后，可一键安装至本地 NVMe/SSD。"
        : "当前系统已运行在本地硬盘模式，可随时管理电源或重新部署。";
    }
  };

  renderPerfState();
  updateClock();
  setInterval(updateClock, 15000);
  sendCommand("telemetry:refresh");
})();
