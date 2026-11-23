#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

if [[ -f "${ROOT_DIR}/.env.local" ]]; then
  # shellcheck disable=SC1091
  source "${ROOT_DIR}/.env.local"
fi

ECS_HOST="${SPINEL_ECS_HOST:-}"
ECS_DIR="${SPINEL_ECS_DIR:-~/SpinelOS}"
ACTION="sync"

for arg in "$@"; do
  case "$arg" in
    --build) ACTION="build" ;;
    --pull) ACTION="pull" ;;
    -h|--help)
      cat <<'EOF'
Usage: ./scripts/sync-ecs.sh [--build | --pull]
Configure SPINEL_ECS_HOST in .env.local (gitignored) or environment variable.
Examples:
  SPINEL_ECS_HOST=ubuntu@1.2.3.4 ./scripts/sync-ecs.sh
  ./scripts/sync-ecs.sh --build   # Sync code and build ISO on ECS
  ./scripts/sync-ecs.sh --pull    # Download built ISO to ./out/
EOF
      exit 0
      ;;
    *)
      ECS_HOST="$arg"
      ;;
  esac
done

if [[ -z "${ECS_HOST}" ]]; then
  echo "[ERROR] SPINEL_ECS_HOST is not set. Create .env.local or pass user@ip." >&2
  exit 1
fi

SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=8)
if ! ssh "${SSH_OPTS[@]}" "${ECS_HOST}" "true" 2>/dev/null; then
  echo "[ERROR] 免密 SSH 尚未配置完成，请先在终端执行一次下面命令（输入一次密码即可）：" >&2
  echo "  ssh-copy-id -i ~/.ssh/id_rsa.pub ${ECS_HOST}" >&2
  exit 1
fi

if [[ "${ACTION}" == "pull" ]]; then
  mkdir -p "${ROOT_DIR}/out"
  echo "==> Pulling ISO from ${ECS_HOST}:${ECS_DIR}/out/ -> ${ROOT_DIR}/out/"
  rsync -avz --progress -e "ssh -o BatchMode=yes" \
    "${ECS_HOST}:${ECS_DIR}/out/*.iso" "${ROOT_DIR}/out/"
  exit 0
fi

echo "==> Syncing SpinelOS -> ${ECS_HOST}:${ECS_DIR}/"
rsync -avz \
  --exclude='.git' \
  --exclude='out' \
  --exclude='.DS_Store' \
  --exclude='.env.local' \
  -e "ssh -o BatchMode=yes" \
  "${ROOT_DIR}/" "${ECS_HOST}:${ECS_DIR}/"

if [[ "${ACTION}" == "build" ]]; then
  echo "==> Triggering remote ISO build on ${ECS_HOST}..."
  ssh -t -o BatchMode=yes "${ECS_HOST}" \
    "cd ${ECS_DIR} && chmod +x scripts/build-iso.sh docker/build-inside-container.sh && sudo ./scripts/build-iso.sh"
fi
