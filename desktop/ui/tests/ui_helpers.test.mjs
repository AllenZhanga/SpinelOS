import test from "node:test";
import assert from "node:assert/strict";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const H = require("../js/ui_helpers.js");

test("formatTempBytes formats byte scales accurately", () => {
  assert.equal(H.formatTempBytes(0), "0 B");
  assert.equal(H.formatTempBytes(512), "512 B");
  assert.equal(H.formatTempBytes(2048), "2 KiB");
  assert.equal(H.formatTempBytes(5 * 1024 * 1024), "5.0 MiB");
  assert.equal(H.formatTempBytes(3 * 1024 ** 3), "3.00 GiB");
});

test("buildKvCommand skips nullish fields cleanly", () => {
  assert.equal(
    H.buildKvCommand("settings:proxy-save", {
      enabled: "true",
      http_proxy: "http://127.0.0.1:7890",
      unused: undefined,
      also_unused: null
    }),
    "settings:proxy-save\nenabled=true\nhttp_proxy=http://127.0.0.1:7890"
  );
});

test("filterAndSortProcs filters by query and sorts by CPU or memory", () => {
  const sample = [
    { name: "bash", cmd: "/bin/bash", pid: 101, cpu: 1.5, mem_mb: 12 },
    { name: "chromium", cmd: "/usr/lib/chromium", pid: 205, cpu: 24.0, mem_mb: 410 },
    { name: "spinel-shell", cmd: "/usr/local/bin/spinel-shell", pid: 88, cpu: 4.2, mem_mb: 64 }
  ];

  const byCpu = H.filterAndSortProcs(sample, "", "cpu");
  assert.deepEqual(byCpu.map((p) => p.pid), [205, 88, 101]);

  const byMem = H.filterAndSortProcs(sample, "", "mem");
  assert.deepEqual(byMem.map((p) => p.pid), [205, 88, 101]);

  const filtered = H.filterAndSortProcs(sample, "SHELL", "cpu");
  assert.equal(filtered.length, 1);
  assert.equal(filtered[0].pid, 88);
});

test("formatLabwcKey and eventToLabwcKey map shortcuts consistently", () => {
  assert.equal(H.formatLabwcKey("C-S-4"), "Ctrl + Shift + 4");
  assert.equal(H.formatLabwcKey("W-Return"), "Super + Enter");
  assert.equal(H.formatLabwcKey(""), "--");

  assert.equal(H.eventToLabwcKey({ key: "Control", code: "ControlLeft", ctrlKey: true }), null);
  assert.equal(H.eventToLabwcKey({ key: "a", code: "KeyA" }), null);
  assert.equal(H.eventToLabwcKey({ key: "Print", code: "PrintScreen" }), "Print");
  assert.equal(
    H.eventToLabwcKey({ key: "$", code: "Digit4", ctrlKey: true, shiftKey: true }),
    "C-S-4"
  );
  assert.equal(
    H.eventToLabwcKey({ key: "Enter", code: "Enter", metaKey: true }),
    "W-Return"
  );
});

test("buildMcpConfigSnippet renders client-specific TOML/JSON and paths", () => {
  const codex = H.buildMcpConfigSnippet("codex", { installed: true, path: "/home/u/.codex/config.toml" });
  assert.match(codex.snippet, /\[mcp_servers\.spinel_os\]/);
  assert.equal(codex.installed, true);
  assert.equal(codex.buttonText, "更新配置");
  assert.equal(codex.path, "/home/u/.codex/config.toml");

  const antigravity = H.buildMcpConfigSnippet("antigravity", null);
  const parsed = JSON.parse(antigravity.snippet);
  assert.equal(parsed.mcpServers["spinel-os"].env.SPINEL_AGENT_CLIENT, "Antigravity");
  assert.equal(antigravity.installed, false);
  assert.equal(antigravity.buttonText, "自动写入配置");
});

test("formatSnapStatusChip detects live, filesystem, and ready states", () => {
  assert.deepEqual(H.formatSnapStatusChip({ is_live: true }), { ready: false, text: "Live ISO 环境不可用" });
  assert.deepEqual(
    H.formatSnapStatusChip({ is_live: false, root_fs: "ext4" }),
    { ready: false, text: "根分区为 ext4" }
  );
  assert.deepEqual(
    H.formatSnapStatusChip({
      is_live: false,
      root_fs: "btrfs",
      snapper_installed: true,
      config_ready: true,
      snapshots: [{ id: 1 }, { id: 2 }]
    }),
    { ready: true, text: "Btrfs 已就绪 · 共 2 个快照" }
  );
});

test("proxy and network helpers validate and serialize cleanly", () => {
  assert.deepEqual(H.buildProxySavePayload(" ", "", "localhost"), {
    ok: false,
    error: "请至少填写 HTTP 或 SOCKS5 代理地址。"
  });
  const saved = H.buildProxySavePayload(" http://10.0.2.2:7890 ", "", "localhost,127.0.0.1");
  assert.equal(saved.ok, true);
  assert.equal(
    saved.payload,
    "settings:proxy-save\nenabled=true\nhttp_proxy=http://10.0.2.2:7890\nsocks_proxy=\nno_proxy=localhost,127.0.0.1"
  );

  const extSummary = H.formatProxySummary({
    enabled: false,
    external_proxy: "http://127.0.0.1:7897",
    gateway: "192.168.1.1"
  });
  assert.equal(extSummary.active, true);
  assert.match(extSummary.statusText, /外部代理生效/);
  assert.match(extSummary.gwHintText, /192\.168\.1\.1/);

  assert.equal(
    H.formatIfaceSummary({
      device: "eth0",
      connection: "Wired",
      method: "manual",
      ip: "192.168.1.50",
      prefix: "24",
      gateway: "192.168.1.1",
      dns: "223.5.5.5"
    }),
    "Wired · 手动静态 IP · 192.168.1.50/24 · 网关 192.168.1.1 · DNS 223.5.5.5"
  );
  assert.equal(
    H.buildNetIpPayload({ device: "wlan0", connection: "Home" }, { method: "auto", dns: "1.1.1.1" }),
    "settings:net-ip-apply\ndevice=wlan0\nconnection=Home\nmethod=auto\nip=\nmask=\ngateway=\ndns=1.1.1.1"
  );
});

test("mount folder name suggestion and validation enforce safe names", () => {
  assert.equal(H.suggestMntFolderName({ label: "My Games Disk!", name: "nvme0n1p2" }), "my_games_disk");
  assert.equal(H.suggestMntFolderName({ label: "中文磁盘", name: "sda1" }), "sda1");
  assert.equal(H.validateMntFolderName("  games_ssd-1 ").valid, true);
  assert.equal(H.validateMntFolderName("..").valid, false);
  assert.equal(H.validateMntFolderName("bad/name").valid, false);
});
