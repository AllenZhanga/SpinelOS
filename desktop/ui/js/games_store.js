(() => {
  const UI = window.SpinelUI;
  const { state, sendCommand, formatBytes, showFeedback, forceSoftwareRepaint, fillIconContainer, matchAppKind, renderDesktopShortcuts } = UI;

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
    const p = state.perfState;
    document.querySelectorAll("[data-perf-key]").forEach((btn) => {
      const k = btn.getAttribute("data-perf-key");
      const v = btn.getAttribute("data-perf-val");
      btn.classList.toggle("active", String(p[k]) === String(v));
    });
    const fsrBtn = document.getElementById("btn-toggle-fsr");
    if (fsrBtn) {
      fsrBtn.classList.toggle("is-on", Boolean(p.fsr));
      fsrBtn.setAttribute("aria-checked", String(Boolean(p.fsr)));
    }
    const gmBtn = document.getElementById("btn-toggle-gamemode");
    if (gmBtn) {
      gmBtn.classList.toggle("is-on", Boolean(p.gamemode));
      gmBtn.setAttribute("aria-checked", String(Boolean(p.gamemode)));
    }
    const ccGmCircle = document.getElementById("cc-gm-circle");
    const ccGmVal = document.getElementById("cc-gm-val");
    if (ccGmCircle) ccGmCircle.classList.toggle("active-blue", Boolean(p.gamemode));
    if (ccGmVal) ccGmVal.textContent = p.gamemode ? "已开启" : "已关闭";

    const ccMhCircle = document.getElementById("cc-mh-circle");
    const ccMhVal = document.getElementById("cc-mh-val");
    if (ccMhCircle) ccMhCircle.classList.toggle("active-indigo", p.mangohud !== "off");
    if (ccMhVal) {
      const mhMap = { off: "已关闭", compact: "精简监控", full: "详细监控" };
      ccMhVal.textContent = mhMap[p.mangohud] || p.mangohud;
    }

    const ccFsrOrb = document.getElementById("cc-btn-fsr");
    if (ccFsrOrb) ccFsrOrb.classList.toggle("active", Boolean(p.fsr));

    const preview = document.getElementById("perf-cmd-preview");
    if (preview) preview.textContent = buildLaunchOptionString(p);
    forceSoftwareRepaint();
  }

  window.__spinelPerfFeedback = (ok, msg) => showFeedback("perf-feedback", msg || "", !ok);

  function sendPerfUpdate(patch) {
    state.perfState = { ...state.perfState, ...patch };
    renderPerfState();
    sendCommand([
      "perf:set",
      `mangohud=${state.perfState.mangohud}`,
      `fps_limit=${state.perfState.fps_limit}`,
      `fsr=${state.perfState.fsr ? "true" : "false"}`,
      `gamemode=${state.perfState.gamemode ? "true" : "false"}`,
      `render_res=${state.perfState.render_res}`
    ].join("\n"));
  }

  window.__spinelPerfConfig = (cfg) => {
    state.perfState = { ...state.perfState, ...cfg };
    renderPerfState();
  };

  /* ---------------- 4. Steam Games & Storage ---------------- */
  function renderGameOverrideModal() {
    const ov = state.ovEditState;
    if (!ov) return;
    document.querySelectorAll("#game-ov-modal [data-ov-key]").forEach((btn) => {
      const k = btn.getAttribute("data-ov-key");
      const v = btn.getAttribute("data-ov-val");
      btn.classList.toggle("active", String(ov[k]) === String(v));
    });
    const fsrBtn = document.getElementById("ov-btn-fsr");
    if (fsrBtn) fsrBtn.classList.toggle("is-on", Boolean(ov.fsr));
    const gmBtn = document.getElementById("ov-btn-gamemode");
    if (gmBtn) gmBtn.classList.toggle("is-on", Boolean(ov.gamemode));
  }

  function openGameOverrideModal(game) {
    const modal = document.getElementById("game-ov-modal");
    if (!modal || !game) return;
    const base = game.has_override && game.override ? game.override : state.perfState;
    state.ovEditState = {
      appid: String(game.appid),
      mangohud: base.mangohud || "compact",
      fps_limit: Number(base.fps_limit || 0),
      render_res: base.render_res || "native",
      fsr: Boolean(base.fsr),
      gamemode: base.gamemode !== undefined ? Boolean(base.gamemode) : true
    };
    const titleEl = document.getElementById("ov-game-title");
    if (titleEl) titleEl.textContent = `${game.name} · 独立性能配置`;
    const subEl = document.getElementById("ov-game-sub");
    if (subEl) {
      subEl.textContent = game.has_override
        ? `AppID ${game.appid} 当前已启用独立参数覆盖`
        : `AppID ${game.appid} 当前继承全局预设，保存后将独立覆盖`;
    }
    renderGameOverrideModal();
    modal.classList.remove("hidden");
  }

  function renderSteamGames() {
    const grid = document.getElementById("steam-game-grid");
    const countBadge = document.getElementById("steam-count-badge");
    if (!grid) return;

    const query = (document.getElementById("game-search-input")?.value || "").trim().toLowerCase();
    const filtered = query
      ? state.steamGames.filter((g) => (g.name || "").toLowerCase().includes(query))
      : state.steamGames;

    if (countBadge) countBadge.textContent = `${state.steamGames.length} 款游戏`;
    grid.innerHTML = "";

    if (filtered.length === 0) {
      const empty = document.createElement("div");
      empty.className = "lib-empty";
      const msg = document.createElement("p");
      msg.textContent = state.steamGames.length === 0
        ? "暂未检测到本地已安装的 Steam 游戏。登录后下载任意游戏，点击“扫描游戏库”即可在这里一键启动。"
        : "没有匹配搜索关键字的游戏。";
      const actions = document.createElement("div");
      actions.className = "footer-actions";
      const b1 = document.createElement("button");
      b1.className = "btn primary small";
      b1.setAttribute("data-cmd", "launch:steam");
      b1.textContent = "启动 Steam";
      const bInstall = document.createElement("button");
      bInstall.className = "btn small";
      bInstall.setAttribute("data-cmd", "store:open:steam");
      bInstall.textContent = "安装 Steam";
      const b2 = document.createElement("button");
      b2.className = "btn glow small";
      b2.setAttribute("data-cmd", "launch:steam-gamepadui");
      b2.textContent = "进入主机大屏";
      actions.append(b1, bInstall, b2);
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
      if (g.has_override) {
        const badge = document.createElement("span");
        badge.className = "game-ov-tag";
        badge.textContent = "独立配置";
        title.appendChild(badge);
      }
      const sub = document.createElement("div");
      sub.className = "game-sub";
      sub.textContent = `AppID ${g.appid} · ${formatBytes(g.size_bytes)}`;
      const btn = document.createElement("button");
      btn.className = "btn primary small game-launch-btn";
      btn.textContent = "立即启动";
      btn.setAttribute("data-cmd", `steam:launch:${g.appid}`);

      const rowActions = document.createElement("div");
      rowActions.className = "game-card-actions";
      const ovBtn = document.createElement("button");
      ovBtn.type = "button";
      ovBtn.className = "btn small";
      ovBtn.textContent = g.has_override ? "调优*" : "参数";
      ovBtn.addEventListener("click", (ev) => {
        ev.stopPropagation();
        openGameOverrideModal(g);
      });
      const dirBtn = document.createElement("button");
      dirBtn.type = "button";
      dirBtn.className = "btn small";
      dirBtn.textContent = g.compatdata ? "存档" : "目录";
      dirBtn.setAttribute("data-cmd", `steam:compat:${g.appid}`);
      rowActions.append(ovBtn, dirBtn);

      meta.append(title, sub, btn, rowActions);
      card.append(poster, meta);
      grid.appendChild(card);
    });
  }

  document.querySelectorAll("#game-ov-modal [data-ov-key]").forEach((btn) => {
    btn.addEventListener("click", () => {
      if (!state.ovEditState) return;
      const k = btn.getAttribute("data-ov-key");
      const v = btn.getAttribute("data-ov-val");
      state.ovEditState[k] = k === "fps_limit" ? Number(v) : v;
      renderGameOverrideModal();
    });
  });

  window.__spinelStorageCaches = (data) => {
    if (!data) return;
    const fmt = (b) => {
      const n = Number(b || 0);
      if (n <= 0) return "0 MB";
      if (n >= 1073741824) return `${(n / 1073741824).toFixed(2)} GB`;
      if (n >= 1048576) return `${(n / 1048576).toFixed(1)} MB`;
      return `${Math.round(n / 1024)} KB`;
    };
    const set = (id, v) => { const el = document.getElementById(id); if (el) el.textContent = v; };
    set("cache-shader-sz", fmt(data.shadercache_bytes));
    set("cache-dl-sz", fmt(data.downloading_bytes));
    set("cache-logs-sz", fmt(data.logs_bytes));
    set("cache-pacman-sz", fmt(data.pacman_bytes));
    const cleanable = Number(data.shadercache_bytes || 0) + Number(data.downloading_bytes || 0) + Number(data.logs_bytes || 0);
    set("storage-total-badge", `可清理 ${fmt(cleanable)}`);
  };

  window.__spinelStorageFeedback = (ok, msg) => showFeedback("storage-feedback", msg || "", !ok);

  window.__spinelSteamGames = (games) => {
    state.steamGames = Array.isArray(games) ? games : [];
    renderSteamGames();
    renderDesktopShortcuts();
  };

  /* ---------------- 5. SpinelOS App Store ---------------- */
  let storeCategory = "all";

  function renderStoreApps() {
    const grid = document.getElementById("store-app-grid");
    if (!grid) return;
    const ss = state.storeState;
    const customBtn = document.getElementById("btn-store-custom");
    if (customBtn) customBtn.disabled = ss.busy;
    const cancelBtn = document.getElementById("btn-store-cancel");
    if (cancelBtn) {
      cancelBtn.classList.toggle("hidden", !ss.busy);
      if (!ss.busy) {
        cancelBtn.disabled = false;
        cancelBtn.textContent = "停止下载";
      }
    }
    document.querySelectorAll("[data-store-sys]").forEach((b) => { b.disabled = ss.busy; });

    const query = (document.getElementById("store-search-input")?.value || "").trim().toLowerCase();
    const list = (ss.apps || []).filter((app) => {
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
      const isActiveTarget = ss.busy && (ss.active_id === app.id || ss.active_id === app.pkg);

      if (app.installed) {
        const launchBtn = document.createElement("button");
        launchBtn.className = "btn primary small";
        launchBtn.textContent = "打开";
        launchBtn.setAttribute("data-cmd", `store:launch:${app.id}`);
        const rmBtn = document.createElement("button");
        rmBtn.className = "btn small danger";
        if (isActiveTarget && ss.active_action === "remove") {
          rmBtn.textContent = "停止卸载";
          rmBtn.disabled = false;
          rmBtn.addEventListener("click", (ev) => {
            ev.stopPropagation();
            rmBtn.disabled = true;
            rmBtn.textContent = "正在终止...";
            sendCommand("store:cancel");
          });
        } else {
          rmBtn.textContent = "卸载";
          rmBtn.disabled = ss.busy;
          if (ss.busy) rmBtn.title = "请等待当前操作完成";
          rmBtn.addEventListener("click", (ev) => {
            ev.stopPropagation();
            requestStoreOp("remove", { id: app.id, name: app.name, backend: app.backend, pkg: app.pkg });
          });
        }
        actions.append(launchBtn, rmBtn);
      } else {
        const instBtn = document.createElement("button");
        if (isActiveTarget) {
          instBtn.className = "btn small danger";
          instBtn.textContent = "停止下载";
          instBtn.disabled = false;
          instBtn.addEventListener("click", (ev) => {
            ev.stopPropagation();
            instBtn.disabled = true;
            instBtn.textContent = "正在终止...";
            sendCommand("store:cancel");
          });
        } else {
          instBtn.className = "btn primary small";
          instBtn.textContent = "下载安装";
          instBtn.disabled = ss.busy;
          if (ss.busy) instBtn.title = "请等待当前队列完成";
          instBtn.addEventListener("click", (ev) => {
            ev.stopPropagation();
            requestStoreOp("install", { id: app.id, name: app.name, backend: app.backend, pkg: app.pkg });
          });
        }
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
    const verbMap = { install: "正在下载安装", remove: "正在卸载", "sys-update": "正在执行", "mirror-test": "正在执行", "keyring-fix": "正在执行" };
    if (logTitle) logTitle.textContent = `${verbMap[action] || "正在执行"}: ${op.name || op.pkg}`;
    state.storeState.busy = true;
    state.storeState.active_id = op.id || op.pkg || "";
    state.storeState.active_action = action;
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
    if (state.storeState.busy) return;
    if ((op.backend === "pacman" || op.backend === "native") && !state.storeState.sudo_nopass) {
      state.pendingStoreOp = { action, op };
      const box = document.getElementById("store-pass-box");
      const title = document.getElementById("store-pass-title");
      const confirmBtn = document.getElementById("btn-store-pass-confirm");
      const input = document.getElementById("store-pass-input");
      if (title) title.textContent = `执行 ${op.name || op.pkg} 需要管理员授权`;
      if (confirmBtn) confirmBtn.textContent = "确认并继续";
      if (input) input.value = "";
      box?.classList.remove("hidden");
      input?.focus();
      return;
    }
    executeStoreOp(action, op, "");
  }

  document.querySelectorAll("[data-store-sys]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const action = btn.getAttribute("data-store-sys");
      const names = { "sys-update": "一键更新系统", "mirror-test": "测速优选国内镜像", "keyring-fix": "修复签名密钥环" };
      requestStoreOp(action, { id: action, name: names[action] || action, backend: "pacman", pkg: action });
    });
  });

  document.getElementById("store-pass-box")?.addEventListener("click", (ev) => {
    if (ev.target.id === "store-pass-box") {
      state.pendingStoreOp = null;
      ev.currentTarget.classList.add("hidden");
    }
  });
  document.addEventListener("keydown", (ev) => {
    if (ev.key === "Escape") {
      const box = document.getElementById("store-pass-box");
      if (box && !box.classList.contains("hidden")) {
        state.pendingStoreOp = null;
        box.classList.add("hidden");
      }
    }
  });

  window.__spinelFocusStoreApp = (appId) => {
    state.pendingStoreFocus = String(appId || "").trim();
  };

  window.__spinelStoreData = (data) => {
    state.storeState = { ...state.storeState, ...data };
    if (!state.storeState.busy) {
      state.storeState.active_id = "";
      state.storeState.active_action = "";
    }
    renderStoreApps();
    if (state.pendingStoreFocus && !state.storeState.busy) {
      const targetId = state.pendingStoreFocus;
      state.pendingStoreFocus = "";
      const targetApp = (state.storeState.apps || []).find((a) => a.id === targetId);
      if (targetApp && !targetApp.installed) {
        requestStoreOp("install", {
          id: targetApp.id,
          name: targetApp.name,
          backend: targetApp.backend,
          pkg: targetApp.pkg
        });
      }
    }
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
    state.storeState.busy = false;
    state.storeState.active_id = "";
    state.storeState.active_action = "";
    const logTitle = document.getElementById("store-log-title");
    if (logTitle) {
      if (action === "cancel") {
        logTitle.textContent = `已终止下载: ${targetId}`;
      } else {
        logTitle.textContent = ok
          ? `${action === "install" ? "安装完成" : "卸载完成"}: ${targetId}`
          : `操作未完成: ${targetId}（请查看下方输出）`;
      }
    }
    renderStoreApps();
  };

  document.getElementById("game-search-input")?.addEventListener("input", renderSteamGames);
  document.getElementById("store-search-input")?.addEventListener("input", renderStoreApps);
  document.getElementById("store-pass-input")?.addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") {
      ev.preventDefault();
      document.querySelector('[data-cmd="store:pass-confirm"]')?.click();
    }
  });

  UI.hooks.renderPerfState = renderPerfState;
  UI.hooks.sendPerfUpdate = sendPerfUpdate;
  UI.hooks.renderGameOverrideModal = renderGameOverrideModal;
  UI.hooks.renderSteamGames = renderSteamGames;
  UI.hooks.renderStoreApps = renderStoreApps;
  UI.hooks.setStoreCategory = (cat) => { storeCategory = cat || "all"; };
  UI.hooks.requestStoreOp = requestStoreOp;
  UI.hooks.executeStoreOp = executeStoreOp;
})();
