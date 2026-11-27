((rootFactory) => {
  const api = rootFactory();
  if (typeof module !== "undefined" && module.exports) {
    module.exports = api;
  }
  if (typeof window !== "undefined") {
    window.SpinelHelpers = api;
    if (window.SpinelUI) window.SpinelUI.helpers = api;
  }
})(() => {
  function formatTempBytes(bytes) {
    const n = Number(bytes || 0);
    if (n >= 1024 ** 3) return `${(n / 1024 ** 3).toFixed(2)} GiB`;
    if (n >= 1024 ** 2) return `${(n / 1024 ** 2).toFixed(1)} MiB`;
    if (n >= 1024) return `${(n / 1024).toFixed(0)} KiB`;
    return `${n} B`;
  }

  function buildKvCommand(cmd, fields) {
    const lines = [cmd];
    if (fields && typeof fields === "object") {
      Object.entries(fields).forEach(([key, val]) => {
        if (val !== undefined && val !== null) {
          lines.push(`${key}=${String(val)}`);
        }
      });
    }
    return lines.join("\n");
  }

  function filterAndSortProcs(procs, query = "", sortBy = "cpu") {
    const q = String(query || "").trim().toLowerCase();
    let list = Array.isArray(procs) ? [...procs] : [];
    if (q) {
      list = list.filter((p) => `${p.name || ""} ${p.cmd || ""} ${p.pid || ""}`.toLowerCase().includes(q));
    }
    list.sort((a, b) => {
      if (sortBy === "mem") return (Number(b.mem_mb || 0) - Number(a.mem_mb || 0)) || (Number(b.cpu || 0) - Number(a.cpu || 0));
      return (Number(b.cpu || 0) - Number(a.cpu || 0)) || (Number(b.mem_mb || 0) - Number(a.mem_mb || 0));
    });
    return list;
  }

  function buildMcpConfigSnippet(clientKey, clientStatus = null) {
    const t = String(clientKey || "antigravity");
    let snippet = "";
    if (t === "codex") {
      snippet = '[mcp_servers.spinel_os]\ncommand = "/usr/local/bin/spinel-os-mcp"\nenv = { SPINEL_AGENT_CLIENT = "Codex" }';
    } else {
      const clientNames = {
        antigravity: "Antigravity",
        claude: "Claude",
        deepseek: "DeepSeekHarness",
        gemini: "GeminiCLI"
      };
      const cname = clientNames[t] || "GeminiCLI";
      snippet = JSON.stringify({
        mcpServers: {
          "spinel-os": {
            command: "/usr/local/bin/spinel-os-mcp",
            args: [],
            env: { SPINEL_AGENT_CLIENT: cname }
          }
        }
      }, null, 2);
    }

    const defaultPaths = {
      antigravity: "~/.gemini/config/mcp_config.json",
      codex: "~/.codex/config.toml",
      claude: "~/.claude.json",
      deepseek: "~/.config/deepseek/mcp_config.json",
      gemini: "~/.gemini/settings.json"
    };
    const installed = Boolean(clientStatus && clientStatus.installed);
    return {
      snippet,
      path: (clientStatus && clientStatus.path) || defaultPaths[t] || "",
      installed,
      buttonText: installed ? "更新配置" : "自动写入配置"
    };
  }

  function formatLabwcKey(raw) {
    if (!raw) return "--";
    return String(raw)
      .split("-")
      .map((part) => {
        if (part === "C") return "Ctrl";
        if (part === "S") return "Shift";
        if (part === "A") return "Alt";
        if (part === "W") return "Super";
        if (part === "Return") return "Enter";
        if (part === "space") return "Space";
        if (part.length === 1) return part.toUpperCase();
        return part;
      })
      .join(" + ");
  }

  function eventToLabwcKey(ev) {
    if (!ev) return null;
    const modKeys = new Set(["Control", "Shift", "Alt", "Meta", "Super", "OS"]);
    if (modKeys.has(ev.key)) return null;
    let keyName = "";
    const c = ev.code || "";
    if (/^Key[A-Z]$/.test(c)) keyName = c.slice(3).toLowerCase();
    else if (/^Digit[0-9]$/.test(c)) keyName = c.slice(5);
    else if (/^F([1-9]|1[0-2])$/.test(c)) keyName = c;
    else if (c === "Enter" || c === "NumpadEnter") keyName = "Return";
    else if (c === "Space") keyName = "space";
    else if (c === "Tab") keyName = "Tab";
    else if (c === "Backspace") keyName = "BackSpace";
    else if (c === "Delete") keyName = "Delete";
    else if (c === "PrintScreen" || ev.key === "Print") keyName = "Print";
    else if (c === "Minus") keyName = "minus";
    else if (c === "Equal") keyName = "equal";
    else if (c === "Slash") keyName = "slash";
    else if (/^[a-zA-Z0-9]$/.test(ev.key || "")) keyName = ev.key.toLowerCase();
    if (!keyName) return null;

    const mods = [];
    if (ev.ctrlKey) mods.push("C");
    if (ev.altKey) mods.push("A");
    if (ev.shiftKey) mods.push("S");
    if (ev.metaKey) mods.push("W");
    const allowBare = (keyName === "Print" || /^F([1-9]|1[0-2])$/.test(keyName));
    if (mods.length === 0 && !allowBare) return null;
    return mods.length > 0 ? `${mods.join("-")}-${keyName}` : keyName;
  }

  function formatSnapStatusChip(data) {
    const snaps = Array.isArray(data?.snapshots) ? data.snapshots : [];
    const ready = Boolean(data && !data.is_live && data.root_fs === "btrfs" && data.snapper_installed && data.config_ready);
    let text = "";
    if (!data || data.is_live) text = "Live ISO 环境不可用";
    else if (data.root_fs !== "btrfs") text = `根分区为 ${data.root_fs || "非 Btrfs"}`;
    else if (!data.snapper_installed) text = "未安装 snapper";
    else if (!data.config_ready) text = "未初始化 root 快照配置";
    else text = `Btrfs 已就绪 · 共 ${snaps.length} 个快照`;
    return { ready, text };
  }

  function formatProxySummary(data) {
    const enabled = Boolean(data?.enabled);
    const hasExt = !enabled && Boolean(data?.external_proxy);
    const gateway = data?.gateway || "";
    return {
      active: enabled || hasExt,
      statusText: enabled
        ? "已启用"
        : (hasExt ? `外部代理生效 (${data.external_proxy})` : "已关闭"),
      gwHintText: gateway
        ? `当前默认网关: ${gateway}（NAT 模式下一般即为宿主机代理可达网关，需开启宿主机的“允许局域网连接”）。`
        : "适用于 NAT 模式连接 Windows/Mac 宿主机代理软件（如 Clash / v2rayN，需开启“允许局域网连接”）。"
    };
  }

  function buildProxySavePayload(httpVal, socksVal, noVal) {
    const httpProxy = String(httpVal || "").trim();
    const socksProxy = String(socksVal || "").trim();
    const noProxy = String(noVal || "").trim();
    if (!httpProxy && !socksProxy) {
      return { ok: false, error: "请至少填写 HTTP 或 SOCKS5 代理地址。" };
    }
    return {
      ok: true,
      payload: buildKvCommand("settings:proxy-save", {
        enabled: "true",
        http_proxy: httpProxy,
        socks_proxy: socksProxy,
        no_proxy: noProxy
      })
    };
  }

  function formatIfaceSummary(ifx) {
    if (!ifx) return "";
    if (!ifx.connection) return `状态: ${ifx.state || "--"}`;
    const modeLabel = ifx.method === "manual" ? "手动静态 IP" : "DHCP 自动";
    const addrDesc = ifx.ip ? `${ifx.ip}/${ifx.prefix || "24"}` : "未分配 IP";
    const gwDesc = ifx.gateway ? ` · 网关 ${ifx.gateway}` : "";
    const dnsDesc = ifx.dns ? ` · DNS ${ifx.dns}` : "";
    return `${ifx.connection} · ${modeLabel} · ${addrDesc}${gwDesc}${dnsDesc}`;
  }

  function buildNetIpPayload(ifx, fields = {}) {
    if (!ifx || !ifx.device) return "";
    return buildKvCommand("settings:net-ip-apply", {
      device: ifx.device,
      connection: ifx.connection || "",
      method: fields.method || "auto",
      ip: String(fields.ip || "").trim(),
      mask: String(fields.mask || "").trim(),
      gateway: String(fields.gateway || "").trim(),
      dns: String(fields.dns || "").trim()
    });
  }

  function suggestMntFolderName(d) {
    const ascii = String(d?.label || "")
      .replace(/[^A-Za-z0-9._-]+/g, "_")
      .replace(/^_+|_+$/g, "")
      .slice(0, 28);
    if (ascii.length >= 3) return ascii.toLowerCase();
    return String(d?.name || "disk").replace(/[^A-Za-z0-9._-]+/g, "_");
  }

  function validateMntFolderName(raw) {
    const name = String(raw || "").trim();
    if (!/^[A-Za-z0-9._-]{1,48}$/.test(name) || name === "." || name === "..") {
      return { valid: false, name, error: "挂载目录名仅支持 1~48 位英文、数字、下划线、点或连字符。" };
    }
    return { valid: true, name, error: "" };
  }

  return {
    formatTempBytes,
    buildKvCommand,
    filterAndSortProcs,
    buildMcpConfigSnippet,
    formatLabwcKey,
    eventToLabwcKey,
    formatSnapStatusChip,
    formatProxySummary,
    buildProxySavePayload,
    formatIfaceSummary,
    buildNetIpPayload,
    suggestMntFolderName,
    validateMntFolderName
  };
});
