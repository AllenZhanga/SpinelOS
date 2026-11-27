(() => {
  const UI = window.SpinelUI;
  const { state, sendCommand, showFeedback } = UI;
  const { formatTempBytes, filterAndSortProcs } = UI.helpers;

  /* ---------------- Process Monitor & Memory Cleanup ---------------- */
  const memoryCleanupButton = document.getElementById("btn-clear-memory");
  let lastProcPayload = null;
  let procSortBy = "cpu";
  let memoryCleanupRequested = false;
  let memoryCleanupStartTimer = 0;

  function renderProcessList() {
    const listEl = document.getElementById("procs-list");
    if (!listEl || !lastProcPayload) return;

    const cpuChip = document.getElementById("procs-cpu-chip");
    const memChip = document.getElementById("procs-mem-chip");
    const tmpfsChip = document.getElementById("procs-tmpfs-chip");
    const cntChip = document.getElementById("procs-cnt-chip");
    if (cpuChip) cpuChip.textContent = `CPU: ${lastProcPayload.cpu_pct ?? "--"}%`;
    if (memChip) memChip.textContent = `内存: ${lastProcPayload.mem || "--"}`;
    const tmpfsBytes = lastProcPayload.tmpfs_bytes;
    const hasTmpfs = Number.isFinite(tmpfsBytes);
    if (tmpfsChip) {
      tmpfsChip.classList.toggle("hidden", !hasTmpfs);
      if (hasTmpfs) tmpfsChip.textContent = `/tmp：${formatTempBytes(tmpfsBytes)}`;
    }
    if (memoryCleanupButton) {
      memoryCleanupButton.classList.toggle("hidden", typeof lastProcPayload.memory_cleanup_running !== "boolean");
      memoryCleanupButton.disabled = memoryCleanupRequested || Boolean(lastProcPayload.memory_cleanup_running);
    }
    if (cntChip) cntChip.textContent = `进程: ${lastProcPayload.total_procs ?? "--"}`;

    const query = document.getElementById("procs-search-input")?.value || "";
    const procs = filterAndSortProcs(lastProcPayload.procs, query, procSortBy);

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

  memoryCleanupButton?.addEventListener("click", () => {
    memoryCleanupRequested = true;
    memoryCleanupButton.disabled = true;
    showFeedback("procs-feedback", "正在清理内存…");
    memoryCleanupStartTimer = setTimeout(() => {
      memoryCleanupRequested = false;
      memoryCleanupButton.disabled = Boolean(lastProcPayload?.memory_cleanup_running);
      if (!memoryCleanupButton.disabled) showFeedback("procs-feedback", "未收到清理结果，请重试。", true);
    }, 6000);
    sendCommand("monitor:memory-clean");
  });

  window.__spinelMemoryCleanup = (directories, bytes, incomplete, cacheCleared, heapReleased) => {
    clearTimeout(memoryCleanupStartTimer);
    memoryCleanupRequested = false;
    if (lastProcPayload) lastProcPayload.memory_cleanup_running = false;
    if (memoryCleanupButton) memoryCleanupButton.disabled = false;
    const messages = [];
    messages.push(cacheCleared ? "已清理 SpinelOS 界面缓存。" : "界面缓存清理失败。");
    if (heapReleased) messages.push("已归还监视器进程的空闲内存。");
    if (directories > 0) messages.push(`已清理 ${directories} 个临时目录。`);
    if (bytes > 0 && (directories > 0 || incomplete)) messages.push(`/tmp 占用减少 ${formatTempBytes(bytes)}。`);
    if (incomplete) messages.push("部分临时文件未能清理。");
    else if (directories === 0 && bytes !== null) messages.push("当前没有可清理的 SpinelOS 临时目录。");
    showFeedback("procs-feedback", messages.join(""), incomplete || !cacheCleared);
    sendCommand("monitor:procs-get");
  };

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

  /* ---------------- System Logs & Spinel Doctor ---------------- */
  window.__spinelLogsData = (data) => {
    if (!data) return;
    if (data.source) state.activeLogSource = data.source;
    document.querySelectorAll("#logs-source-tabs .pill-btn").forEach((b) => {
      const cmd = b.getAttribute("data-cmd") || "";
      b.classList.toggle("active", cmd === `monitor:logs:${state.activeLogSource}` || cmd === `settings:logs:${state.activeLogSource}`);
    });
    const clrBtn = document.getElementById("btn-logs-clear");
    if (clrBtn) {
      clrBtn.textContent = state.activeLogSource === "mcp" ? "清空审计日志" : "清空应用日志";
    }
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
    sendCommand(`monitor:logs:${state.activeLogSource}`);
  });
  document.getElementById("btn-logs-clear")?.addEventListener("click", () => {
    sendCommand(`monitor:logs-clear:${state.activeLogSource}`);
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
})();
