(() => {
  const params = new URLSearchParams(window.location.search);
  const mode = params.get("mode") || "desktop";
  const initialTab = params.get("tab") || (mode === "monitor" ? "procs" : "wifi");

  const modeRoots = {
    bar: "top-bar",
    desktop: "desktop-canvas",
    dock: "dock-shell",
    cc: "cc-shell",
    games: "app-games",
    store: "app-store",
    settings: "app-settings",
    installer: "app-installer",
    welcome: "app-welcome",
    doctor: "app-doctor",
    monitor: "app-monitor"
  };

  Object.values(modeRoots).forEach((id) => {
    const el = document.getElementById(id);
    if (el) el.classList.add("hidden");
  });
  const activeRootEl = document.getElementById(modeRoots[mode] || "desktop-canvas");
  if (activeRootEl) activeRootEl.classList.remove("hidden");

  const state = {
    isLiveEnv: false,
    steamGames: [],
    desktopApps: [],
    trackedWindows: [],
    selectedDisk: "",
    installMode: "disk",
    isUefiMode: true,
    installRunning: false,
    selectedWifiSsid: "",
    btPowered: false,
    activeThemeId: "tahoe-light",
    availableThemes: [],
    perfState: {
      mangohud: "compact",
      fps_limit: 0,
      fsr: false,
      gamemode: true,
      render_res: "native"
    },
    storeState: {
      flatpak_ready: true,
      sudo_nopass: false,
      busy: false,
      active_id: "",
      active_action: "",
      apps: []
    },
    pendingStoreOp: null,
    pendingStoreFocus: params.get("focus") || "",
    ovEditState: null,
    welcomeStep: 1
  };

  let ipcSeq = 0;
  const pendingIpc = new Map();

  function postRawBridge(payload) {
    if (!payload) return;
    if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.sysbridge) {
      window.webkit.messageHandlers.sysbridge.postMessage(payload);
    } else {
      console.log("[sysbridge mock]", payload);
    }
  }

  function sendCommand(cmd) {
    if (!cmd) return;
    if (typeof cmd === "object") {
      postRawBridge(JSON.stringify(cmd));
      return;
    }
    postRawBridge(cmd);
  }

  function invokeIpc(method, reqParams = {}) {
    const id = ++ipcSeq;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        pendingIpc.delete(id);
        reject(new Error(`IPC timeout: ${method}`));
      }, 15000);
      pendingIpc.set(id, { resolve, reject, timer });
      postRawBridge(JSON.stringify({ id, method, params: reqParams || {} }));
    });
  }

  window.__spinelIpcReply = (reply) => {
    if (!reply || reply.id === undefined) return;
    const entry = pendingIpc.get(reply.id);
    if (!entry) return;
    clearTimeout(entry.timer);
    pendingIpc.delete(reply.id);
    if (reply.ok) entry.resolve(reply);
    else entry.reject(new Error(reply.error || "ipc_error"));
  };

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
          <stop offset="0%" stop-color="#38bdf8"/>
          <stop offset="55%" stop-color="#0ea5e9"/>
          <stop offset="100%" stop-color="#0369a1"/>
        </linearGradient>
        <linearGradient id="${uid}_pocket" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="#ffffff"/>
          <stop offset="100%" stop-color="#dbeafe"/>
        </linearGradient>`,
      ime: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#60a5fa"/>
          <stop offset="100%" stop-color="#4f46e5"/>
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
      gravitymark: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#0ea5e9"/>
          <stop offset="52%" stop-color="#4f46e5"/>
          <stop offset="100%" stop-color="#0f172a"/>
        </linearGradient>`,
      glmark2: `
        <linearGradient id="${uid}_bg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0%" stop-color="#34d399"/>
          <stop offset="100%" stop-color="#0284c7"/>
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
        <path d="M12 20a5 5 0 0 1 5-5h9.5l3.8 4.2H47a5 5 0 0 1 5 5V44a5 5 0 0 1-5 5H17a5 5 0 0 1-5-5V20z" fill="rgba(255,255,255,0.42)"/>
        <rect x="16" y="19.5" width="32" height="10" rx="2.5" fill="rgba(255,255,255,0.72)"/>
        <rect x="12" y="23" width="40" height="26" rx="6" fill="url(#${uid}_pocket)"/>
        <path d="M21 32h22M21 38.5h15" stroke="#0284c7" stroke-width="2.8" stroke-linecap="round"/>`,
      ime: `
        <rect x="12" y="14" width="40" height="36" rx="9" fill="rgba(255,255,255,0.22)" stroke="rgba(255,255,255,0.9)" stroke-width="2.2"/>
        <text x="32" y="39" text-anchor="middle" font-size="22" font-weight="700" fill="#ffffff" font-family="Noto Sans CJK SC, sans-serif">文</text>`,
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
      gravitymark: `
        <circle cx="32" cy="32" r="18" fill="none" stroke="rgba(255,255,255,0.3)" stroke-width="2"/>
        <circle cx="32" cy="32" r="11" fill="none" stroke="#38bdf8" stroke-width="2.4" stroke-dasharray="16 6"/>
        <circle cx="32" cy="32" r="4.5" fill="#ffffff"/>
        <circle cx="46" cy="21" r="2.8" fill="#fbbf24"/>`,
      glmark2: `
        <path d="M16 41a18 18 0 1 1 32 0" fill="none" stroke="rgba(255,255,255,0.88)" stroke-width="3.4" stroke-linecap="round"/>
        <path d="M32 34l9-10" stroke="#fde047" stroke-width="3.4" stroke-linecap="round"/>
        <circle cx="32" cy="34" r="3.4" fill="#ffffff"/>`,
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
    if (s.includes("gravitymark")) return "gravitymark";
    if (s.includes("glmark2")) return "glmark2";
    if (s.includes("discord")) return "discord";
    if (s.includes("telegram")) return "telegram";
    if (s.includes("obs")) return "obs";
    if (s.includes("vlc")) return "vlc";
    if (s.includes("mpv")) return "mpv";
    if (s.includes("amberol")) return "amberol";
    if (s.includes("kdenlive")) return "kdenlive";
    if (s.includes("qbittorrent")) return "qbittorrent";
    if (s.includes("gimp")) return "gimp";
    if (s.includes("libreoffice") || s.includes("soffice")) return "libreoffice";
    if (s.includes("papers")) return "papers";
    if (s.includes("cups") || s.includes("printer")) return "printer";
    if (s.includes("obsidian")) return "obsidian";
    if (s.includes("vscode") || s === "code" || s.includes("visual studio code")) return "vscode";
    if (s.includes("docker")) return "docker";
    if (s.includes("podman")) return "podman";
    if (s.includes("ollama")) return "ollama";
    if (s.includes("vllm")) return "vllm";
    if (s.includes("llama")) return "llamacpp";
    if (s.includes("antigravity")) return "antigravity";
    if (s.includes("chatgpt")) return "chatgpt";
    if (s.includes("grok")) return "grok";
    if (s.includes("claude")) return "claude";
    if (s.includes("browser")) return "browser";
    if (s.includes("spinel-files") || s.includes("thunar") || s.includes("files") || s.includes("文件管理")) return "files";
    if (s.includes("spinel-terminal") || s.includes("ptyxis") || s.includes("foot") || s.includes("terminal") || s.includes("终端")) return "terminal";
    if (s.includes("fcitx") || s.includes("输入法")) return "ime";
    if (s.includes("应用商店") || s.includes("app-store")) return "store";
    if (s.includes("游戏与启动中心") || s.includes("app-games")) return "games";
    if (s.includes("系统设置") || s.includes("settings")) return "settings";
    if (s.includes("安装") || s.includes("installer")) return "installer";
    return "app";
  }

  function findAppIconUri(keys) {
    if (!Array.isArray(keys) || keys.length === 0 || state.desktopApps.length === 0) return "";
    const lowerKeys = keys.map((k) => String(k || "").trim().toLowerCase()).filter(Boolean);
    if (lowerKeys.length === 0) return "";
    for (const k of lowerKeys) {
      const exact = state.desktopApps.find((a) => a.id.toLowerCase() === k && a.icon_uri);
      if (exact) return exact.icon_uri;
    }
    for (const k of lowerKeys) {
      const hit = state.desktopApps.find((a) => {
        if (!a.icon_uri) return false;
        const aid = a.id.toLowerCase();
        const aico = String(a.icon || "").toLowerCase();
        const aname = a.name.toLowerCase();
        const hay = `${aid} ${aico} ${aname}`;
        return hay.includes(k) ||
          (aid.length >= 3 && k.includes(aid)) ||
          (aico.length >= 3 && k.includes(aico)) ||
          (aname.length >= 3 && k.includes(aname));
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

  /* ---------------- 1. Desktop Shortcuts, Theme & Wallpaper ---------------- */
  function renderDesktopShortcuts() {
    const grid = document.getElementById("desktop-shortcuts");
    if (!grid) return;
    grid.innerHTML = "";

    const baseShortcuts = [
      { label: "游戏中心", cmd: "games:open", kind: "games", appKeys: [] },
      { label: "应用商店", cmd: "store:open", kind: "store", appKeys: [] },
      { label: "Steam", cmd: "launch:steam", kind: "steam", appKeys: ["steam"] },
      { label: "主机大屏", cmd: "launch:steam-gamepadui", kind: "gamepad", appKeys: [] },
      { label: "文件管理", cmd: "launch:files", kind: "files", appKeys: ["org.spinelos.files", "thunar", "org.xfce.thunar"] },
      { label: "终端", cmd: "launch:terminal", kind: "terminal", appKeys: ["org.spinelos.terminal", "org.gnome.ptyxis", "ptyxis", "foot"] },
      { label: "系统设置", cmd: "settings:open:display", kind: "settings", appKeys: [] }
    ];
    if (state.isLiveEnv) {
      baseShortcuts.push({ label: "安装 Spinel OS", cmd: "installer:open", kind: "installer", appKeys: [] });
    }

    const storeFeaturedIds = ["google-chrome", "com.google.chrome", "chromium", "firefox", "heroic", "com.heroicgameslauncher.hgl", "lutris", "protonup-qt", "gravitymark", "discord", "com.obsproject.studio"];
    state.desktopApps.forEach((app) => {
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

    state.steamGames.slice(0, 6).forEach((g) => {
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

  document.body.classList.add("theme-light");

  function forceSoftwareRepaint() {
    if (!activeRootEl) return;
    document.documentElement.dataset.theme = state.activeThemeId;
    void document.documentElement.offsetHeight;
    void activeRootEl.offsetHeight;
  }

  function applyUiTheme(t) {
    if (!t) return;
    state.activeThemeId = t.id || "tahoe-light";
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
    if (t.ui_bg && mode !== "desktop" && mode !== "bar" && mode !== "cc") {
      document.body.style.background = t.ui_bg;
    }
    if (activeRootEl && t.ui_win_bg) {
      activeRootEl.style.background = "";
      if (t.ui_bg) activeRootEl.style.backgroundColor = t.ui_bg;
      activeRootEl.style.backgroundImage = t.ui_win_bg.includes("gradient") ? t.ui_win_bg : "";
      if (!t.ui_win_bg.includes("gradient")) activeRootEl.style.background = t.ui_win_bg;
      const toolbar = activeRootEl.querySelector(".win-toolbar");
      if (toolbar && t.ui_toolbar_bg) toolbar.style.background = t.ui_toolbar_bg;
    }
    document.body.classList.toggle("theme-light", Boolean(t.is_light));
    forceSoftwareRepaint();
  }

  window.__spinelThemeData = (data) => {
    if (!data) return;
    if (Array.isArray(data.themes)) state.availableThemes = data.themes;
    if (data.active) applyUiTheme(data.active);
    const grid = document.getElementById("theme-grid");
    const wpLabel = document.getElementById("wp-image-label");
    const wpClearBtn = document.getElementById("btn-wp-clear");
    if (wpLabel) {
      const p = String(data.wallpaper_image || "").trim();
      if (p) {
        const name = p.split("/").pop() || p;
        wpLabel.textContent = `当前图片: ${name} (${p})`;
      } else {
        wpLabel.textContent = "当前使用主题内置极光渐变";
      }
    }
    if (wpClearBtn) {
      wpClearBtn.disabled = !String(data.wallpaper_image || "").trim();
    }
    if (!grid || !Array.isArray(state.availableThemes)) return;
    grid.innerHTML = "";
    state.availableThemes.forEach((t) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "wp-card" + (t.id === state.activeThemeId ? " active" : "");
      btn.setAttribute("data-cmd", `theme:set:${t.id}`);

      const swatch = document.createElement("div");
      swatch.className = "wp-swatch";
      swatch.style.background = `radial-gradient(circle at 22% 22%, ${t.wp_glow1 || t.ui_accent}, transparent 58%), radial-gradient(circle at 78% 76%, ${t.wp_glow2 || t.ui_accent2}, transparent 60%), linear-gradient(135deg, ${t.wp_c0 || t.ui_bg}, ${t.wp_c1 || t.ui_bg})`;

      const meta = document.createElement("div");
      meta.className = "theme-card-meta";
      const title = document.createElement("strong");
      title.textContent = (t.name || t.id) + (t.id === state.activeThemeId ? " · 当前" : "");
      const sub = document.createElement("small");
      sub.textContent = t.subtitle || (t.builtin ? "内置风格" : "自定义扩展 (.theme)");
      meta.append(title, sub);

      btn.append(swatch, meta);
      grid.appendChild(btn);
    });
  };

  window.__spinelWallpaper = () => {};

  /* Custom in-DOM Select Dropdown */
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

  /* ---------------- 2. Dock, Launchpad & Window Sync ---------------- */
  function findMatchingWindow(matcher) {
    return state.trackedWindows.find((w) => {
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
      { title: "应用商店", cmd: "store:open", kind: "store", appKeys: [], match: ["spinel os 应用商店", "spinelos 应用商店"] },
      { title: "Steam", cmd: "launch:steam", kind: "steam", appKeys: ["steam"], match: ["steam"] },
      { title: "Gamescope 主机大屏", cmd: "launch:steam-gamepadui", kind: "gamepad", appKeys: [], match: ["gamescope"] },
      { title: "Spinel Files 文件管理", cmd: "launch:files", kind: "files", appKeys: ["org.spinelos.files", "thunar", "org.xfce.thunar"], match: ["spinel-files", "org.spinelos.files", "thunar"] },
      { title: "终端", cmd: "launch:terminal", kind: "terminal", appKeys: ["org.spinelos.terminal", "org.gnome.ptyxis", "ptyxis", "foot"], match: ["spinel-terminal", "org.spinelos.terminal", "spinel 终端", "ptyxis", "foot"] },
      { title: "系统设置", cmd: "settings:open:display", kind: "settings", appKeys: [], match: ["spinel os 系统设置", "spinelos 系统设置"] }
    ];
    if (state.isLiveEnv) {
      pinned.push({ title: "安装 Spinel OS 到硬盘", cmd: "installer:open", kind: "installer", appKeys: [], match: ["安装 spinel"] });
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

    const extraWins = state.trackedWindows.filter((w) => !matchedIds.has(w.id));
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

  const launchpadMenu = mode === "games" ? document.getElementById("launchpad-context-menu") : null;
  const launchpadOpenLocation = document.getElementById("launchpad-open-location");
  const launchpadRemoveShortcut = document.getElementById("launchpad-remove-shortcut");
  let launchpadMenuApp = null;
  let launchpadMenuAnchor = null;

  function closeLaunchpadMenu(restoreFocus = false) {
    if (!launchpadMenuApp) return;
    launchpadMenu.classList.add("hidden");
    if (restoreFocus) launchpadMenuAnchor.focus({ preventScroll: true });
    launchpadMenuApp = null;
    launchpadMenuAnchor = null;
  }

  function openLaunchpadMenu(ev, app) {
    ev.preventDefault();
    launchpadMenuAnchor = ev.currentTarget;
    launchpadMenuApp = app;
    launchpadRemoveShortcut.classList.toggle("hidden", !app.user_shortcut);
    launchpadMenu.classList.remove("hidden");
    const menu = launchpadMenu.getBoundingClientRect();
    const anchor = launchpadMenuAnchor.getBoundingClientRect();
    const x = ev.clientX || anchor.left;
    const y = ev.clientY || anchor.bottom;
    launchpadMenu.style.left = `${Math.max(8, Math.min(x, window.innerWidth - menu.width - 8))}px`;
    launchpadMenu.style.top = `${Math.max(8, Math.min(y, window.innerHeight - menu.height - 8))}px`;
    launchpadOpenLocation.focus({ preventScroll: true });
  }

  function sendLaunchpadAction(method) {
    if (!launchpadMenuApp) return;
    const id = launchpadMenuApp.id;
    closeLaunchpadMenu(true);
    showFeedback("launchpad-feedback", "");
    sendCommand({ method, params: { id } });
  }

  if (launchpadMenu) {
    launchpadOpenLocation.addEventListener("click", () => sendLaunchpadAction("app.open-location"));
    launchpadRemoveShortcut.addEventListener("click", () => sendLaunchpadAction("app.remove-shortcut"));
    launchpadMenu.addEventListener("keydown", (ev) => {
      if ((ev.key === "ArrowDown" || ev.key === "ArrowUp") && !launchpadRemoveShortcut.classList.contains("hidden")) {
        ev.preventDefault();
        (document.activeElement === launchpadOpenLocation ? launchpadRemoveShortcut : launchpadOpenLocation).focus();
      }
    });
    document.addEventListener("pointerdown", (ev) => {
      if (!launchpadMenu.contains(ev.target)) closeLaunchpadMenu();
    });
    document.addEventListener("keydown", (ev) => {
      if (ev.key === "Escape") closeLaunchpadMenu(true);
    });
    document.querySelector("#app-games .app-scroll")?.addEventListener("scroll", () => closeLaunchpadMenu());
    window.addEventListener("blur", () => closeLaunchpadMenu());
    window.__spinelLaunchpadFeedback = (ok, message) => showFeedback("launchpad-feedback", message, !ok);
  }

  function renderLaunchpadApps() {
    const grid = document.getElementById("launchpad-app-grid");
    if (!grid) return;
    const focused = launchpadMenuAnchor || document.activeElement;
    const focusIndex = Array.from(grid.children).indexOf(focused);
    const focusId = focusIndex >= 0 ? focused.dataset.appId : null;
    closeLaunchpadMenu();
    grid.innerHTML = "";
    state.desktopApps.forEach((app) => {
      const btn = document.createElement("button");
      btn.className = "launchpad-item";
      btn.dataset.appId = app.id;
      btn.setAttribute("data-cmd", `app:launch:${app.id}`);
      if (launchpadMenu) {
        btn.setAttribute("aria-haspopup", "menu");
        btn.addEventListener("contextmenu", (ev) => openLaunchpadMenu(ev, app));
        btn.addEventListener("keydown", (ev) => {
          if (ev.key === "ContextMenu" || (ev.shiftKey && ev.key === "F10")) openLaunchpadMenu(ev, app);
        });
      }
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
    if (focusIndex >= 0) {
      const replacement = Array.from(grid.children).find((button) => button.dataset.appId === focusId)
        || grid.children[Math.min(focusIndex, grid.children.length - 1)]
        || document.getElementById("launchpad-heading");
      replacement?.focus({ preventScroll: true });
    }
  }

  window.__spinelWindows = (list) => {
    state.trackedWindows = Array.isArray(list) ? list : [];
    const activeAppEl = document.getElementById("bar-active-app");
    if (activeAppEl) {
      const activeWin = state.trackedWindows.find((w) => w.activated) || state.trackedWindows[0];
      activeAppEl.textContent = activeWin ? (activeWin.title || activeWin.app_id || "应用") : "访达";
    }
    renderDock();
  };

  window.SpinelUI = {
    params,
    mode,
    initialTab,
    activeRootEl,
    state,
    hooks: {},
    sendCommand,
    invokeIpc,
    formatBytes,
    showFeedback,
    forceSoftwareRepaint,
    enhanceSelect,
    syncAllCustomSelects,
    makeSvgIcon,
    matchAppKind,
    findAppIconUri,
    fillIconContainer,
    applyUiTheme,
    renderDesktopShortcuts,
    renderDock,
    renderLaunchpadApps
  };

  window.__spinelDesktopApps = (apps) => {
    state.desktopApps = Array.isArray(apps) ? apps : [];
    renderDesktopShortcuts();
    renderDock();
    renderLaunchpadApps();
    if (mode === "store") window.SpinelUI.hooks.renderStoreApps?.();
  };
})();
