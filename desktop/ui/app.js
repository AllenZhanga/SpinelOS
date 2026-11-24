(() => {
  const params = new URLSearchParams(window.location.search);
  const mode = params.get("mode") || "desktop";
  const initialTab = params.get("tab") || "wifi";

  const modeRoots = {
    bar: "top-bar",
    desktop: "desktop-canvas",
    dock: "dock-shell",
    cc: "cc-shell",
    games: "app-games",
    settings: "app-settings",
    installer: "app-installer"
  };

  Object.values(modeRoots).forEach((id) => {
    const el = document.getElementById(id);
    if (el) el.classList.add("hidden");
  });
  const activeRootEl = document.getElementById(modeRoots[mode] || "desktop-canvas");
  if (activeRootEl) activeRootEl.classList.remove("hidden");

  let isLiveEnv = false;
  let steamGames = [];
  let desktopApps = [];
  let trackedWindows = [];
  let selectedDisk = "";
  let installMode = "disk";
  let isUefiMode = true;
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

  /* ---------------- Inline macOS-style Squircle Icons ---------------- */
  let svgIconSeq = 0;

  function makeSvgIcon(kind) {
    const uid = `sp${++svgIconSeq}`;
    const bgDefs = {
      games: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#38bdf8"/>
          <stop offset="52%" stop-color="#6366f1"/>
          <stop offset="100%" stop-color="#4338ca"/>
        </linearGradient>`,
      steam: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#17305c"/>
          <stop offset="55%" stop-color="#145388"/>
          <stop offset="100%" stop-color="#1b8dd8"/>
        </linearGradient>`,
      gamepad: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#f43f5e"/>
          <stop offset="52%" stop-color="#a855f7"/>
          <stop offset="100%" stop-color="#6366f1"/>
        </linearGradient>`,
      browser: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#ffffff"/>
          <stop offset="100%" stop-color="#e2e8f0"/>
        </linearGradient>
        <radialGradient id="${uid}_dial" cx="50%" cy="45%" r="55%">
          <stop offset="0%" stop-color="#38bdf8"/>
          <stop offset="65%" stop-color="#0284c7"/>
          <stop offset="100%" stop-color="#1d4ed8"/>
        </radialGradient>`,
      files: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#7dd3fc"/>
          <stop offset="100%" stop-color="#38bdf8"/>
        </linearGradient>
        <linearGradient id="${uid}_face" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#2563eb"/>
          <stop offset="100%" stop-color="#1d4ed8"/>
        </linearGradient>`,
      terminal: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#e2e8f0"/>
          <stop offset="100%" stop-color="#94a3b8"/>
        </linearGradient>
        <linearGradient id="${uid}_scr" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#1e2430"/>
          <stop offset="100%" stop-color="#0b0f17"/>
        </linearGradient>`,
      settings: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#e5e7eb"/>
          <stop offset="100%" stop-color="#9ca3af"/>
        </linearGradient>
        <linearGradient id="${uid}_gear" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#64748b"/>
          <stop offset="100%" stop-color="#334155"/>
        </linearGradient>`,
      installer: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#34d399"/>
          <stop offset="100%" stop-color="#059669"/>
        </linearGradient>`,
      app: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#60a5fa"/>
          <stop offset="100%" stop-color="#7c3aed"/>
        </linearGradient>`
    };

    const art = {
      games: `
        <path d="M21 23c-4.5 0-8 3.2-9 7.8l-1.4 8.2a4.6 4.6 0 0 0 8.1 3.7l3.2-3.4h20.2l3.2 3.4a4.6 4.6 0 0 0 8.1-3.7L52 30.8C51 26.2 47.5 23 43 23H21z" fill="rgba(255,255,255,0.95)"/>
        <path d="M21 29v7m-3.5-3.5h7" stroke="#4f46e5" stroke-width="2.6" stroke-linecap="round"/>
        <circle cx="42.5" cy="30" r="2" fill="#38bdf8"/>
        <circle cx="46.5" cy="34" r="2" fill="#6366f1"/>
        <circle cx="38.5" cy="34" r="2" fill="#818cf8"/>
        <circle cx="42.5" cy="38" r="2" fill="#ec4899"/>`,
      steam: `
        <circle cx="39.5" cy="24.5" r="9.5" fill="none" stroke="rgba(255,255,255,0.96)" stroke-width="3.6"/>
        <circle cx="39.5" cy="24.5" r="4.2" fill="#ffffff"/>
        <circle cx="23.5" cy="43.5" r="7" fill="none" stroke="rgba(255,255,255,0.96)" stroke-width="3.4"/>
        <circle cx="23.5" cy="43.5" r="3.4" fill="#ffffff"/>
        <path d="M12 37.5l10.8 4.4M29 41l10-12.5" stroke="#ffffff" stroke-width="5.2" stroke-linecap="round"/>`,
      gamepad: `
        <rect x="13" y="16" width="38" height="32" rx="10" fill="rgba(255,255,255,0.18)" stroke="rgba(255,255,255,0.9)" stroke-width="2.4"/>
        <path d="M24 27v10m-5-5h10" stroke="#ffffff" stroke-width="3" stroke-linecap="round"/>
        <circle cx="41.5" cy="29.5" r="2.2" fill="#ffffff"/>
        <circle cx="45" cy="34.5" r="2.2" fill="#fde047"/>`,
      browser: `
        <circle cx="32" cy="32" r="23" fill="url(#${uid}_dial)"/>
        <circle cx="32" cy="32" r="21" fill="none" stroke="rgba(255,255,255,0.35)" stroke-width="1"/>
        <polygon points="32,32 45,19 36,36" fill="#ef4444"/>
        <polygon points="32,32 19,45 28,28" fill="#ffffff"/>
        <circle cx="32" cy="32" r="2.6" fill="#e2e8f0"/>`,
      files: `
        <path d="M4 18a14 14 0 0 1 14-14h15.5c-2.5 8-3.5 18-3.5 28h7.5c0 9-3 18-6 28H18A14 14 0 0 1 4 46V18z" fill="url(#${uid}_face)"/>
        <path d="M33.5 4c-2.5 8-3.5 18-3.5 28h7.5c0 9-3 18-6 28" fill="none" stroke="rgba(15,23,42,0.22)" stroke-width="1.8"/>
        <path d="M21 21v7M43 21v7" stroke="#0f172a" stroke-width="3.4" stroke-linecap="round"/>
        <path d="M18.5 41.5c4.5 5.2 22.5 5.2 27 0" fill="none" stroke="#0f172a" stroke-width="3.2" stroke-linecap="round"/>`,
      terminal: `
        <rect x="6" y="7" width="52" height="50" rx="11" fill="url(#${uid}_scr)"/>
        <path d="M16 23l9 7-9 7" fill="none" stroke="#ffffff" stroke-width="3.6" stroke-linecap="round" stroke-linejoin="round"/>
        <path d="M29 37h13" stroke="#38bdf8" stroke-width="3.6" stroke-linecap="round"/>`,
      settings: `
        <circle cx="32" cy="32" r="19" fill="url(#${uid}_gear)" stroke="#475569" stroke-width="1.5"/>
        <circle cx="32" cy="32" r="14.5" fill="none" stroke="rgba(255,255,255,0.42)" stroke-width="2.5" stroke-dasharray="4.2 3.2"/>
        <circle cx="32" cy="32" r="7.5" fill="#cbd5e1" stroke="#334155" stroke-width="2.2"/>
        <circle cx="32" cy="32" r="3" fill="#334155"/>`,
      installer: `
        <rect x="14" y="15" width="36" height="34" rx="8" fill="rgba(255,255,255,0.2)" stroke="rgba(255,255,255,0.9)" stroke-width="2.4"/>
        <path d="M32 22v16m0 0-6.5-6.5M32 38l6.5-6.5M22 43h20" fill="none" stroke="#ffffff" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>`,
      app: `
        <rect x="16" y="16" width="32" height="32" rx="9" fill="rgba(255,255,255,0.22)" stroke="rgba(255,255,255,0.92)" stroke-width="2.6"/>
        <circle cx="32" cy="32" r="6" fill="#ffffff"/>`
    };

    const defs = bgDefs[kind] || bgDefs.app;
    const body = art[kind] || art.app;
    return `
      <svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
        <defs>
          ${defs}
          <linearGradient id="${uid}_sheen" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0%" stop-color="#ffffff" stop-opacity="0.28"/>
            <stop offset="45%" stop-color="#ffffff" stop-opacity="0.04"/>
            <stop offset="100%" stop-color="#000000" stop-opacity="0.14"/>
          </linearGradient>
        </defs>
        <rect x="2" y="2" width="60" height="60" rx="14.5" fill="url(#${uid}_bg)"/>
        ${body}
        <rect x="2" y="2" width="60" height="60" rx="14.5" fill="url(#${uid}_sheen)"/>
        <rect x="2.75" y="2.75" width="58.5" height="58.5" rx="13.8" fill="none" stroke="rgba(255,255,255,0.28)" stroke-width="1.2"/>
      </svg>
    `;
  }

  function matchAppKind(appIdOrTitle) {
    const s = (appIdOrTitle || "").toLowerCase();
    if (s.includes("steam")) return "steam";
    if (s.includes("gamescope")) return "gamepad";
    if (s.includes("chromium") || s.includes("firefox") || s.includes("browser")) return "browser";
    if (s.includes("thunar") || s.includes("files")) return "files";
    if (s.includes("foot") || s.includes("terminal")) return "terminal";
    if (s.includes("游戏与启动中心") || s.includes("app-games")) return "games";
    if (s.includes("系统设置") || s.includes("settings")) return "settings";
    if (s.includes("安装") || s.includes("installer")) return "installer";
    return "app";
  }

  /* ---------------- 1. Desktop Shortcuts & Wallpaper ---------------- */
  function renderDesktopShortcuts() {
    const grid = document.getElementById("desktop-shortcuts");
    if (!grid) return;
    grid.innerHTML = "";

    const baseShortcuts = [
      { label: "游戏中心", cmd: "games:open", kind: "games" },
      { label: "Steam", cmd: "launch:steam", kind: "steam" },
      { label: "主机大屏", cmd: "launch:steam-gamepadui", kind: "gamepad" },
      { label: "Chromium", cmd: "launch:browser", kind: "browser" },
      { label: "文件管理", cmd: "launch:files", kind: "files" },
      { label: "终端", cmd: "launch:terminal", kind: "terminal" },
      { label: "系统设置", cmd: "settings:open:display", kind: "settings" }
    ];
    if (isLiveEnv) {
      baseShortcuts.push({ label: "安装 SpinelOS", cmd: "installer:open", kind: "installer" });
    }

    baseShortcuts.forEach((sc) => {
      const btn = document.createElement("button");
      btn.className = "shortcut-item";
      btn.setAttribute("data-cmd", sc.cmd);
      const icon = document.createElement("div");
      icon.className = "shortcut-icon";
      icon.innerHTML = makeSvgIcon(sc.kind);
      const label = document.createElement("span");
      label.className = "shortcut-label";
      label.textContent = sc.label;
      btn.append(icon, label);
      grid.appendChild(btn);
    });

    steamGames.slice(0, 6).forEach((g) => {
      const btn = document.createElement("button");
      btn.className = "shortcut-item";
      btn.setAttribute("data-cmd", `steam:launch:${g.appid}`);
      const icon = document.createElement("div");
      icon.className = "shortcut-icon";
      if (g.cover_uri) {
        const img = document.createElement("img");
        img.src = g.cover_uri;
        img.alt = g.name;
        img.onerror = () => { icon.innerHTML = makeSvgIcon("steam"); };
        icon.appendChild(img);
      } else {
        icon.innerHTML = makeSvgIcon("steam");
      }
      const label = document.createElement("span");
      label.className = "shortcut-label";
      label.textContent = g.name;
      btn.append(icon, label);
      grid.appendChild(btn);
    });
  }

  window.__spinelWallpaper = (wp) => {
    const canvas = document.getElementById("desktop-canvas");
    if (canvas) {
      canvas.classList.remove("wp-crystal-blue", "wp-obsidian-purple", "wp-emerald-aurora", "wp-amber-dusk");
      canvas.classList.add(`wp-${wp}`);
    }
    document.querySelectorAll("#wallpaper-grid .wp-card").forEach((btn) => {
      btn.classList.toggle("active", btn.getAttribute("data-cmd") === `wallpaper:set:${wp}`);
    });
  };

  /* ---------------- 2. macOS Floating Dock & Window Sync ---------------- */
  function findMatchingWindow(matcher) {
    return trackedWindows.find((w) => {
      const hay = `${w.app_id} ${w.title}`.toLowerCase();
      return matcher.some((m) => hay.includes(m));
    }) || null;
  }

  function renderDock() {
    if (mode !== "dock") return;
    const pinnedEl = document.getElementById("dock-pinned");
    const runningEl = document.getElementById("dock-running");
    const dividerEl = document.getElementById("dock-divider");
    if (!pinnedEl || !runningEl) return;

    const pinned = [
      { title: "游戏与应用中心", cmd: "games:open", kind: "games", match: ["游戏与启动中心"] },
      { title: "Steam", cmd: "launch:steam", kind: "steam", match: ["steam"] },
      { title: "Gamescope 主机大屏", cmd: "launch:steam-gamepadui", kind: "gamepad", match: ["gamescope"] },
      { title: "Chromium 浏览器", cmd: "launch:browser", kind: "browser", match: ["chromium"] },
      { title: "Thunar 文件管理", cmd: "launch:files", kind: "files", match: ["thunar"] },
      { title: "Foot 终端", cmd: "launch:terminal", kind: "terminal", match: ["foot"] },
      { title: "系统设置", cmd: "settings:open:display", kind: "settings", match: ["spinelos 系统设置"] }
    ];
    if (isLiveEnv) {
      pinned.push({ title: "安装 SpinelOS 到硬盘", cmd: "installer:open", kind: "installer", match: ["安装 spinel"] });
    }

    pinnedEl.innerHTML = "";
    const matchedIds = new Set();

    const createDockItem = (label, cmd, kind, win, closable) => {
      const btn = document.createElement("button");
      btn.className = "dock-item" + (win && win.activated && !win.minimized ? " active" : "") + (win && win.minimized ? " minimized" : "");
      btn.setAttribute("data-cmd", cmd);
      btn.setAttribute("aria-label", label);

      const tip = document.createElement("span");
      tip.className = "dock-tooltip";
      tip.textContent = label;

      const iconWrap = document.createElement("span");
      iconWrap.className = "dock-icon";
      iconWrap.innerHTML = makeSvgIcon(kind);

      btn.append(tip, iconWrap);
      if (win) {
        const dot = document.createElement("span");
        dot.className = "dock-dot";
        btn.appendChild(dot);
      }
      if (closable && win) {
        btn.addEventListener("contextmenu", (ev) => {
          ev.preventDefault();
          sendCommand(`window:close:${win.id}`);
        });
      }
      return btn;
    };

    pinned.forEach((p) => {
      const win = findMatchingWindow(p.match);
      if (win) matchedIds.add(win.id);
      const label = win ? `${p.title}` : p.title;
      pinnedEl.appendChild(createDockItem(label, win ? `window:toggle:${win.id}` : p.cmd, p.kind, win, false));
    });

    const extraWins = trackedWindows.filter((w) => !matchedIds.has(w.id));
    runningEl.innerHTML = "";
    if (dividerEl) dividerEl.classList.toggle("hidden", extraWins.length === 0);

    extraWins.forEach((w) => {
      const label = w.title || w.app_id || "应用窗口";
      const kind = matchAppKind(`${w.app_id} ${w.title}`);
      runningEl.appendChild(createDockItem(label, `window:toggle:${w.id}`, kind, w, true));
    });

    const totalItems = pinned.length + extraWins.length;
    const width = Math.max(460, totalItems * 58 + (extraWins.length > 0 ? 48 : 32) + 84);
    sendCommand(`dock:resize:${width}`);
  }

  window.__spinelWindows = (list) => {
    trackedWindows = Array.isArray(list) ? list : [];
    const activeAppEl = document.getElementById("bar-active-app");
    if (activeAppEl) {
      const activeWin = trackedWindows.find((w) => w.activated) || trackedWindows[0];
      activeAppEl.textContent = activeWin ? (activeWin.title || activeWin.app_id || "应用") : "访达";
    }
    renderDock();
  };

  /* ---------------- 3. Performance Control Strip ---------------- */
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
    const ccGmCircle = document.getElementById("cc-gm-circle");
    const ccGmVal = document.getElementById("cc-gm-val");
    if (ccGmCircle) ccGmCircle.classList.toggle("active-blue", Boolean(perfState.gamemode));
    if (ccGmVal) ccGmVal.textContent = perfState.gamemode ? "已开启" : "已关闭";

    const ccMhCircle = document.getElementById("cc-mh-circle");
    const ccMhVal = document.getElementById("cc-mh-val");
    if (ccMhCircle) ccMhCircle.classList.toggle("active-indigo", perfState.mangohud !== "off");
    if (ccMhVal) {
      const mhMap = { off: "已关闭", compact: "精简监控", full: "详细监控" };
      ccMhVal.textContent = mhMap[perfState.mangohud] || perfState.mangohud;
    }

    const ccFsrOrb = document.getElementById("cc-btn-fsr");
    if (ccFsrOrb) ccFsrOrb.classList.toggle("active", Boolean(perfState.fsr));

    const preview = document.getElementById("perf-cmd-preview");
    if (preview) preview.textContent = buildLaunchOptionString(perfState);
  }

  function sendPerfUpdate(patch) {
    perfState = { ...perfState, ...patch };
    renderPerfState();
    sendCommand([
      "perf:set",
      `mangohud=${perfState.mangohud}`,
      `fps_limit=${perfState.fps_limit}`,
      `fsr=${perfState.fsr ? "true" : "false"}`,
      `gamemode=${perfState.gamemode ? "true" : "false"}`,
      `render_res=${perfState.render_res}`
    ].join("\n"));
  }

  window.__spinelPerfConfig = (cfg) => {
    perfState = { ...perfState, ...cfg };
    renderPerfState();
  };

  /* ---------------- 4. Steam Games & Launchpad Apps ---------------- */
  function renderSteamGames() {
    const grid = document.getElementById("steam-game-grid");
    const countBadge = document.getElementById("steam-count-badge");
    if (!grid) return;

    const query = (document.getElementById("game-search-input")?.value || "").trim().toLowerCase();
    const filtered = query
      ? steamGames.filter((g) => (g.name || "").toLowerCase().includes(query))
      : steamGames;

    if (countBadge) countBadge.textContent = `${steamGames.length} 款游戏`;
    grid.innerHTML = "";

    if (filtered.length === 0) {
      const empty = document.createElement("div");
      empty.className = "lib-empty";
      const msg = document.createElement("p");
      msg.textContent = steamGames.length === 0
        ? "暂未检测到本地已安装的 Steam 游戏。登录后下载任意游戏，点击“扫描游戏库”即可在这里一键启动。"
        : "没有匹配搜索关键字的游戏。";
      const actions = document.createElement("div");
      actions.className = "footer-actions";
      const b1 = document.createElement("button");
      b1.className = "btn primary small";
      b1.setAttribute("data-cmd", "launch:steam");
      b1.textContent = "启动 Steam";
      const b2 = document.createElement("button");
      b2.className = "btn glow small";
      b2.setAttribute("data-cmd", "launch:steam-gamepadui");
      b2.textContent = "进入主机大屏";
      actions.append(b1, b2);
      empty.append(msg, actions);
      grid.appendChild(empty);
      return;
    }

    filtered.forEach((g) => {
      const card = document.createElement("article");
      card.className = "game-card";
      const poster = document.createElement("div");
      poster.className = "game-poster";
      const fallback = document.createElement("div");
      fallback.className = "game-poster-fallback hidden";
      fallback.textContent = g.name;

      const img = document.createElement("img");
      img.alt = g.name;
      const sources = [];
      if (g.cover_uri) sources.push(g.cover_uri);
      sources.push(
        `https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/${g.appid}/library_600x900.jpg`,
        `https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/${g.appid}/header.jpg`
      );
      let step = 0;
      img.src = sources[0];
      img.onerror = () => {
        step += 1;
        if (step < sources.length) img.src = sources[step];
        else {
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
      const btn = document.createElement("button");
      btn.className = "btn primary small game-launch-btn";
      btn.textContent = "立即启动";
      btn.setAttribute("data-cmd", `steam:launch:${g.appid}`);

      meta.append(title, sub, btn);
      card.append(poster, meta);
      grid.appendChild(card);
    });
  }

  function renderLaunchpadApps() {
    const grid = document.getElementById("launchpad-app-grid");
    if (!grid) return;
    grid.innerHTML = "";
    desktopApps.forEach((app) => {
      const btn = document.createElement("button");
      btn.className = "launchpad-item";
      btn.setAttribute("data-cmd", `app:launch:${app.id}`);
      if (app.icon_uri) {
        const img = document.createElement("img");
        img.src = app.icon_uri;
        img.alt = app.name;
        img.onerror = () => { img.outerHTML = makeSvgIcon(matchAppKind(app.id)); };
        btn.appendChild(img);
      } else {
        const wrap = document.createElement("div");
        wrap.innerHTML = makeSvgIcon(matchAppKind(app.id));
        btn.appendChild(wrap.firstElementChild);
      }
      const label = document.createElement("span");
      label.textContent = app.name;
      btn.appendChild(label);
      grid.appendChild(btn);
    });
  }

  window.__spinelSteamGames = (games) => {
    steamGames = Array.isArray(games) ? games : [];
    renderSteamGames();
    renderDesktopShortcuts();
  };

  window.__spinelDesktopApps = (apps) => {
    desktopApps = Array.isArray(apps) ? apps : [];
    renderLaunchpadApps();
  };

  /* ---------------- 5. Settings Window ---------------- */
  function switchSettingsTab(tab) {
    const tabs = ["wifi", "bt", "display", "appearance"];
    const target = tabs.includes(tab) ? tab : "wifi";
    tabs.forEach((t) => {
      const body = document.getElementById(`tab-${t}`);
      if (body) body.classList.toggle("hidden", t !== target);
    });
    document.querySelectorAll("#app-settings .tab-btn").forEach((btn) => {
      btn.classList.toggle("active", btn.getAttribute("data-cmd") === `settings:tab:${target}`);
    });
    if (target === "wifi") sendCommand("settings:wifi-list");
    else if (target === "bt") sendCommand("settings:bt-status");
    else if (target === "display") sendCommand("settings:display-scan");
  }

  window.__spinelOpenSettings = (tab) => switchSettingsTab(tab);

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
    if (ok) document.getElementById("wifi-connect-box")?.classList.add("hidden");
  };

  window.__spinelBtState = (data) => {
    btPowered = Boolean(data.powered);
    const badge = document.getElementById("bt-power-badge");
    const powerBtn = document.getElementById("btn-bt-power");
    const scanBtn = document.getElementById("btn-bt-scan");
    if (badge) badge.textContent = btPowered ? "已开启" : "已关闭";
    if (powerBtn) powerBtn.textContent = btPowered ? "关闭蓝牙" : "开启蓝牙";
    if (scanBtn) { scanBtn.disabled = false; scanBtn.textContent = "扫描手柄 (6s)"; }

    const listEl = document.getElementById("bt-device-list");
    if (!listEl) return;
    listEl.innerHTML = "";
    const devs = Array.isArray(data.devices) ? data.devices : [];
    if (devs.length === 0) {
      listEl.innerHTML = `<div class="disk-empty">${btPowered ? "暂无发现的蓝牙设备，请将手柄置于配对模式后扫描。" : "蓝牙已关闭，请先开启蓝牙。"}</div>`;
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
    });
  };

  window.__spinelDisplayAction = (ok, msg) => showFeedback("display-feedback", msg || (ok ? "已应用显示设置" : "应用失败"), !ok);

  /* ---------------- 6. Graphical Disk Installer ---------------- */
  function showFormError(msg) {
    const el = document.getElementById("inst-form-error");
    if (!el) return;
    el.classList.toggle("hidden", !msg);
    el.textContent = msg || "";
  }

  function setInstallMode(nextMode) {
    installMode = nextMode === "partition" ? "partition" : "disk";
    document.getElementById("inst-disk-section")?.classList.toggle("hidden", installMode !== "disk");
    document.getElementById("inst-part-section")?.classList.toggle("hidden", installMode !== "partition");
    document.getElementById("btn-mode-disk")?.classList.toggle("active", installMode === "disk");
    document.getElementById("btn-mode-part")?.classList.toggle("active", installMode === "partition");
    const confirmLabel = document.getElementById("inst-confirm-label");
    if (confirmLabel) {
      confirmLabel.textContent = installMode === "disk"
        ? "我已确认所选硬盘无重要数据，允许清空整块硬盘并安装 SpinelOS（测试建议优先使用空盘或副盘）。"
        : "我已确认所选根分区 (/) 无重要数据，允许将其格式化为 ext4（其余分区与原 EFI 文件将保留）。";
    }
    showFormError("");
  }

  function submitInstaller() {
    showFormError("");
    const rootPart = document.getElementById("inst-root-part")?.value || "";
    const efiPart = document.getElementById("inst-efi-part")?.value || "";
    const osProber = document.getElementById("inst-os-prober")?.checked ?? true;

    if (installMode === "disk") {
      if (!selectedDisk) { showFormError("请先选择一个目标硬盘。"); return; }
    } else {
      if (!rootPart) { showFormError("请选择要格式化为根分区 (/) 的目标分区。"); return; }
      if (isUefiMode && !efiPart) { showFormError("UEFI 模式下请选择一个已有的 FAT32 EFI 分区。"); return; }
      if (isUefiMode && rootPart === efiPart) { showFormError("根分区和 EFI 分区不能选择同一个分区。"); return; }
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

    sendCommand([
      "installer:start",
      `mode=${installMode}`,
      `disk=${selectedDisk}`,
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
    installRunning = Boolean(env.installing);
    isUefiMode = Boolean(env.uefi);
    const badge = document.getElementById("inst-env-badge");
    if (badge) badge.textContent = `${env.is_live ? "Live ISO" : "已安装环境"} · ${env.uefi ? "UEFI GPT" : "Legacy BIOS"}`;
    document.getElementById("inst-efi-wrap")?.classList.toggle("hidden", !isUefiMode);

    const listEl = document.getElementById("inst-disk-list");
    if (!listEl) return;
    const rawDevices = env.lsblk && Array.isArray(env.lsblk.blockdevices) ? env.lsblk.blockdevices : [];
    const disks = rawDevices.filter((d) => d.type === "disk" && !/^loop|^zram/.test(d.name || ""));

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

    if (disks.length === 0) {
      listEl.innerHTML = `<div class="disk-empty">未检测到可用磁盘设备</div>`;
      selectedDisk = "";
      return;
    }

    listEl.innerHTML = "";
    let stillValid = false;
    disks.forEach((d) => {
      const devPath = `/dev/${d.name}`;
      const isBoot = env.boot_disk && d.name === env.boot_disk;
      const isRemovable = d.rm === true || d.rm === 1 || d.rm === "1" || d.tran === "usb";
      if (!isBoot && selectedDisk === devPath) stillValid = true;

      const item = document.createElement("div");
      item.className = "disk-item" + (isBoot ? " disabled" : "") + (selectedDisk === devPath && !isBoot ? " selected" : "");
      const left = document.createElement("div"); left.className = "disk-left";
      const radio = document.createElement("input"); radio.type = "radio"; radio.name = "target_disk";
      radio.disabled = Boolean(isBoot); radio.checked = selectedDisk === devPath && !isBoot;
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
          selectedDisk = devPath;
          listEl.querySelectorAll(".disk-item").forEach((el) => el.classList.remove("selected"));
          item.classList.add("selected");
          radio.checked = true;
        });
      }
      listEl.appendChild(item);
    });
    if (!stillValid) selectedDisk = "";
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
    installRunning = false;
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

  /* ---------------- 7. Events, Context Menu & Gamepad ---------------- */
  if (mode === "desktop") {
    const canvas = document.getElementById("desktop-canvas");
    const menu = document.getElementById("desktop-ctx-menu");
    canvas?.addEventListener("contextmenu", (ev) => {
      ev.preventDefault();
      if (!menu) return;
      menu.style.left = `${Math.min(ev.clientX, window.innerWidth - 220)}px`;
      menu.style.top = `${Math.min(ev.clientY, window.innerHeight - 260)}px`;
      menu.classList.remove("hidden");
    });
    document.addEventListener("click", () => menu?.classList.add("hidden"));
  }

  document.getElementById("game-search-input")?.addEventListener("input", renderSteamGames);

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

    if (cmd === "installer:mode:disk") setInstallMode("disk");
    else if (cmd === "installer:mode:partition") setInstallMode("partition");
    else if (cmd === "installer:submit") submitInstaller();
    else if (cmd === "installer:back") {
      if (!installRunning) {
        document.getElementById("inst-progress-view").classList.add("hidden");
        document.getElementById("inst-form-view").classList.remove("hidden");
      }
    } else if (cmd.startsWith("settings:tab:") && mode === "settings") {
      switchSettingsTab(cmd.split(":")[2]);
    } else if (cmd === "settings:wifi-scan") {
      const btn = document.getElementById("btn-wifi-rescan");
      if (btn) { btn.disabled = true; btn.textContent = "扫描中..."; }
      sendCommand("settings:wifi-scan");
    } else if (cmd === "settings:wifi-cancel") {
      document.getElementById("wifi-connect-box")?.classList.add("hidden");
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
    } else if (cmd === "perf:cycle-mangohud") {
      const order = ["off", "compact", "full"];
      const next = order[(order.indexOf(perfState.mangohud) + 1) % order.length];
      sendPerfUpdate({ mangohud: next });
    } else if (cmd === "perf:copy") {
      const str = buildLaunchOptionString(perfState);
      navigator.clipboard?.writeText(str).then(() => {
        const btn = document.getElementById("btn-copy-launch");
        if (btn) { btn.textContent = "已复制!"; setTimeout(() => { btn.textContent = "复制 Steam 启动参数"; }, 1500); }
      }).catch(() => {});
    } else {
      sendCommand(cmd);
    }
  });

  function initGamepadNavigation() {
    if ((mode !== "desktop" && mode !== "games" && mode !== "settings" && mode !== "installer") ||
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
      return Array.from(activeRootEl.querySelectorAll("button:not(:disabled), input:not(:disabled), select:not(:disabled)")).filter((el) => {
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
      const changed = isLiveEnv !== data.is_live;
      isLiveEnv = data.is_live;
      const modeChip = document.getElementById("desktop-mode-chip");
      if (modeChip) modeChip.textContent = isLiveEnv ? "Live ISO 模式" : "本地硬盘模式";
      const aboutMode = document.getElementById("about-mode");
      if (aboutMode) aboutMode.textContent = isLiveEnv ? "Live USB / ISO 体验模式" : "本地硬盘持久化系统";
      document.getElementById("btn-desk-install")?.classList.toggle("hidden", !isLiveEnv);
      if (changed) {
        renderDesktopShortcuts();
        renderDock();
      }
    }
  };

  if (mode === "settings") switchSettingsTab(initialTab);
  if (mode === "desktop") renderDesktopShortcuts();
  if (mode === "dock") renderDock();

  renderPerfState();
  initGamepadNavigation();
  updateClock();
  setInterval(updateClock, 15000);
  sendCommand("telemetry:refresh");
})();
