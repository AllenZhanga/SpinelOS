(() => {
  const UI = window.SpinelUI;
  const { state, sendCommand, showFeedback, enhanceSelect, syncSelectValue, bindModalDismiss } = UI;
  const {
    formatProxySummary,
    buildProxySavePayload,
    formatIfaceSummary,
    buildNetIpPayload,
    suggestMntFolderName,
    validateMntFolderName,
    buildKvCommand
  } = UI.helpers;

  /* ---------------- System Proxy ---------------- */
  let detectedGateway = "";

  window.__spinelProxyConfig = (data) => {
    if (!data) return;
    detectedGateway = data.gateway || "";
    const summary = formatProxySummary(data);
    const chip = document.getElementById("proxy-status-chip");
    if (chip) {
      chip.textContent = summary.statusText;
      chip.classList.toggle("ok", summary.active);
    }
    const hint = document.getElementById("proxy-gw-hint");
    if (hint) hint.textContent = summary.gwHintText;

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
    const res = buildProxySavePayload(
      document.getElementById("proxy-http-input")?.value,
      document.getElementById("proxy-socks-input")?.value,
      document.getElementById("proxy-noproxy-input")?.value
    );
    if (!res.ok) {
      showFeedback("proxy-feedback", res.error, true);
      return;
    }
    sendCommand(res.payload);
  });

  document.getElementById("btn-proxy-clear")?.addEventListener("click", () => {
    sendCommand("settings:proxy-clear");
  });

  /* ---------------- Wi-Fi & Static IP Config ---------------- */
  let activeNetCfgIface = null;

  function syncNetMethodInputs() {
    const method = document.getElementById("net-ip-method")?.value || "auto";
    const isManual = method === "manual";
    ["net-ip-addr", "net-ip-mask", "net-ip-gw"].forEach((id) => {
      const el = document.getElementById(id);
      if (el) el.disabled = !isManual;
    });
  }

  function openNetIpConfig(ifx) {
    activeNetCfgIface = ifx;
    const box = document.getElementById("net-ip-config-box");
    const titleEl = document.getElementById("net-cfg-title");
    const methodEl = document.getElementById("net-ip-method");
    const addrEl = document.getElementById("net-ip-addr");
    const maskEl = document.getElementById("net-ip-mask");
    const gwEl = document.getElementById("net-ip-gw");
    const dnsEl = document.getElementById("net-ip-dns");

    if (titleEl) titleEl.textContent = `配置网络接口: ${ifx.device}${ifx.connection ? ` (${ifx.connection})` : ""}`;
    if (addrEl) addrEl.value = ifx.ip || "";
    if (maskEl) maskEl.value = ifx.prefix || "24";
    if (gwEl) gwEl.value = ifx.gateway || "";
    if (dnsEl) dnsEl.value = ifx.dns || "";
    syncSelectValue(methodEl, ifx.method === "manual" ? "manual" : "auto");
    syncNetMethodInputs();
    box?.classList.remove("hidden");
    showFeedback("wifi-feedback", "");
  }

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
          const t = document.createElement("div");
          t.className = "disk-title";
          t.textContent = `${ifx.device} (${ifx.type})`;
          const sub = document.createElement("div");
          sub.className = "disk-sub";
          sub.textContent = formatIfaceSummary(ifx);
          left.append(t, sub);

          const right = document.createElement("div");
          right.className = "disk-tags";
          const tag = document.createElement("span");
          tag.className = "tag";
          tag.textContent = ifx.ip ? (ifx.method === "manual" ? "静态" : "DHCP") : ifx.state;
          right.appendChild(tag);

          const cfgBtn = document.createElement("button");
          cfgBtn.type = "button";
          cfgBtn.className = "btn small primary";
          cfgBtn.textContent = "配置 IP / DNS";
          cfgBtn.addEventListener("click", (ev) => {
            ev.stopPropagation();
            openNetIpConfig(ifx);
          });
          right.appendChild(cfgBtn);

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
      listEl.innerHTML = `<div class="disk-empty">暂无扫描到的 Wi-Fi 热点</div>`;
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
          state.selectedWifiSsid = n.ssid;
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

  document.getElementById("net-ip-method")?.addEventListener("change", syncNetMethodInputs);
  document.getElementById("btn-net-ip-cancel")?.addEventListener("click", () => {
    activeNetCfgIface = null;
    document.getElementById("net-ip-config-box")?.classList.add("hidden");
  });
  document.querySelectorAll("[data-dns-preset]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const el = document.getElementById("net-ip-dns");
      if (el) el.value = btn.getAttribute("data-dns-preset") || "";
    });
  });
  document.getElementById("btn-net-ip-save")?.addEventListener("click", () => {
    if (!activeNetCfgIface) return;
    const payload = buildNetIpPayload(activeNetCfgIface, {
      method: document.getElementById("net-ip-method")?.value || "auto",
      ip: document.getElementById("net-ip-addr")?.value,
      mask: document.getElementById("net-ip-mask")?.value,
      gateway: document.getElementById("net-ip-gw")?.value,
      dns: document.getElementById("net-ip-dns")?.value
    });
    if (payload) sendCommand(payload);
  });

  window.__spinelWifiAction = (ok, msg) => {
    showFeedback("wifi-feedback", msg || (ok ? "操作成功" : "操作失败"), !ok);
    if (ok) {
      document.getElementById("wifi-connect-box")?.classList.add("hidden");
      document.getElementById("net-ip-config-box")?.classList.add("hidden");
    }
  };

  /* ---------------- Bluetooth ---------------- */
  window.__spinelBtState = (data) => {
    state.btPowered = Boolean(data.powered);
    const badge = document.getElementById("bt-power-badge");
    const powerBtn = document.getElementById("btn-bt-power");
    const scanBtn = document.getElementById("btn-bt-scan");
    if (badge) {
      badge.textContent = state.btPowered ? "已开启" : "已关闭";
      badge.classList.toggle("ok", state.btPowered);
    }
    if (powerBtn) powerBtn.textContent = state.btPowered ? "关闭蓝牙" : "开启蓝牙";
    if (scanBtn) { scanBtn.disabled = false; scanBtn.textContent = "扫描手柄 (6s)"; }

    const listEl = document.getElementById("bt-device-list");
    if (!listEl) return;
    listEl.innerHTML = "";
    const devs = Array.isArray(data.devices) ? data.devices : [];
    if (devs.length === 0) {
      listEl.innerHTML = `<div class="disk-empty">${state.btPowered ? "暂无发现的蓝牙设备，请将手柄置于配对模式后扫描。" : "蓝牙已关闭，请先开启蓝牙。"}</div>`;
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
        const disc = document.createElement("button");
        disc.className = "btn small";
        disc.textContent = "断开";
        disc.setAttribute("data-cmd", `settings:bt-disconnect:${dev.mac}`);
        tags.append(tag, disc);
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

  window.__spinelBtAction = (ok, msg) => showFeedback("bt-feedback", msg || (ok ? "蓝牙操作完成" : "蓝牙操作失败"), !ok);

  /* ---------------- Display Outputs ---------------- */
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
        sendCommand(buildKvCommand("settings:display-apply", {
          output: out.name,
          mode: modeSel.value,
          scale: scaleSel.value
        }));
      });
      controls.append(modeSel, scaleSel, applyBtn);
      item.append(left, controls);
      listEl.appendChild(item);
      enhanceSelect(modeSel);
      enhanceSelect(scaleSel);
    });
  };

  window.__spinelDisplayAction = (ok, msg) => showFeedback("display-feedback", msg || (ok ? "已应用显示设置" : "应用失败"), !ok);

  /* ---------------- Device Manager ---------------- */
  let pendingFstabOp = null;
  let devicesSudoNopass = false;

  function closeFstabModal() {
    pendingFstabOp = null;
    document.getElementById("dev-fstab-modal")?.classList.add("hidden");
  }

  function openFstabModal(mode, d) {
    pendingFstabOp = { mode, dev: d.path, label: d.label || d.name };
    const modal = document.getElementById("dev-fstab-modal");
    const title = document.getElementById("dev-fstab-title");
    const desc = document.getElementById("dev-fstab-desc");
    const nameWrap = document.getElementById("dev-fstab-name-wrap");
    const modeWrap = document.getElementById("dev-fstab-mode-wrap");
    const passWrap = document.getElementById("dev-fstab-pass-wrap");
    const nameInput = document.getElementById("dev-fstab-name-input");
    const modeSelect = document.getElementById("dev-fstab-mode-select");
    const passInput = document.getElementById("dev-fstab-pass-input");
    const confirmBtn = document.getElementById("btn-dev-fstab-confirm");

    const isRemove = mode === "remove";
    if (title) title.textContent = isRemove ? `取消开机挂载: ${d.label || d.name}` : `固定挂载到 /mnt: ${d.label || d.name}`;
    if (desc) {
      desc.textContent = isRemove
        ? `将从 /etc/fstab 中移除 ${d.path} (${d.fstab_mount}) 的开机自动挂载配置，不会删除磁盘数据。`
        : `将 ${d.path} 写入 /etc/fstab，实现每次开机自动挂载到 /mnt 目录（自动附加 nofail 保护，防止拔盘卡死）。`;
    }
    nameWrap?.classList.toggle("hidden", isRemove);
    modeWrap?.classList.toggle("hidden", isRemove);
    passWrap?.classList.toggle("hidden", devicesSudoNopass);
    if (confirmBtn) confirmBtn.textContent = isRemove ? "确认移除" : "确认应用";
    if (nameInput && !isRemove) nameInput.value = suggestMntFolderName(d);
    if (modeSelect && !isRemove) syncSelectValue(modeSelect, d.fstype === "ntfs" ? "ro" : "rw");
    if (passInput) passInput.value = "";
    modal?.classList.remove("hidden");
    if (!isRemove) nameInput?.focus();
    else passInput?.focus();
  }

  function submitFstabModal() {
    if (!pendingFstabOp) return;
    const pass = document.getElementById("dev-fstab-pass-input")?.value || "";
    if (!devicesSudoNopass && !pass) {
      document.getElementById("dev-fstab-pass-input")?.focus();
      return;
    }
    if (pendingFstabOp.mode === "remove") {
      sendCommand(buildKvCommand("devices:fstab-remove", { dev: pendingFstabOp.dev, password: pass }));
    } else {
      const check = validateMntFolderName(document.getElementById("dev-fstab-name-input")?.value);
      if (!check.valid) {
        showFeedback("devices-feedback", check.error, true);
        return;
      }
      const modeVal = document.getElementById("dev-fstab-mode-select")?.value || "ro";
      sendCommand(buildKvCommand("devices:fstab-set", {
        dev: pendingFstabOp.dev,
        name: check.name,
        ro: modeVal === "ro" ? "1" : "0",
        password: pass
      }));
    }
    closeFstabModal();
  }

  bindModalDismiss("dev-fstab-modal", "btn-dev-fstab-cancel", closeFstabModal);
  document.getElementById("btn-dev-fstab-confirm")?.addEventListener("click", submitFstabModal);
  document.getElementById("dev-fstab-pass-input")?.addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") { ev.preventDefault(); submitFstabModal(); }
  });

  window.__spinelDevicesAction = (ok, msg) => showFeedback("devices-feedback", msg || (ok ? "外设操作完成" : "外设操作失败"), !ok);

  window.__spinelDevicesData = (data) => {
    if (!data) return;
    devicesSudoNopass = Boolean(data.sudo_nopass);
    const disks = Array.isArray(data.disks) ? data.disks : [];
    const usb = Array.isArray(data.usb) ? data.usb : [];
    const inputs = Array.isArray(data.inputs) ? data.inputs : [];
    const audioDevs = Array.isArray(data.audio_devs) ? data.audio_devs : [];

    const summaryEl = document.getElementById("dev-summary-chip");
    if (summaryEl) {
      summaryEl.textContent = `存储 ${disks.length} · USB 外设 ${usb.length} · 音频 ${audioDevs.length} · 输入 ${inputs.length}`;
    }

    const diskList = document.getElementById("dev-disk-list");
    if (diskList) {
      diskList.innerHTML = "";
      if (disks.length === 0) {
        diskList.innerHTML = '<div class="disk-empty">未检测到外置 U 盘或扩展数据分区（插入外置磁盘后点右上角「重新扫描外设」）。</div>';
      } else {
        disks.forEach((d) => {
          const item = document.createElement("div");
          item.className = "disk-item";
          const left = document.createElement("div");
          const title = document.createElement("div");
          title.className = "disk-title";
          title.textContent = `${d.label || d.name} (${d.path})`;
          const busLabel = d.removable ? "USB/外置" : (d.tran ? d.tran.toUpperCase() : "扩展磁盘");
          const mountDesc = d.mountpoint ? `已挂载: ${d.mountpoint}${d.mounted_ro ? " (只读)" : ""}` : "未挂载";
          const fstabDesc = d.fstab_mount ? ` · 开机自动挂载: ${d.fstab_mount} (${d.fstab_ro ? "只读" : "读写"})` : "";
          const sub = document.createElement("div");
          sub.className = "disk-sub";
          sub.textContent = `${busLabel} · ${d.size || "-"} · ${d.fstype || "-"} · ${mountDesc}${fstabDesc}`;
          left.append(title, sub);

          const actions = document.createElement("div");
          actions.className = "disk-tags";
          if (d.mountpoint) {
            const openBtn = document.createElement("button");
            openBtn.type = "button";
            openBtn.className = "btn small primary";
            openBtn.textContent = "打开目录";
            openBtn.setAttribute("data-cmd", `devices:open:${d.mountpoint}`);
            const umountBtn = document.createElement("button");
            umountBtn.type = "button";
            umountBtn.className = "btn small";
            umountBtn.textContent = "卸载";
            umountBtn.setAttribute("data-cmd", `devices:unmount:${d.path}`);
            actions.append(openBtn, umountBtn);
          } else {
            const mountBtn = document.createElement("button");
            mountBtn.type = "button";
            mountBtn.className = "btn small primary";
            mountBtn.textContent = "挂载使用";
            mountBtn.setAttribute("data-cmd", `devices:mount:${d.path}`);
            const roBtn = document.createElement("button");
            roBtn.type = "button";
            roBtn.className = "btn small";
            roBtn.textContent = "只读挂载";
            roBtn.title = "仅读取数据（适合 Windows 休眠/快速启动盘或重要资料备份）";
            roBtn.setAttribute("data-cmd", `devices:mount-ro:${d.path}`);
            actions.append(mountBtn, roBtn);
          }
          if (d.uuid && d.fstype && d.fstype !== "未格式化") {
            const fstabBtn = document.createElement("button");
            fstabBtn.type = "button";
            fstabBtn.className = "btn small";
            fstabBtn.textContent = d.fstab_mount ? "取消开机挂载" : "固定到 /mnt";
            fstabBtn.title = d.fstab_mount
              ? "从 /etc/fstab 移除开机自动挂载"
              : "写入 /etc/fstab，每次开机自动挂载到 /mnt 目录";
            fstabBtn.addEventListener("click", () => {
              if (d.fstab_mount && devicesSudoNopass) {
                sendCommand(buildKvCommand("devices:fstab-remove", { dev: d.path, password: "" }));
              } else {
                openFstabModal(d.fstab_mount ? "remove" : "set", d);
              }
            });
            actions.appendChild(fstabBtn);
          }
          if (d.removable && d.parent_disk) {
            const ejectBtn = document.createElement("button");
            ejectBtn.type = "button";
            ejectBtn.className = "btn small danger";
            ejectBtn.textContent = "安全弹出";
            ejectBtn.setAttribute("data-cmd", `devices:eject:${d.parent_disk}`);
            actions.appendChild(ejectBtn);
          }
          item.append(left, actions);
          diskList.appendChild(item);
        });
      }
    }

    const usbList = document.getElementById("dev-usb-list");
    if (usbList) {
      usbList.innerHTML = "";
      if (usb.length === 0) {
        usbList.innerHTML = '<div class="disk-empty">未检测到外接 USB 外设。</div>';
      } else {
        usb.forEach((u) => {
          const item = document.createElement("div");
          item.className = "disk-item";
          const left = document.createElement("div");
          const title = document.createElement("div");
          title.className = "disk-title";
          title.textContent = u.name || u.vid_pid;
          const sub = document.createElement("div");
          sub.className = "disk-sub";
          sub.textContent = `接口 ${u.bus_id} · ID ${u.vid_pid}${u.speed ? ` · ${u.speed}` : ""}`;
          left.append(title, sub);
          const tags = document.createElement("div");
          tags.className = "disk-tags";
          const drv = document.createElement("span");
          drv.className = "tag";
          drv.textContent = u.driver ? `驱动: ${u.driver}` : "已连接";
          tags.appendChild(drv);
          item.append(left, tags);
          usbList.appendChild(item);
        });
      }
    }

    const audioList = document.getElementById("dev-audio-list");
    if (audioList) {
      audioList.innerHTML = "";
      if (audioDevs.length === 0) {
        audioList.innerHTML = '<div class="disk-empty">未检测到音频输入或输出设备。</div>';
      } else {
        audioDevs.forEach((ad) => {
          const item = document.createElement("div");
          item.className = "disk-item";
          const left = document.createElement("div");
          const title = document.createElement("div");
          title.className = "disk-title";
          title.textContent = ad.label || ad.node;
          const sub = document.createElement("div");
          sub.className = "disk-sub";
          const dirText = ad.kind === "source" ? "音频输入（麦克风）" : "音频输出（音响 / 耳机 / HDMI）";
          const stText = !ad.enabled ? "已禁用" : (ad.is_default ? "启用中 · 当前默认" : "启用中");
          sub.textContent = `${dirText} · ${stText}`;
          left.append(title, sub);

          const actions = document.createElement("div");
          actions.className = "disk-tags";
          const stTag = document.createElement("span");
          stTag.className = "tag";
          stTag.textContent = ad.enabled ? (ad.kind === "source" ? "输入" : "输出") : "已禁用";
          const toggleBtn = document.createElement("button");
          toggleBtn.type = "button";
          if (ad.enabled) {
            toggleBtn.className = "btn small danger";
            toggleBtn.textContent = "禁用";
            toggleBtn.addEventListener("click", (ev) => {
              ev.stopPropagation();
              toggleBtn.disabled = true;
              toggleBtn.textContent = "应用中...";
              sendCommand(buildKvCommand("devices:audio-disable", {
                node: ad.node,
                kind: ad.kind,
                label: ad.label || ad.node
              }));
            });
          } else {
            toggleBtn.className = "btn small primary";
            toggleBtn.textContent = "启用";
            toggleBtn.addEventListener("click", (ev) => {
              ev.stopPropagation();
              toggleBtn.disabled = true;
              toggleBtn.textContent = "应用中...";
              sendCommand(`devices:audio-enable:${ad.node}`);
            });
          }
          actions.append(stTag, toggleBtn);
          item.append(left, actions);
          audioList.appendChild(item);
        });
      }
    }

    const inputList = document.getElementById("dev-input-list");
    if (inputList) {
      inputList.innerHTML = "";
      if (inputs.length === 0) {
        inputList.innerHTML = '<div class="disk-empty">未读取到外接键盘、鼠标或手柄。</div>';
      } else {
        inputs.forEach((inDev) => {
          const item = document.createElement("div");
          item.className = "disk-item";
          const left = document.createElement("div");
          const title = document.createElement("div");
          title.className = "disk-title";
          title.textContent = inDev.name;
          const sub = document.createElement("div");
          sub.className = "disk-sub";
          sub.textContent = `连接方式: ${inDev.bus}`;
          left.append(title, sub);
          const tags = document.createElement("div");
          tags.className = "disk-tags";
          const kind = document.createElement("span");
          kind.className = "tag";
          kind.textContent = inDev.kind;
          tags.appendChild(kind);
          item.append(left, tags);
          inputList.appendChild(item);
        });
      }
    }
  };
})();
