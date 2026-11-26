#!/usr/bin/env bash
set -euo pipefail

# Run this script ON your Ubuntu cloud server as root / sudo user
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_SRC="${SCRIPT_DIR}/spinel-relay"

if [[ ! -f "${BIN_SRC}" ]]; then
  echo "[ERROR] 找不到二进制文件 ${BIN_SRC}，请先在开发机运行 ./scripts/build-relay.sh 生成并上传。" >&2
  exit 1
fi

sudo install -Dm755 "${BIN_SRC}" /usr/local/bin/spinel-relay
sudo mkdir -p /var/lib/spinel-relay
sudo chmod 700 /var/lib/spinel-relay
sudo install -Dm644 "${SCRIPT_DIR}/spinel-relay.service" /etc/systemd/system/spinel-relay.service

sudo systemctl daemon-reload
sudo systemctl enable --now spinel-relay

if command -v ufw >/dev/null 2>&1; then
  sudo ufw allow 7443/tcp comment 'Spinel Relay TLS' || true
  sudo ufw allow 7444/udp comment 'Spinel STUN UDP' || true
fi

sleep 1
echo "==> spinel-relay status:"
sudo systemctl --no-pager -l status spinel-relay | head -n 15
echo ""
echo "==> 服务器公钥指纹 (在客户端设置留空可首次自动信任，或填入此值防劫持):"
sudo cat /var/lib/spinel-relay/fingerprint.txt 2>/dev/null || true
