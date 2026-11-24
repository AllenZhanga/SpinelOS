#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

if [[ -f "${ROOT_DIR}/.env.local" ]]; then
  # shellcheck disable=SC1091
  source "${ROOT_DIR}/.env.local"
fi

DEV_HOST="${SPINEL_DEV_HOST:-}"
DEV_PORT="${SPINEL_DEV_PORT:-22}"
MODE="app"  # app | ui | full

while [[ $# -gt 0 ]]; do
  case "$1" in
    --ui-only) MODE="ui"; shift ;;
    --full) MODE="full"; shift ;;
    -p|--port) DEV_PORT="${2:-22}"; shift 2 ;;
    -h|--help)
      cat <<'EOF'
Usage: ./scripts/dev-deploy.sh [user@ip] [-p port] [--ui-only | --full]
Fast hot-deploy to a running SpinelOS VM or PC:
  (default)   Incremental C++20 build + UI sync + live restart spinel-shell
  --ui-only   Sync desktop/ui/ only + live restart spinel-shell (~1s)
  --full      Upgrade OS packages (Chromium/Thunar/etc.), labwc SpinelDark theme, scripts + spinel-shell
EOF
      exit 0
      ;;
    *)
      DEV_HOST="$1"
      shift
      ;;
  esac
done

if [[ -z "${DEV_HOST}" ]]; then
  echo "[ERROR] 请传入目标 SpinelOS 地址（例如 ./scripts/dev-deploy.sh gamer@192.168.64.3），或在 .env.local 中设置 SPINEL_DEV_HOST。" >&2
  exit 1
fi

if [[ "${MODE}" != "ui" ]]; then
  echo "==> [1/3] Incremental C++20 build inside spinelos-builder..."
  mkdir -p "${ROOT_DIR}/build/linux-amd64"
  docker run --rm --platform linux/amd64 \
    -v "${ROOT_DIR}:/workspace" \
    spinelos-builder \
    bash -c 'cmake -S /workspace/desktop -B /workspace/build/linux-amd64 -DCMAKE_BUILD_TYPE=Release && cmake --build /workspace/build/linux-amd64 -j"$(nproc)"'
else
  echo "==> [1/3] Skipping C++ build (--ui-only)..."
fi

echo "==> [2/3] Uploading assets to ${DEV_HOST}:${DEV_PORT}..."
SSH_CMD="ssh -p ${DEV_PORT}"
ssh -p "${DEV_PORT}" "${DEV_HOST}" "rm -rf /tmp/spinel-dev && mkdir -p /tmp/spinel-dev/ui"
if [[ "${MODE}" != "ui" ]]; then
  rsync -avz -e "${SSH_CMD}" "${ROOT_DIR}/build/linux-amd64/spinel-shell" "${DEV_HOST}:/tmp/spinel-dev/spinel-shell"
fi
rsync -avz --delete -e "${SSH_CMD}" "${ROOT_DIR}/desktop/ui/" "${DEV_HOST}:/tmp/spinel-dev/ui/"

if [[ "${MODE}" == "full" ]]; then
  mkdir -p /tmp/spinel-sys-stage
  rsync -avz -e "${SSH_CMD}" \
    "${ROOT_DIR}/archiso/airootfs/etc/xdg/labwc/" "${DEV_HOST}:/tmp/spinel-dev/labwc/"
  rsync -avz -e "${SSH_CMD}" \
    "${ROOT_DIR}/archiso/airootfs/usr/share/themes/SpinelDark/" "${DEV_HOST}:/tmp/spinel-dev/SpinelDark/"
  rsync -avz -e "${SSH_CMD}" \
    "${ROOT_DIR}/archiso/airootfs/usr/local/bin/start-spinel-desktop" \
    "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-install" \
    "${DEV_HOST}:/tmp/spinel-dev/"
fi

echo "==> [3/3] Installing & restarting spinel-shell on live Wayland session..."
ssh -t -p "${DEV_PORT}" "${DEV_HOST}" bash -s -- "${MODE}" <<'EOF'
set -euo pipefail
MODE="$1"

if [ "$MODE" != "ui" ]; then
  sudo install -Dm755 /tmp/spinel-dev/spinel-shell /usr/local/bin/spinel-shell
fi
sudo mkdir -p /usr/share/spinel-shell/ui
sudo cp -a /tmp/spinel-dev/ui/. /usr/share/spinel-shell/ui/

if [ "$MODE" = "full" ]; then
  echo "==> Updating SpinelDark theme, labwc config & desktop packages..."
  sudo mkdir -p /etc/xdg/labwc /usr/share/themes/SpinelDark
  sudo cp -a /tmp/spinel-dev/labwc/. /etc/xdg/labwc/
  sudo cp -a /tmp/spinel-dev/SpinelDark/. /usr/share/themes/SpinelDark/
  sudo install -Dm755 /tmp/spinel-dev/start-spinel-desktop /usr/local/bin/start-spinel-desktop
  sudo install -Dm755 /tmp/spinel-dev/spinel-install /usr/local/bin/spinel-install

  sudo pacman -Syu --noconfirm --needed \
    chromium thunar thunar-archive-plugin file-roller gvfs mousepad ristretto wlr-randr wl-clipboard os-prober
fi

UID_NUM="$(id -u)"
export XDG_RUNTIME_DIR="/run/user/${UID_NUM}"
export DBUS_SESSION_BUS_ADDRESS="unix:path=${XDG_RUNTIME_DIR}/bus"
export GDK_BACKEND=wayland
export GTK_THEME=Adwaita:dark

WAYLAND_SOCK="$(ls "${XDG_RUNTIME_DIR}"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -n1 || true)"
if [ -z "$WAYLAND_SOCK" ]; then
  echo "[WARN] 未找到正在运行的 Wayland 会话，文件已更新，重启桌面即可生效。"
  exit 0
fi
export WAYLAND_DISPLAY="$(basename "$WAYLAND_SOCK")"

if [ -d /sys/module/nvidia ] || lspci -n 2>/dev/null | grep -qi "10de:"; then
  export WEBKIT_DISABLE_DMABUF_RENDERER=1
elif command -v systemd-detect-virt >/dev/null 2>&1 && systemd-detect-virt -q; then
  export WEBKIT_DISABLE_DMABUF_RENDERER=1
fi

if [ "$MODE" = "full" ]; then
  labwc --reconfigure 2>/dev/null || true
fi

pkill -x spinel-shell 2>/dev/null || true
sleep 0.3
nohup /usr/local/bin/spinel-shell >/tmp/spinel-shell.log 2>&1 </dev/null &
echo "Done! spinel-shell restarted on ${WAYLAND_DISPLAY}."
EOF
