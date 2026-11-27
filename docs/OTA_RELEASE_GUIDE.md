# SpinelOS 云端 OTA 与 Relay 服务端发布指南

本文档记录 SpinelOS 云端 OTA 更新包发布、`spinel-relay` 服务端升级及发布前后核对清单，确保每次迭代不会遗漏云端分发同步。

---

## 1. 服务架构：OTA 与远程沙箱共用 `spinel-relay`

SpinelOS 的**远程沙箱/云接力中继**与 **OTA 分发**由同一个 Go 服务端程序 [server/spinel-relay/main.go](file:///Users/mac/z-svt/git/0_github/SpinelOS/server/spinel-relay/main.go) 统一托管：

- **统一服务与端口**（定义于 [spinel-relay.service](file:///Users/mac/z-svt/git/0_github/SpinelOS/server/spinel-relay/deploy/spinel-relay.service)）：
  - `TCP/TLS :7443`：同时承接远程互联/沙箱中继信令与 `https://<server>:7443/ota/` 静态文件分发；
  - `UDP :7444`：STUN / 直连辅助端口；
  - 默认数据目录 `/var/lib/spinel-relay`，OTA 静态目录 `/var/lib/spinel-relay/ota`。
- **共用同一套鉴权**：
  - `spinel-relay` 默认开启 OTA 鉴权（`-ota-public=false`）；
  - 客户端 [ota.conf](file:///Users/mac/z-svt/git/0_github/SpinelOS/archiso/airootfs/etc/spinelos/ota.conf) 将 `OTA_BASE_URL` 留空，[spinel-update](file:///Users/mac/z-svt/git/0_github/SpinelOS/archiso/airootfs/usr/local/bin/spinel-update) 会自动读取 `~/.config/spinelos/relay.conf` 中的 `server_addr`、`device_id` 与 `token` 访问 `https://<server>:7443/ota/`。

---

## 2. 本地密钥与环境约定（严禁提交到 Git）

1. **`.env.local`**（已在 `.gitignore`）：配置默认 OTA 上传目标，避免每次手敲服务器地址：
   ```bash
   SPINEL_OTA_HOST=user@your-relay-server
   # 可选，默认 /var/lib/spinel-relay/ota
   # SPINEL_OTA_REMOTE_DIR=/var/lib/spinel-relay/ota
   ```
2. **签名密钥对**：
   - 私钥：`.secrets/ota-signing-key.pem`（ECDSA P-256，本地私有目录 `.secrets/`，禁止入库）。
   - 公钥：[archiso/airootfs/etc/spinelos/ota-pubkey.pem](file:///Users/mac/z-svt/git/0_github/SpinelOS/archiso/airootfs/etc/spinelos/ota-pubkey.pem)（入库并打包进 Live ISO、OTA 包及客户端 `/etc/spinelos/ota-pubkey.pem`）。
   - 若 `.secrets/ota-signing-key.pem` 重建导致公钥变化，必须同时把新的 `ota-pubkey.pem` 提交到仓库，并对存量设备执行一次 `--full` 热部署或离线 bootstrap 更新，否则客户端验签会失败。

---

## 3. 发布流程速查

### 3.1 常规客户端更新：发布云端 OTA（最常用）

适用场景：修改了 `desktop/` C++ 原生组件、前端 `desktop/ui/`、`.desktop`/MIME/图标，或系统配置与脚本（`labwc`、主题、`start-spinel-desktop`、`spinel-install`、`spinel-update` 等）。

1. **确认本地代码已通过构建、单测并提交**：
   ```bash
   git status --short
   ```
2. **一键编译、打包、签名并上传到云端 OTA**：
   ```bash
   ./scripts/release-ota.sh
   ```
   常用可选参数：
   - 临时指定服务器：`./scripts/release-ota.sh --host user@server`
   - 仅本地打包不上传（产物输出至 `out/ota/`）：`./scripts/release-ota.sh --no-upload`
   - 自定义版本号 / 更新说明：`./scripts/release-ota.sh --version 0.1.150-abc1234 --notes "说明文案"`

`release-ota.sh` 会自动完成：
- 在 `spinelos-builder` 容器中编译 Release 全目标并运行 `ctest`；
- 剥离 ELF 调试符号、打包二进制、`ui/`、`assets/`、`full/` 系统配置及 `ota-pubkey.pem`；
- 生成 `latest.json`（含 SHA256 与提交信息）、使用 `.secrets/ota-signing-key.pem` 签名生成 `latest.json.sig`，并生成 `bootstrap.sh`；
- 通过 `ssh` + `sudo rsync --delete` 同步至 `${SPINEL_OTA_HOST}:${SPINEL_OTA_REMOTE_DIR:-/var/lib/spinel-relay/ota}/`。

### 3.2 升级云端中继服务：`spinel-relay`

适用场景：修改了 `server/spinel-relay/` 下的 Go 源码或 systemd 单元配置。

```bash
./scripts/build-relay.sh
scp -r out/spinel-relay-ubuntu-amd64 <user>@<server>:~/spinel-relay-pkg
ssh -t <user>@<server> 'cd ~/spinel-relay-pkg && sudo ./install-ubuntu.sh'
```

### 3.3 重新制作完整 Live ISO

适用场景：增删 Arch 系统软件包（`archiso/packages.custom.x86_64`）、调整 GRUB/Plymouth 底层引导或需要产出新装机镜像。

```bash
./scripts/build-iso.sh
```

---

## 4. 变更一致性同步清单

每当新增系统二进制、`.desktop` 入口、字体/主题/udev/polkit/systemd 等系统级配置时，需同步检查以下 6 处：

1. [archiso/packages.custom.x86_64](file:///Users/mac/z-svt/git/0_github/SpinelOS/archiso/packages.custom.x86_64)（Arch 软件包清单）
2. [docker/build-inside-container.sh](file:///Users/mac/z-svt/git/0_github/SpinelOS/docker/build-inside-container.sh)（Live ISO 权限与构筑）
3. [archiso/airootfs/usr/local/bin/spinel-install](file:///Users/mac/z-svt/git/0_github/SpinelOS/archiso/airootfs/usr/local/bin/spinel-install)（硬盘装机同步）
4. [scripts/dev-deploy.sh](file:///Users/mac/z-svt/git/0_github/SpinelOS/scripts/dev-deploy.sh)（开发机热部署）
5. [scripts/release-ota.sh](file:///Users/mac/z-svt/git/0_github/SpinelOS/scripts/release-ota.sh)（OTA 包收集清单）
6. [archiso/airootfs/usr/local/bin/spinel-update](file:///Users/mac/z-svt/git/0_github/SpinelOS/archiso/airootfs/usr/local/bin/spinel-update)（设备端解包与重启应用逻辑）

---

## 5. 发布后验证清单

1. **检查本地产物与远端清单**：
   ```bash
   cat out/ota/latest.json
   ```
2. **在已登录账号的 SpinelOS 客户端验证查新**：
   ```bash
   spinel-update check --json
   ```
3. **在客户端执行升级**（GUI 或 CLI）：
   - GUI：「系统设置 → 系统更新」或「应用中心」确认检测到新 `version` 与 `notes` 并可执行更新；
   - CLI：`sudo spinel-update upgrade --full --yes`；
   - 离线/未登录设备：`sudo spinel-update offline /path/to/spinelos-ota-*.tar.zst --full --yes`。
