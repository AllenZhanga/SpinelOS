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

  document.addEventListener("click", (ev) => {
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
          const radio = item.querySelector("input[type='radio']");
          if (radio) radio.checked = true;
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

  updateClock();
  setInterval(updateClock, 15000);
  sendCommand("telemetry:refresh");
})();
