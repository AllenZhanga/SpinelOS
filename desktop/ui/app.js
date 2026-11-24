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
    store: "app-store",
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
      store: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#38bdf8"/>
          <stop offset="50%" stop-color="#0ea5e9"/>
          <stop offset="100%" stop-color="#1d4ed8"/>
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
      chrome: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#ffffff"/>
          <stop offset="100%" stop-color="#e2e8f0"/>
        </linearGradient>`,
      chromium: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#93c5fd"/>
          <stop offset="100%" stop-color="#2563eb"/>
        </linearGradient>`,
      firefox: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#4338ca"/>
          <stop offset="55%" stop-color="#7c3aed"/>
          <stop offset="100%" stop-color="#ea580c"/>
        </linearGradient>`,
      heroic: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#0f172a"/>
          <stop offset="55%" stop-color="#1e3a8a"/>
          <stop offset="100%" stop-color="#0284c7"/>
        </linearGradient>`,
      lutris: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#fbbf24"/>
          <stop offset="55%" stop-color="#f97316"/>
          <stop offset="100%" stop-color="#c2410c"/>
        </linearGradient>`,
      protonup: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#2dd4bf"/>
          <stop offset="100%" stop-color="#0f766e"/>
        </linearGradient>`,
      discord: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#818cf8"/>
          <stop offset="55%" stop-color="#5865f2"/>
          <stop offset="100%" stop-color="#4338ca"/>
        </linearGradient>`,
      obs: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#334155"/>
          <stop offset="100%" stop-color="#0f172a"/>
        </linearGradient>`,
      vlc: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#fb923c"/>
          <stop offset="100%" stop-color="#c2410c"/>
        </linearGradient>`,
      qbittorrent: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#38bdf8"/>
          <stop offset="100%" stop-color="#1d4ed8"/>
        </linearGradient>`,
      gimp: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#a78bfa"/>
          <stop offset="55%" stop-color="#6366f1"/>
          <stop offset="100%" stop-color="#312e81"/>
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
      store: `
        <path d="M32 16L20 44" stroke="#ffffff" stroke-width="5" stroke-linecap="round"/>
        <path d="M32 16L44 44" stroke="#ffffff" stroke-width="5" stroke-linecap="round"/>
        <path d="M17 37.5h30" stroke="#ffffff" stroke-width="4.6" stroke-linecap="round"/>
        <circle cx="45" cy="44" r="2.2" fill="#bae6fd"/>`,
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
      chrome: `
        <circle cx="32" cy="32" r="20" fill="#ea4335"/>
        <path d="M32 32l17.3-10A20 20 0 0 1 32 52z" fill="#fbbc05"/>
        <path d="M32 32L32 52A20 20 0 0 1 14.7 22z" fill="#34a853"/>
        <circle cx="32" cy="32" r="10.2" fill="#ffffff"/>
        <circle cx="32" cy="32" r="7.4" fill="#1a73e8"/>`,
      chromium: `
        <circle cx="32" cy="32" r="20" fill="#3b82f6"/>
        <path d="M32 32l17.3-10A20 20 0 0 1 32 52z" fill="#93c5fd"/>
        <path d="M32 32L32 52A20 20 0 0 1 14.7 22z" fill="#60a5fa"/>
        <circle cx="32" cy="32" r="10.2" fill="#ffffff"/>
        <circle cx="32" cy="32" r="7.4" fill="#1d4ed8"/>`,
      firefox: `
        <circle cx="32" cy="33" r="17" fill="#6366f1"/>
        <path d="M47 26c-2-8-10-12-17-10 4 2 6 6 4 10-3-2-7-2-10 1-5 5-4 14 2 18 7 4 16 2 20-5 2-4 3-9 1-14z" fill="#fb923c"/>
        <circle cx="32" cy="33" r="7.5" fill="#fde047" opacity="0.92"/>`,
      heroic: `
        <path d="M32 13l16 7v12c0 10.5-6.8 18-16 21-9.2-3-16-10.5-16-21V20l16-7z" fill="rgba(255,255,255,0.18)" stroke="#ffffff" stroke-width="2.8" stroke-linejoin="round"/>
        <path d="M26 24v16M38 24v16M26 32h12" stroke="#38bdf8" stroke-width="3.6" stroke-linecap="round"/>`,
      lutris: `
        <circle cx="32" cy="32" r="18" fill="rgba(255,255,255,0.22)" stroke="#ffffff" stroke-width="2.6"/>
        <path d="M23 36l9-16 9 16H23z" fill="#ffffff"/>
        <circle cx="32" cy="33" r="3" fill="#ea580c"/>`,
      protonup: `
        <circle cx="32" cy="32" r="17" fill="none" stroke="rgba(255,255,255,0.72)" stroke-width="2.4" stroke-dasharray="5 3"/>
        <path d="M32 42V22m0 0-7 7m7-7 7 7" fill="none" stroke="#ffffff" stroke-width="3.6" stroke-linecap="round" stroke-linejoin="round"/>`,
      discord: `
        <rect x="14" y="20" width="36" height="24" rx="11" fill="#ffffff"/>
        <circle cx="25.5" cy="32" r="3.2" fill="#4f46e5"/>
        <circle cx="38.5" cy="32" r="3.2" fill="#4f46e5"/>`,
      obs: `
        <circle cx="32" cy="32" r="19" fill="none" stroke="rgba(255,255,255,0.88)" stroke-width="2.8"/>
        <circle cx="32" cy="24" r="6" fill="rgba(255,255,255,0.92)"/>
        <circle cx="24.5" cy="36.5" r="6" fill="rgba(255,255,255,0.78)"/>
        <circle cx="39.5" cy="36.5" r="6" fill="rgba(255,255,255,0.65)"/>`,
      vlc: `
        <path d="M28 15h8l8 29H20l8-29z" fill="#ffffff"/>
        <path d="M25.2 25h13.6M22.5 34h19" stroke="#ea580c" stroke-width="4.2"/>
        <rect x="16" y="43" width="32" height="5.5" rx="2.5" fill="#ffffff"/>`,
      qbittorrent: `
        <circle cx="32" cy="32" r="18" fill="rgba(255,255,255,0.18)" stroke="#ffffff" stroke-width="2.6"/>
        <path d="M23 30c0-4.4 3.6-8 8-8h2v16h-2a8 8 0 0 1-8-8zm16-6v16" fill="none" stroke="#ffffff" stroke-width="3.4" stroke-linecap="round"/>`,
      gimp: `
        <path d="M17 44c5-2 10-8 16-14l10-11 4 4-11 10c-6 6-12 11-19 11z" fill="#ffffff"/>
        <circle cx="44" cy="19" r="3.2" fill="#fde047"/>`,
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
        <rect x="0" y="0" width="64" height="64" rx="15" fill="url(#${uid}_bg)"/>
        ${body}
        <rect x="0" y="0" width="64" height="64" rx="15" fill="url(#${uid}_sheen)"/>
        <rect x="0.75" y="0.75" width="62.5" height="62.5" rx="14.2" fill="none" stroke="rgba(255,255,255,0.28)" stroke-width="1.2"/>
      </svg>
    `;
  }

  function matchAppKind(appIdOrTitle) {
    const s = (appIdOrTitle || "").toLowerCase();
    if (s.includes("steam")) return "steam";
    if (s.includes("gamescope")) return "gamepad";
    if (s.includes("chromium")) return "chromium";
    if (s.includes("chrome")) return "chrome";
    if (s.includes("firefox")) return "firefox";
    if (s.includes("heroic")) return "heroic";
    if (s.includes("lutris")) return "lutris";
    if (s.includes("protonup") || s.includes("pupgui")) return "protonup";
    if (s.includes("discord")) return "discord";
    if (s.includes("obs")) return "obs";
    if (s.includes("vlc")) return "vlc";
    if (s.includes("qbittorrent")) return "qbittorrent";
    if (s.includes("gimp")) return "gimp";
    if (s.includes("browser")) return "browser";
    if (s.includes("thunar") || s.includes("files")) return "files";
    if (s.includes("foot") || s.includes("terminal")) return "terminal";
    if (s.includes("应用商店") || s.includes("app-store")) return "store";
    if (s.includes("游戏与启动中心") || s.includes("app-games")) return "games";
    if (s.includes("系统设置") || s.includes("settings")) return "settings";
    if (s.includes("安装") || s.includes("installer")) return "installer";
    return "app";
  }

  function findAppIconUri(keys) {
    if (!Array.isArray(keys) || keys.length === 0 || desktopApps.length === 0) return "";
    const lowerKeys = keys.map((k) => String(k || "").trim().toLowerCase()).filter(Boolean);
    if (lowerKeys.length === 0) return "";
    for (const k of lowerKeys) {
      const exact = desktopApps.find((a) => a.id.toLowerCase() === k && a.icon_uri);
      if (exact) return exact.icon_uri;
    }
    for (const k of lowerKeys) {
      const hit = desktopApps.find((a) => {
        if (!a.icon_uri) return false;
        const hay = `${a.id} ${a.icon} ${a.name}`.toLowerCase();
        return hay.includes(k);
      });
      if (hit) return hit.icon_uri;
    }
    return "";
  }

  function fillIconContainer(el, iconUri, fallbackKind, altText) {
    el.innerHTML = "";
    if (iconUri) {
      const img = document.createElement("img");
      img.src = iconUri;
      img.alt = altText || "";
      img.onerror = () => { el.innerHTML = makeSvgIcon(fallbackKind); };
      el.appendChild(img);
    } else {
      el.innerHTML = makeSvgIcon(fallbackKind);
    }
  }

  /* ---------------- 1. Desktop Shortcuts & Wallpaper ---------------- */
  function renderDesktopShortcuts() {
    const grid = document.getElementById("desktop-shortcuts");
    if (!grid) return;
    grid.innerHTML = "";

    const baseShortcuts = [
      { label: "游戏中心", cmd: "games:open", kind: "games", appKeys: [] },
      { label: "应用商店", cmd: "store:open", kind: "store", appKeys: [] },
      { label: "Steam", cmd: "launch:steam", kind: "steam", appKeys: ["steam"] },
      { label: "主机大屏", cmd: "launch:steam-gamepadui", kind: "gamepad", appKeys: [] },
      { label: "文件管理", cmd: "launch:files", kind: "files", appKeys: ["thunar", "org.xfce.thunar"] },
      { label: "终端", cmd: "launch:terminal", kind: "terminal", appKeys: ["foot"] },
      { label: "系统设置", cmd: "settings:open:display", kind: "settings", appKeys: [] }
    ];
    if (isLiveEnv) {
      baseShortcuts.push({ label: "安装 SpinelOS", cmd: "installer:open", kind: "installer", appKeys: [] });
    }

    // Also automatically surface Store-installed apps (browsers, Heroic, Lutris, Discord, OBS, etc.) on Desktop
    const storeFeaturedIds = ["com.google.chrome", "chromium", "firefox", "com.heroicgameslauncher.hgl", "lutris", "discord", "com.obsproject.studio"];
    desktopApps.forEach((app) => {
      if (storeFeaturedIds.includes(app.id.toLowerCase())) {
        baseShortcuts.push({
          label: app.name,
          cmd: `app:launch:${app.id}`,
          kind: matchAppKind(app.id),
          appKeys: [app.id]
        });
      }
    });

    baseShortcuts.forEach((sc) => {
      const btn = document.createElement("button");
      btn.className = "shortcut-item";
      btn.setAttribute("data-cmd", sc.cmd);
      const icon = document.createElement("div");
      icon.className = "shortcut-icon";
      fillIconContainer(icon, findAppIconUri(sc.appKeys), sc.kind, sc.label);
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
      icon.className = "shortcut-icon is-cover";
      fillIconContainer(icon, g.cover_uri || findAppIconUri(["steam"]), "steam", g.name);
      const label = document.createElement("span");
      label.className = "shortcut-label";
      label.textContent = g.name;
      btn.append(icon, label);
      grid.appendChild(btn);
    });
  }

  let activeThemeId = "tahoe-light";
  let availableThemes = [];
  document.body.classList.add("theme-light");

  function forceSoftwareRepaint() {
    if (!activeRootEl) return;
    // WebKitGTK software/non-composited mode may defer :root CSS var repaints until next layout/compositor pass
    document.documentElement.dataset.theme = activeThemeId;
    activeRootEl.style.opacity = "0.999";
    void activeRootEl.offsetHeight;
    window.requestAnimationFrame(() => {
      if (activeRootEl) activeRootEl.style.opacity = "";
    });
  }

  function applyUiTheme(t) {
    if (!t) return;
    activeThemeId = t.id || "crystal-aurora";
    const root = document.documentElement.style;
    const setVar = (k, v) => { if (v) root.setProperty(k, v); };
    setVar("--bg", t.ui_bg);
    setVar("--win-bg", t.ui_win_bg);
    setVar("--toolbar-bg", t.ui_toolbar_bg);
    setVar("--surface", t.ui_surface);
    setVar("--card-bg", t.ui_card);
    setVar("--input-bg", t.ui_input_bg);
    setVar("--border", t.ui_border);
    setVar("--text", t.ui_text);
    setVar("--muted", t.ui_muted);
    setVar("--accent", t.ui_accent);
    setVar("--accent-2", t.ui_accent2);
    if (activeRootEl && t.ui_win_bg) {
      activeRootEl.style.background = t.ui_win_bg;
    }
    document.body.classList.toggle("theme-light", Boolean(t.is_light));
    forceSoftwareRepaint();
  }

  window.__spinelThemeData = (data) => {
    if (!data) return;
    if (Array.isArray(data.themes)) availableThemes = data.themes;
    if (data.active) applyUiTheme(data.active);
    const grid = document.getElementById("theme-grid");
    if (!grid || !Array.isArray(availableThemes)) return;
    grid.innerHTML = "";
    availableThemes.forEach((t) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "wp-card" + (t.id === activeThemeId ? " active" : "");
      btn.setAttribute("data-cmd", `theme:set:${t.id}`);

      const swatch = document.createElement("div");
      swatch.className = "wp-swatch";
      swatch.style.background = `radial-gradient(circle at 22% 22%, ${t.wp_glow1 || t.ui_accent}, transparent 58%), radial-gradient(circle at 78% 76%, ${t.wp_glow2 || t.ui_accent2}, transparent 60%), linear-gradient(135deg, ${t.wp_c0 || t.ui_bg}, ${t.wp_c1 || t.ui_bg})`;

      const meta = document.createElement("div");
      meta.className = "theme-card-meta";
      const title = document.createElement("strong");
      title.textContent = (t.name || t.id) + (t.id === activeThemeId ? " · 当前" : "");
      const sub = document.createElement("small");
      sub.textContent = t.subtitle || (t.builtin ? "内置风格" : "自定义扩展 (.theme)");
      meta.append(title, sub);

      btn.append(swatch, meta);
      grid.appendChild(btn);
    });
  };

  window.__spinelWallpaper = () => {};

  /* Custom in-DOM Select Dropdown (avoids oversized/invisible WebKitGTK6 native popup) */
  let openSelectMenu = null;
  let openSelectOwner = null;

  function closeCustomSelectMenu() {
    if (openSelectMenu) {
      openSelectMenu.remove();
      openSelectMenu = null;
      openSelectOwner = null;
    }
  }

  document.addEventListener("pointerdown", (ev) => {
    if (!openSelectMenu) return;
    if (openSelectMenu.contains(ev.target)) return;
    if (openSelectOwner && openSelectOwner.contains(ev.target)) return;
    closeCustomSelectMenu();
  });
  document.addEventListener("keydown", (ev) => {
    if (ev.key === "Escape") closeCustomSelectMenu();
  });
  window.addEventListener("resize", closeCustomSelectMenu);

  function enhanceSelect(sel) {
    if (!sel || sel.dataset.spEnhanced === "1") return;
    sel.dataset.spEnhanced = "1";

    const wrap = document.createElement("div");
    wrap.className = "sp-select";
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "sp-select-btn";

    sel.parentNode.insertBefore(wrap, sel);
    wrap.append(btn, sel);
    sel.classList.add("sp-native-hidden");

    const syncLabel = () => {
      const opt = sel.options[sel.selectedIndex] || sel.options[0];
      btn.textContent = opt ? opt.textContent : "请选择";
    };
    syncLabel();

    sel.addEventListener("change", syncLabel);
    const observer = new MutationObserver(syncLabel);
    observer.observe(sel, { childList: true, subtree: true, attributes: true });

    btn.addEventListener("click", (ev) => {
      ev.stopPropagation();
      if (openSelectOwner === wrap) {
        closeCustomSelectMenu();
        return;
      }
      closeCustomSelectMenu();

      const rect = btn.getBoundingClientRect();
      const menu = document.createElement("div");
      menu.className = "sp-select-menu";
      const minW = Math.max(rect.width, 150);
      menu.style.minWidth = `${minW}px`;
      menu.style.maxWidth = `${Math.min(window.innerWidth - 20, Math.max(rect.width + 64, 280))}px`;

      Array.from(sel.options).forEach((opt, idx) => {
        const item = document.createElement("button");
        item.type = "button";
        item.className = "sp-select-item" + (idx === sel.selectedIndex ? " active" : "");
        item.textContent = opt.textContent;
        item.disabled = opt.disabled;
        item.addEventListener("click", (cev) => {
          cev.stopPropagation();
          sel.selectedIndex = idx;
          syncLabel();
          sel.dispatchEvent(new Event("change", { bubbles: true }));
          closeCustomSelectMenu();
        });
        menu.appendChild(item);
      });

      document.body.appendChild(menu);
      const mh = Math.min(menu.scrollHeight || 200, 230);
      const top = rect.bottom + 4 + mh <= window.innerHeight - 8
        ? rect.bottom + 4
        : Math.max(8, rect.top - mh - 4);
      const left = Math.min(rect.left, Math.max(8, window.innerWidth - minW - 12));
      menu.style.top = `${top}px`;
      menu.style.left = `${left}px`;

      openSelectMenu = menu;
      openSelectOwner = wrap;
    });
  }

  function syncAllCustomSelects() {
    document.querySelectorAll("select").forEach((sel) => enhanceSelect(sel));
  }

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
      { title: "游戏与应用中心", cmd: "games:open", kind: "games", appKeys: [], match: ["游戏与启动中心"] },
      { title: "应用商店", cmd: "store:open", kind: "store", appKeys: [], match: ["spinelos 应用商店"] },
      { title: "Steam", cmd: "launch:steam", kind: "steam", appKeys: ["steam"], match: ["steam"] },
      { title: "Gamescope 主机大屏", cmd: "launch:steam-gamepadui", kind: "gamepad", appKeys: [], match: ["gamescope"] },
      { title: "Thunar 文件管理", cmd: "launch:files", kind: "files", appKeys: ["thunar", "org.xfce.thunar"], match: ["thunar"] },
      { title: "Foot 终端", cmd: "launch:terminal", kind: "terminal", appKeys: ["foot"], match: ["foot"] },
      { title: "系统设置", cmd: "settings:open:display", kind: "settings", appKeys: [], match: ["spinelos 系统设置"] }
    ];
    if (isLiveEnv) {
      pinned.push({ title: "安装 SpinelOS 到硬盘", cmd: "installer:open", kind: "installer", appKeys: [], match: ["安装 spinel"] });
    }

    pinnedEl.innerHTML = "";
    const matchedIds = new Set();

    const createDockItem = (label, cmd, iconUri, fallbackKind, win, closable) => {
      const btn = document.createElement("button");
      btn.className = "dock-item" + (win && win.activated && !win.minimized ? " active" : "") + (win && win.minimized ? " minimized" : "");
      btn.setAttribute("data-cmd", cmd);
      btn.setAttribute("aria-label", label);

      const tip = document.createElement("span");
      tip.className = "dock-tooltip";
      tip.textContent = label;

      const iconWrap = document.createElement("span");
      iconWrap.className = "dock-icon";
      fillIconContainer(iconWrap, iconUri, fallbackKind, label);

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
      const iconUri = findAppIconUri(p.appKeys);
      pinnedEl.appendChild(createDockItem(p.title, win ? `window:toggle:${win.id}` : p.cmd, iconUri, p.kind, win, false));
    });

    const extraWins = trackedWindows.filter((w) => !matchedIds.has(w.id));
    runningEl.innerHTML = "";
    if (dividerEl) dividerEl.classList.toggle("hidden", extraWins.length === 0);

    extraWins.forEach((w) => {
      const label = w.title || w.app_id || "应用窗口";
      const kind = matchAppKind(`${w.app_id} ${w.title}`);
      const iconUri = findAppIconUri([w.app_id, w.title]);
      runningEl.appendChild(createDockItem(label, `window:toggle:${w.id}`, iconUri, kind, w, true));
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

    const needGs = cfg.render_res !== "native" || Number(cfg.fps_limit) > 0 || Boolean(cfg.fsr);
    if (needGs) {
      parts.push("gamescope", "-f");
      if (cfg.render_res === "720p") parts.push("-w", "1280", "-h", "720");
      else if (cfg.render_res === "900p") parts.push("-w", "1600", "-h", "900");
      else if (cfg.render_res === "1080p") parts.push("-w", "1920", "-h", "1080");
      if (Number(cfg.fps_limit) > 0) parts.push("-r", String(cfg.fps_limit));
      if (cfg.fsr) parts.push("-F", "fsr");
      if (cfg.mangohud !== "off") parts.push("--mangoapp");
      parts.push("--");
    }
    if (parts.length === 0) return "原生直出（无额外包装参数）";
    parts.push("%command%");
    return parts.join(" ");
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
    forceSoftwareRepaint();
  }

  window.__spinelPerfFeedback = (ok, msg) => showFeedback("perf-feedback", msg || "", !ok);

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
      const kind = matchAppKind(`${app.id} ${app.name}`);
      if (app.icon_uri && kind !== "files" && kind !== "terminal") {
        const img = document.createElement("img");
        img.src = app.icon_uri;
        img.alt = app.name;
        img.onerror = () => { img.outerHTML = makeSvgIcon(kind); };
        btn.appendChild(img);
      } else {
        const wrap = document.createElement("div");
        wrap.innerHTML = makeSvgIcon(kind);
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
    renderDesktopShortcuts();
    renderDock();
    renderLaunchpadApps();
    if (mode === "store") renderStoreApps();
  };

  /* ---------------- 5. Settings Window ---------------- */
  let activeLogSource = "apps";

  function switchSettingsTab(tab) {
    const tabs = ["wifi", "bt", "display", "perf", "appearance", "logs"];
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
    else if (target === "perf") sendCommand("perf:get");
    else if (target === "appearance") {
      sendCommand("theme:refresh");
      sendCommand("ime:status");
    }
    else if (target === "logs") sendCommand(`settings:logs:${activeLogSource}`);
  }

  window.__spinelImeState = (data) => {
    if (!data) return;
    const chip = document.getElementById("ime-status-chip");
    if (chip) {
      if (!data.installed) chip.textContent = "未安装（需 --full 更新）";
      else chip.textContent = (data.running ? "运行中 · " : "未启动 · ") +
        (data.engine === "rime" ? (data.rime_ice ? "Rime 雾凇拼音" : "Rime 中州韵") : "内置中文拼音");
    }
    document.getElementById("btn-ime-pinyin")?.classList.toggle("active", data.engine !== "rime");
    document.getElementById("btn-ime-rime")?.classList.toggle("active", data.engine === "rime");
    const iceBtn = document.getElementById("btn-ime-ice");
    if (iceBtn) {
      iceBtn.textContent = data.rime_ice ? "更新雾凇拼音词库 (已安装)" : "安装雾凇拼音词库 (rime-ice)";
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

  window.__spinelOpenSettings = (tab) => switchSettingsTab(tab);

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
      enhanceSelect(modeSel);
      enhanceSelect(scaleSel);
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
    syncAllCustomSelects();

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

  /* ---------------- 6.5. SpinelOS App Store ---------------- */
  let storeState = { flatpak_ready: true, sudo_nopass: false, busy: false, active_id: "", active_action: "", apps: [] };
  let storeCategory = "all";
  let pendingStoreOp = null;

  function renderStoreApps() {
    const grid = document.getElementById("store-app-grid");
    if (!grid) return;
    const customBtn = document.getElementById("btn-store-custom");
    if (customBtn) customBtn.disabled = storeState.busy;

    const query = (document.getElementById("store-search-input")?.value || "").trim().toLowerCase();
    const list = (storeState.apps || []).filter((app) => {
      const catMatch = storeCategory === "all" ||
        app.category === storeCategory ||
        (storeCategory === "游戏平台" && (app.category === "游戏平台" || app.category === "游戏工具")) ||
        (storeCategory === "社交与影音" && (app.category === "社交语音" || app.category === "直播录屏" || app.category === "影音播放"));
      if (!catMatch) return false;
      if (!query) return true;
      return `${app.name} ${app.pkg} ${app.summary} ${app.category}`.toLowerCase().includes(query);
    });

    grid.innerHTML = "";
    if (list.length === 0) {
      grid.innerHTML = `<div class="lib-empty">没有匹配的应用。你也可以在右上角直接输入 Pacman 包名或 Flatpak ID 安装。</div>`;
      return;
    }

    list.forEach((app) => {
      const card = document.createElement("article");
      card.className = "store-card glass";

      const head = document.createElement("div");
      head.className = "store-card-head";
      const iconWrap = document.createElement("div");
      iconWrap.className = "store-card-icon";
      // Always render unified SpinelOS squircle icons inside Store cards
      fillIconContainer(iconWrap, "", matchAppKind(`${app.id} ${app.pkg}`), app.name);

      const info = document.createElement("div");
      info.className = "store-card-info";
      const titleRow = document.createElement("div");
      titleRow.className = "store-card-title-row";
      const title = document.createElement("strong");
      title.textContent = app.name;
      const tag = document.createElement("span");
      tag.className = "tag";
      tag.textContent = app.badge;
      titleRow.append(title, tag);

      const sub = document.createElement("p");
      sub.className = "store-card-sub";
      sub.textContent = app.summary;
      info.append(titleRow, sub);
      head.append(iconWrap, info);

      const foot = document.createElement("div");
      foot.className = "store-card-foot";
      const pkgLabel = document.createElement("span");
      pkgLabel.className = "store-card-pkg";
      pkgLabel.textContent = app.pkg;

      const actions = document.createElement("div");
      actions.className = "footer-actions";
      const isActiveTarget = storeState.busy &&
        (storeState.active_id === app.id || storeState.active_id === app.pkg);

      if (app.installed) {
        const launchBtn = document.createElement("button");
        launchBtn.className = "btn primary small";
        launchBtn.textContent = "打开";
        launchBtn.setAttribute("data-cmd", `store:launch:${app.id}`);
        const rmBtn = document.createElement("button");
        rmBtn.className = "btn small danger";
        rmBtn.textContent = (isActiveTarget && storeState.active_action === "remove") ? "卸载中..." : "卸载";
        rmBtn.disabled = storeState.busy;
        if (storeState.busy && !isActiveTarget) rmBtn.title = "请等待当前操作完成";
        rmBtn.addEventListener("click", (ev) => {
          ev.stopPropagation();
          requestStoreOp("remove", { id: app.id, name: app.name, backend: app.backend, pkg: app.pkg });
        });
        actions.append(launchBtn, rmBtn);
      } else {
        const instBtn = document.createElement("button");
        instBtn.className = "btn primary small";
        instBtn.textContent = isActiveTarget ? "安装中..." : "下载安装";
        instBtn.disabled = storeState.busy;
        if (storeState.busy && !isActiveTarget) instBtn.title = "请等待当前队列完成";
        instBtn.addEventListener("click", (ev) => {
          ev.stopPropagation();
          requestStoreOp("install", { id: app.id, name: app.name, backend: app.backend, pkg: app.pkg });
        });
        actions.append(instBtn);
      }

      foot.append(pkgLabel, actions);
      card.append(head, foot);
      grid.appendChild(card);
    });
  }

  function executeStoreOp(action, op, password = "") {
    const logWrap = document.getElementById("store-log-wrap");
    const logBox = document.getElementById("store-log-box");
    const logTitle = document.getElementById("store-log-title");
    if (logWrap) logWrap.classList.remove("hidden");
    if (logBox) logBox.textContent = "";
    if (logTitle) logTitle.textContent = `${action === "install" ? "正在下载安装" : "正在卸载"}: ${op.name || op.pkg}`;
    storeState.busy = true;
    storeState.active_id = op.id || op.pkg || "";
    storeState.active_action = action;
    renderStoreApps();
    sendCommand([
      `store:${action}`,
      `id=${op.id || ""}`,
      `backend=${op.backend}`,
      `pkg=${op.pkg}`,
      `password=${password}`
    ].join("\n"));
  }

  function requestStoreOp(action, op) {
    if (storeState.busy) return;
    if (op.backend === "pacman" && !storeState.sudo_nopass) {
      pendingStoreOp = { action, op };
      const box = document.getElementById("store-pass-box");
      const title = document.getElementById("store-pass-title");
      const confirmBtn = document.getElementById("btn-store-pass-confirm");
      const input = document.getElementById("store-pass-input");
      if (title) title.textContent = `${action === "install" ? "安装" : "卸载"}系统软件 ${op.name || op.pkg} 需要管理员授权`;
      if (confirmBtn) confirmBtn.textContent = action === "install" ? "确认安装" : "确认卸载";
      if (input) input.value = "";
      box?.classList.remove("hidden");
      input?.focus();
      return;
    }
    executeStoreOp(action, op, "");
  }

  document.getElementById("store-pass-box")?.addEventListener("click", (ev) => {
    if (ev.target.id === "store-pass-box") {
      pendingStoreOp = null;
      ev.currentTarget.classList.add("hidden");
    }
  });
  document.addEventListener("keydown", (ev) => {
    if (ev.key === "Escape") {
      const box = document.getElementById("store-pass-box");
      if (box && !box.classList.contains("hidden")) {
        pendingStoreOp = null;
        box.classList.add("hidden");
      }
    }
  });

  window.__spinelStoreData = (data) => {
    storeState = { ...storeState, ...data };
    if (!storeState.busy) {
      storeState.active_id = "";
      storeState.active_action = "";
    }
    renderStoreApps();
  };

  window.__spinelStoreLog = (line) => {
    const logWrap = document.getElementById("store-log-wrap");
    const logBox = document.getElementById("store-log-box");
    if (logWrap) logWrap.classList.remove("hidden");
    if (logBox) {
      logBox.textContent += `${line}\n`;
      logBox.scrollTop = logBox.scrollHeight;
    }
  };

  window.__spinelStoreDone = (ok, targetId, action) => {
    storeState.busy = false;
    storeState.active_id = "";
    storeState.active_action = "";
    const logTitle = document.getElementById("store-log-title");
    if (logTitle) {
      logTitle.textContent = ok
        ? `${action === "install" ? "安装完成" : "卸载完成"}: ${targetId}`
        : `操作未完成: ${targetId}（请查看下方输出）`;
    }
    renderStoreApps();
  };

  /* ---------------- 7. Events, Context Menu & Gamepad ---------------- */
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

  // Allow macOS-style Cmd (Meta) clipboard shortcuts alongside Ctrl inside SpinelOS shell inputs/logs
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

  document.getElementById("game-search-input")?.addEventListener("input", renderSteamGames);
  document.getElementById("store-search-input")?.addEventListener("input", renderStoreApps);
  document.getElementById("store-pass-input")?.addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") {
      ev.preventDefault();
      document.querySelector('[data-cmd="store:pass-confirm"]')?.click();
    }
  });

  document.addEventListener("click", (ev) => {
    const storeCatTarget = ev.target.closest("[data-store-cat]");
    if (storeCatTarget) {
      ev.stopPropagation();
      storeCategory = storeCatTarget.getAttribute("data-store-cat") || "all";
      document.querySelectorAll("[data-store-cat]").forEach((btn) => {
        btn.classList.toggle("active", btn === storeCatTarget);
      });
      renderStoreApps();
      return;
    }

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
    document.getElementById("desktop-ctx-menu")?.classList.add("hidden");
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
    } else if (cmd === "store:custom-install") {
      const backend = document.getElementById("store-custom-backend")?.value || "pacman";
      const pkg = (document.getElementById("store-custom-pkg")?.value || "").trim();
      if (pkg) requestStoreOp("install", { id: "", name: pkg, backend, pkg });
    } else if (cmd === "store:pass-cancel") {
      pendingStoreOp = null;
      document.getElementById("store-pass-box")?.classList.add("hidden");
    } else if (cmd === "store:pass-confirm") {
      const inputEl = document.getElementById("store-pass-input");
      const pwd = inputEl?.value || "";
      if (!pwd) {
        inputEl?.focus();
        return;
      }
      document.getElementById("store-pass-box")?.classList.add("hidden");
      if (pendingStoreOp) {
        const cur = pendingStoreOp;
        pendingStoreOp = null;
        executeStoreOp(cur.action, cur.op, pwd);
      }
    } else if (cmd === "store:log-hide") {
      document.getElementById("store-log-wrap")?.classList.add("hidden");
    } else if (cmd.startsWith("theme:set:")) {
      const tid = cmd.slice(10);
      const chosen = availableThemes.find((t) => t.id === tid);
      if (chosen) {
        window.__spinelThemeData({ active: chosen, themes: availableThemes });
      }
      sendCommand(cmd);
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

  syncAllCustomSelects();
  if (mode === "settings") switchSettingsTab(initialTab);
  if (mode === "store") sendCommand("store:refresh");
  if (mode === "installer") sendCommand("installer:scan-disks");
  if (mode === "desktop") renderDesktopShortcuts();
  if (mode === "dock") renderDock();

  renderPerfState();
  initGamepadNavigation();
  updateClock();
  setInterval(updateClock, 15000);
  sendCommand("telemetry:refresh");
})();
