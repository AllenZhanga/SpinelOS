#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="${1:-amd64}"  # amd64 | arm64
OUT_DIR="${ROOT_DIR}/out/spinel-relay-ubuntu-${ARCH}"

GO_BIN="${GO_BIN:-$(command -v go || echo /usr/local/go/bin/go)}"
if [[ ! -x "${GO_BIN}" ]]; then
  echo "[ERROR] 未找到 Go 编译器，请安装 Go 1.22+" >&2
  exit 1
fi

mkdir -p "${OUT_DIR}"
echo "==> Testing spinel-relay..."
(cd "${ROOT_DIR}/server/spinel-relay" && "${GO_BIN}" test -v ./...)

echo "==> Cross-compiling linux/${ARCH} static binary..."
(cd "${ROOT_DIR}/server/spinel-relay" && CGO_ENABLED=0 GOOS=linux GOARCH="${ARCH}" "${GO_BIN}" build -trimpath -ldflags="-s -w" -o "${OUT_DIR}/spinel-relay" .)

cp -f "${ROOT_DIR}/server/spinel-relay/deploy/spinel-relay.service" "${OUT_DIR}/spinel-relay.service"
cp -f "${ROOT_DIR}/server/spinel-relay/deploy/install-ubuntu.sh" "${OUT_DIR}/install-ubuntu.sh"
chmod +x "${OUT_DIR}/install-ubuntu.sh"

echo "==> Ready! Bundle generated at: ${OUT_DIR}"
echo "    Upload & run on Ubuntu:"
echo "    scp -r ${OUT_DIR} <user>@<server-ip>:~/spinel-relay-pkg && ssh <user>@<server-ip> 'cd ~/spinel-relay-pkg && sudo ./install-ubuntu.sh'"
