(() => {
  const UI = window.SpinelUI;
  const { state, sendCommand, formatBytes, showFeedback, forceSoftwareRepaint, fillIconContainer, findAppIconUri, matchAppKind, renderDesktopShortcuts } = UI;

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

  /* ---------------- 5. SpinelOS App Center & Settings Updates ---------------- */
  let storeCategory = "all";
  let storeSubview = UI.params?.get("tab") === "installed" ? "installed" : "market";

  function switchStoreSubview(nextView) {
    storeSubview = nextView === "installed" ? "installed" : "market";
    document.getElementById("store-view-market")?.classList.toggle("hidden", storeSubview !== "market");
    document.getElementById("store-view-installed")?.classList.toggle("hidden", storeSubview !== "installed");
    document.querySelectorAll("#store-view-tabs [data-store-view]").forEach((btn) => {
      btn.classList.toggle("active", btn.getAttribute("data-store-view") === storeSubview);
    });
    if (storeSubview === "installed") {
      UI.renderLaunchpadApps?.();
    }
  }
  window.__spinelOpenStoreView = switchStoreSubview;
  window.__spinelStoreFeedback = (ok, msg) => showFeedback("store-feedback", msg || "", !ok);
  if (UI.mode === "store") switchStoreSubview(storeSubview);

  function getActiveLogDom() {
    const useUpd = UI.mode === "settings";
    const pfx = useUpd ? "upd" : "store";
    return {
      wrap: document.getElementById(`${pfx}-log-wrap`),
      box: document.getElementById(`${pfx}-log-box`),
      title: document.getElementById(`${pfx}-log-title`),
      progWrap: document.getElementById(`${pfx}-progress-wrap`),
      progBar: document.getElementById(`${pfx}-progress-bar`),
      progText: document.getElementById(`${pfx}-progress-text`),
      progPct: document.getElementById(`${pfx}-progress-pct`),
      cancelBtn: document.getElementById(useUpd ? "btn-upd-cancel" : "btn-store-cancel")
    };
  }

  function renderStoreApps() {
    const ss = state.storeState;
    const customBtn = document.getElementById("btn-store-custom");
    if (customBtn) customBtn.disabled = ss.busy;
    ["btn-store-cancel", "btn-upd-cancel"].forEach((cid) => {
      const cancelBtn = document.getElementById(cid);
      if (cancelBtn) {
        cancelBtn.classList.toggle("hidden", !ss.busy);
        if (!ss.busy) {
          cancelBtn.disabled = false;
          cancelBtn.textContent = cid === "btn-upd-cancel" ? "停止任务" : "停止下载";
        }
      }
    });
    document.querySelectorAll("[data-store-sys]").forEach((b) => {
      if (b.dataset.otaLocked === "1") {
        b.disabled = true;
      } else {
        b.disabled = ss.busy;
      }
    });
    const offlineBtn = document.getElementById("btn-spinel-ota-offline");
    if (offlineBtn) offlineBtn.disabled = ss.busy;

    const drvText = ss.driver_summary
      ? `显卡: ${ss.driver_summary}${ss.kernel_summary ? ` · 内核 ${ss.kernel_summary}` : ""}`
      : "";
    ["store-driver-chip", "upd-driver-chip"].forEach((id) => {
      const el = document.getElementById(id);
      if (el && drvText) el.textContent = drvText;
    });

    const totalUpd = Number(ss.updates_total || 0);
    const gpuUpds = Array.isArray(ss.gpu_updates) ? ss.gpu_updates : [];
    const sysUpdBtn = document.getElementById("btn-sys-update");
    ["store-update-chip", "upd-update-chip"].forEach((id) => {
      const updChip = document.getElementById(id);
      if (!updChip) return;
      if (totalUpd > 0) {
        updChip.textContent = gpuUpds.length > 0
          ? `可更新 ${totalUpd} 项（含显卡/内核 ${gpuUpds.length} 项）`
          : `可更新 ${totalUpd} 项`;
        updChip.style.background = "rgba(245, 158, 11, 0.18)";
        updChip.style.color = "#f59e0b";
      } else {
        updChip.textContent = "系统与驱动已是最新";
        updChip.style.background = "";
        updChip.style.color = "";
      }
    });
    if (sysUpdBtn) {
      sysUpdBtn.textContent = totalUpd > 0 ? `一键更新软件包 (${totalUpd})` : "一键更新软件包";
    }
    const rebootBtn = document.getElementById("btn-upd-reboot");
    if (rebootBtn && ss.reboot_required) rebootBtn.classList.remove("hidden");

    const grid = document.getElementById("store-app-grid");
    if (!grid) return;

    const query = (document.getElementById("store-search-input")?.value || "").trim().toLowerCase();
    const list = (ss.apps || []).filter((app) => {
      const catMatch = storeCategory === "all" || app.category === storeCategory;
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
      const iconKeys = app.id === "wine" ? ["org.spinelos.WindowsApps"] : [app.id, app.pkg, app.name];
      fillIconContainer(iconWrap, findAppIconUri(iconKeys), matchAppKind(`${app.id} ${app.pkg} ${app.name}`), app.name);

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
      const metaRow = document.createElement("div");
      metaRow.className = "store-card-meta-row";
      const pkgLabel = document.createElement("span");
      pkgLabel.className = "store-card-pkg";
      pkgLabel.title = app.pkg;
      pkgLabel.textContent = app.backend === "link" ? app.pkg.replace(/^https:\/\//, "") : app.pkg;
      metaRow.appendChild(pkgLabel);

      if (app.service_unit && app.installed) {
        const svcPill = document.createElement("span");
        svcPill.className = `store-svc-pill${app.service_active ? " running" : ""}`;
        svcPill.textContent = app.service_active ? "运行中" : "已停止";
        metaRow.appendChild(svcPill);
      }

      const actions = document.createElement("div");
      actions.className = "footer-actions";
      const isActiveTarget = ss.busy && (ss.active_id === app.id || ss.active_id === app.pkg);

      if (app.backend === "link") {
        const linkBtn = document.createElement("button");
        linkBtn.className = "btn primary small";
        linkBtn.textContent = "打开官网";
        linkBtn.setAttribute("data-cmd", `store:launch:${app.id}`);
        actions.appendChild(linkBtn);
        if (app.webapp_capable) {
          const pinBtn = document.createElement("button");
          pinBtn.className = "btn small";
          pinBtn.textContent = app.webapp_pinned ? "移除快捷方式" : "固定桌面应用";
          pinBtn.disabled = ss.busy;
          pinBtn.addEventListener("click", (ev) => {
            ev.stopPropagation();
            requestStoreOp(app.webapp_pinned ? "webapp-unpin" : "webapp-pin", {
              id: app.id,
              name: app.name,
              backend: app.backend,
              pkg: app.pkg
            });
          });
          actions.appendChild(pinBtn);
        }
      } else if (app.installed) {
        const launchBtn = document.createElement("button");
        launchBtn.className = "btn primary small";
        launchBtn.textContent = "打开";
        launchBtn.setAttribute("data-cmd", `store:launch:${app.id}`);
        actions.appendChild(launchBtn);

        if (app.service_unit) {
          const svcBtn = document.createElement("button");
          svcBtn.className = app.service_active ? "btn small" : "btn accent small";
          svcBtn.textContent = app.service_active ? "关闭服务" : "开启服务";
          svcBtn.disabled = ss.busy;
          svcBtn.addEventListener("click", (ev) => {
            ev.stopPropagation();
            requestStoreOp(app.service_active ? "svc-stop" : "svc-start", {
              id: app.id,
              name: app.name,
              backend: app.backend,
              pkg: app.service_unit
            });
          });
          actions.appendChild(svcBtn);
        }

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
        actions.appendChild(rmBtn);
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

      foot.append(metaRow, actions);
      card.append(head, foot);
      grid.appendChild(card);
    });
  }

  function setStoreProgress(visible, pct = 0, label = "") {
    const dom = getActiveLogDom();
    const wrap = dom.progWrap;
    const bar = dom.progBar;
    const pctEl = dom.progPct;
    const textEl = dom.progText;
    if (!wrap) return;
    wrap.classList.toggle("hidden", !visible);
    if (!visible) return;
    const clamped = Math.max(0, Math.min(100, Math.round(Number(pct) || 0)));
    if (bar) bar.style.width = `${clamped}%`;
    if (pctEl) pctEl.textContent = `${clamped}%`;
    if (textEl && label) textEl.textContent = label;
  }

  function executeStoreOp(action, op, password = "") {
    const dom = getActiveLogDom();
    const logWrap = dom.wrap;
    const logBox = dom.box;
    const logTitle = dom.title;
    if (logWrap) logWrap.classList.remove("hidden");
    if (logBox) logBox.textContent = "";
    const verbMap = {
      install: "正在下载安装",
      remove: "正在卸载",
      "svc-start": "正在启动服务",
      "svc-stop": "正在停止服务",
      "webapp-pin": "正在固定快捷方式",
      "webapp-unpin": "正在移除快捷方式",
      "sys-update": "正在执行",
      "spinel-ota-full": "正在在线升级",
      "spinel-ota": "正在在线升级",
      "spinel-ota-offline": "正在离线升级",
      "mirror-test": "正在执行",
      "keyring-fix": "正在执行"
    };
    if (logTitle) logTitle.textContent = `${verbMap[action] || "正在执行"}: ${op.name || op.pkg}`;
    setStoreProgress(action === "install" || action === "sys-update" || action.startsWith("spinel-ota"), 2, `${verbMap[action] || "正在执行"} ${op.name || op.pkg}...`);
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
    const isUserOnlyOp =
      action === "webapp-pin" ||
      action === "webapp-unpin" ||
      ((action === "svc-start" || action === "svc-stop") && op.id === "vllm");
    const needsSudo = (op.backend === "pacman" || op.backend === "native") && !isUserOnlyOp;
    if (needsSudo && !state.storeState.sudo_nopass) {
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
      const names = {
        "sys-update": "一键更新软件包",
        "spinel-ota-full": "在线完整升级 Spinel OS (--full)",
        "spinel-ota": "在线升级 Spinel OS",
        "spinel-ota-offline": "离线包升级 Spinel OS (--full)",
        "mirror-test": "测速优选国内镜像",
        "keyring-fix": "修复签名密钥环"
      };
      requestStoreOp(action, { id: action, name: names[action] || action, backend: "pacman", pkg: action });
    });
  });

  window.__spinelStartOfflineOta = (bundlePath) => {
    const p = String(bundlePath || "").trim();
    if (!p) return;
    requestStoreOp("spinel-ota-offline", {
      id: "spinel-ota-offline",
      name: `离线更新包 (${p.split("/").pop() || p})`,
      backend: "pacman",
      pkg: p
    });
  };

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
      if (targetApp) {
        if (storeCategory !== "all" && storeCategory !== targetApp.category) {
          storeCategory = targetApp.category;
          document.querySelectorAll("[data-store-cat]").forEach((btn) => {
            btn.classList.toggle("active", btn.dataset.storeCat === storeCategory);
          });
          const search = document.getElementById("store-search-input");
          if (search && search.value) search.value = "";
          renderStoreApps();
        }
        if (!targetApp.installed) {
          requestStoreOp("install", {
            id: targetApp.id,
            name: targetApp.name,
            backend: targetApp.backend,
            pkg: targetApp.pkg
          });
        }
      }
    }
  };

  window.__spinelStoreProgress = (pct, label) => {
    setStoreProgress(true, pct, label);
  };

  window.__spinelStoreLog = (line) => {
    const dom = getActiveLogDom();
    if (dom.wrap) dom.wrap.classList.remove("hidden");
    if (dom.box) {
      dom.box.textContent += `${line}\n`;
      dom.box.scrollTop = dom.box.scrollHeight;
    }
  };

  window.__spinelStoreDone = (ok, targetId, action) => {
    state.storeState.busy = false;
    state.storeState.active_id = "";
    state.storeState.active_action = "";
    const dom = getActiveLogDom();
    const logTitle = dom.title;
    if (ok && action === "install") {
      setStoreProgress(true, 100, `安装完成: ${targetId}`);
    } else if (action === "cancel") {
      setStoreProgress(false);
    } else if (!ok) {
      setStoreProgress(false);
    } else {
      setStoreProgress(true, 100, `操作完成: ${targetId}`);
    }
    if (ok && (action === "spinel-ota-full" || action === "spinel-ota" || action === "spinel-ota-offline")) {
      document.getElementById("btn-upd-reboot")?.classList.remove("hidden");
      sendCommand("ota:check");
    }
    if (logTitle) {
      if (action === "cancel") {
        logTitle.textContent = `已终止任务: ${targetId}`;
      } else {
        const doneVerb = action === "install" ? "安装完成" : (action === "remove" ? "卸载完成" : "操作完成");
        logTitle.textContent = ok
          ? `${doneVerb}: ${targetId}`
          : `操作未完成: ${targetId}（请查看下方输出）`;
      }
    }
    renderStoreApps();
  };

  document.getElementById("game-search-input")?.addEventListener("input", renderSteamGames);
  document.getElementById("store-search-input")?.addEventListener("input", renderStoreApps);
  document.getElementById("launchpad-search-input")?.addEventListener("input", () => UI.renderLaunchpadApps?.());
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
  UI.hooks.switchStoreSubview = switchStoreSubview;
  UI.hooks.requestStoreOp = requestStoreOp;
  UI.hooks.executeStoreOp = executeStoreOp;
})();
