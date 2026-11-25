#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

if [[ -f "${ROOT_DIR}/.env.local" ]]; then
  # shellcheck disable=SC1091
  source "${ROOT_DIR}/.env.local"
fi

DEV_HOST="${SPINEL_DEV_HOST:-gamer@192.168.0.121}"
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
  --full      Upgrade OS packages (Flatpak/Thunar/etc.), labwc SpinelDark theme, scripts + spinel-shell
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

APPLY_SCRIPT="$(mktemp)"
CTL_SOCK="/tmp/spinel-ctl-$$.sock"
cleanup() {
  ssh -o "ControlPath=${CTL_SOCK}" -O exit "${DEV_HOST}" >/dev/null 2>&1 || true
  rm -f "${APPLY_SCRIPT}" "${CTL_SOCK}"
}
trap cleanup EXIT
cat > "${APPLY_SCRIPT}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
MODE="$1"

# Acquire sudo once upfront with real TTY
sudo -v

if [ "$MODE" != "ui" ]; then
  if ! pacman -Qq vte4 >/dev/null 2>&1; then
    echo "==> Installing vte4 runtime for Spinel Terminal..."
    sudo pacman -Sy --noconfirm --needed vte4
  fi
  sudo install -Dm755 /tmp/spinel-dev/spinel-shell /usr/local/bin/spinel-shell
  sudo install -Dm755 /tmp/spinel-dev/spinel-webui /usr/local/bin/spinel-webui
  sudo install -Dm755 /tmp/spinel-dev/spinel-terminal /usr/local/bin/spinel-terminal
fi
sudo mkdir -p /usr/share/spinel-shell/ui
sudo cp -a /tmp/spinel-dev/ui/. /usr/share/spinel-shell/ui/

if [ "$MODE" = "full" ]; then
  echo "==> Updating SpinelDark theme, Plymouth splash, labwc/terminal config & desktop packages..."
  sudo pacman -Syu --noconfirm --needed \
    flatpak vte4 plymouth mako libnotify swaylock swayidle grim slurp brightnessctl power-profiles-daemon \
    zram-generator btrfs-progs snapper reflector \
    spice-vdagent thunar thunar-archive-plugin file-roller gvfs mousepad ristretto wlr-randr wl-clipboard wl-clip-persist os-prober \
    polkit-gnome xdg-desktop-portal-gtk noto-fonts-emoji \
    fcitx5 fcitx5-chinese-addons fcitx5-rime fcitx5-gtk fcitx5-qt fcitx5-configtool
  fc-cache -f >/dev/null 2>&1 || true
  for legacy_ime in fcitx5-pinyin-zhwiki rime-pinyin-simp; do
    if pacman -Qq "$legacy_ime" >/dev/null 2>&1; then
      echo "==> Removing legacy IME dictionary package $legacy_ime..."
      sudo pacman -Rns --noconfirm "$legacy_ime" 2>/dev/null || true
    fi
  done
  if [ -e /dev/virtio-ports/com.redhat.spice.0 ]; then
    sudo systemctl enable --now spice-vdagentd.service 2>/dev/null || true
  fi
  sudo systemctl enable --now power-profiles-daemon.service 2>/dev/null || true

  sudo mkdir -p /etc/xdg/labwc /etc/xdg/xdg-desktop-portal /etc/udev/rules.d /etc/plymouth /usr/share/themes/SpinelDark /usr/share/themes/SpinelLight /usr/share/fcitx5/themes /usr/share/plymouth/themes/spinelos /usr/share/grub/themes/spinelos /boot/grub/themes/spinelos /usr/share/pixmaps /usr/share/icons/hicolor/256x256/apps /usr/share/icons/hicolor/scalable/apps
  sudo cp -a /tmp/spinel-dev/labwc/. /etc/xdg/labwc/
  sudo cp -a /tmp/spinel-dev/portals/. /etc/xdg/xdg-desktop-portal/
  sudo cp -a /tmp/spinel-dev/SpinelDark/. /usr/share/themes/SpinelDark/
  sudo cp -a /tmp/spinel-dev/SpinelLight/. /usr/share/themes/SpinelLight/
  sudo cp -a /tmp/spinel-dev/fcitx5-themes/. /usr/share/fcitx5/themes/
  sudo cp -a /tmp/spinel-dev/plymouth-theme/. /usr/share/plymouth/themes/spinelos/
  sudo cp -a /tmp/spinel-dev/grub-theme/. /usr/share/grub/themes/spinelos/
  sudo cp -a /tmp/spinel-dev/grub-theme/. /boot/grub/themes/spinelos/
  sudo install -Dm644 /tmp/spinel-dev/plymouthd.conf /etc/plymouth/plymouthd.conf
  sudo install -Dm644 /tmp/spinel-dev/faillock.conf /etc/security/faillock.conf
  sudo install -Dm644 /tmp/spinel-dev/spinelos.png /usr/share/pixmaps/spinelos.png
  sudo install -Dm644 /tmp/spinel-dev/spinelos.png /usr/share/icons/hicolor/256x256/apps/spinelos.png
  sudo install -Dm644 /tmp/spinel-dev/spinelos.svg /usr/share/icons/hicolor/scalable/apps/spinelos.svg
  sudo gtk-update-icon-cache -f /usr/share/icons/hicolor 2>/dev/null || true
  if [ -f /etc/mkinitcpio.conf ] && ! grep -q '\bplymouth\b' /etc/mkinitcpio.conf; then
    echo "==> Enabling Plymouth boot splash in /etc/mkinitcpio.conf..."
    sudo sed -i 's/\budev\b/udev plymouth/' /etc/mkinitcpio.conf
    sudo mkinitcpio -P
  else
    sudo mkinitcpio -P
  fi
  if [ -f /etc/default/grub ]; then
    echo "==> Configuring hidden SpinelOS GRUB boot & Plymouth kernel args..."
    set_vm_grub() {
      sudo sed -i "/^#*\s*${1}=/d" /etc/default/grub
      echo "${1}=${2}" | sudo tee -a /etc/default/grub >/dev/null
    }
    set_vm_grub GRUB_DISTRIBUTOR '"SpinelOS"'
    set_vm_grub GRUB_TIMEOUT_STYLE 'hidden'
    set_vm_grub GRUB_TIMEOUT '1'
    set_vm_grub GRUB_GFXMODE 'auto'
    set_vm_grub GRUB_GFXPAYLOAD_LINUX 'keep'
    set_vm_grub GRUB_THEME '"/boot/grub/themes/spinelos/theme.txt"'
    if ! grep -q '\bsplash\b' /etc/default/grub; then
      sudo sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT="\(.*\)"/GRUB_CMDLINE_LINUX_DEFAULT="\1 splash vt.global_cursor_default=0"/' /etc/default/grub
    elif ! grep -q 'vt\.global_cursor_default=0' /etc/default/grub; then
      sudo sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT="\(.*\)"/GRUB_CMDLINE_LINUX_DEFAULT="\1 vt.global_cursor_default=0"/' /etc/default/grub
    fi
    sudo grub-mkconfig -o /boot/grub/grub.cfg
  fi
  sudo install -Dm644 /tmp/spinel-dev/zram-generator.conf /etc/systemd/zram-generator.conf
  sudo install -Dm644 /tmp/spinel-dev/90-spinel-backlight.rules /etc/udev/rules.d/90-spinel-backlight.rules
  sudo install -Dm644 /tmp/spinel-dev/75-spinel-fonts.conf /etc/fonts/conf.d/75-spinel-fonts.conf
  if [ -f /usr/share/fontconfig/conf.avail/70-no-bitmaps-except-emoji.conf ]; then
    sudo ln -sf /usr/share/fontconfig/conf.avail/70-no-bitmaps-except-emoji.conf /etc/fonts/conf.d/70-no-bitmaps-except-emoji.conf
  fi
  sudo fc-cache -f >/dev/null 2>&1 || true
  sudo udevadm control --reload-rules 2>/dev/null || true
  sudo udevadm trigger --subsystem-match=backlight 2>/dev/null || true
  sudo systemctl daemon-reload 2>/dev/null || true
  sudo systemctl start systemd-zram-setup@zram0.service 2>/dev/null || true
  sudo install -Dm755 /tmp/spinel-dev/start-spinel-desktop /usr/local/bin/start-spinel-desktop
  sudo install -Dm755 /tmp/spinel-dev/spinel-install /usr/local/bin/spinel-install
  sudo install -Dm755 /tmp/spinel-dev/spinel-screenshot /usr/local/bin/spinel-screenshot
  sudo install -Dm755 /tmp/spinel-dev/spinel-lock /usr/local/bin/spinel-lock
fi

UID_NUM="$(id -u)"
export XDG_RUNTIME_DIR="/run/user/${UID_NUM}"
export DBUS_SESSION_BUS_ADDRESS="unix:path=${XDG_RUNTIME_DIR}/bus"
export GDK_BACKEND=wayland
export GTK_THEME=Adwaita
export XMODIFIERS=@im=fcitx
export QT_IM_MODULE=fcitx
export SDL_IM_MODULE=fcitx

WAYLAND_SOCK="$(ls "${XDG_RUNTIME_DIR}"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -n1 || true)"
if [ -z "$WAYLAND_SOCK" ]; then
  echo "[WARN] 未找到正在运行的 Wayland 会话，文件已更新，重启桌面即可生效。"
  exit 0
fi
export WAYLAND_DISPLAY="$(basename "$WAYLAND_SOCK")"

if [ -z "${DISPLAY:-}" ]; then
  SYS_DISP="$(systemctl --user show-environment 2>/dev/null | sed -n 's/^DISPLAY=//p' | head -n1 || true)"
  if [ -n "$SYS_DISP" ]; then
    export DISPLAY="$SYS_DISP"
  elif [ -S /tmp/.X11-unix/X0 ]; then
    export DISPLAY=":0"
  else
    X11_SOCK="$(ls /tmp/.X11-unix/X* 2>/dev/null | head -n1 || true)"
    if [ -n "$X11_SOCK" ]; then
      export DISPLAY=":$(basename "$X11_SOCK" | sed 's/^X//')"
    fi
  fi
fi

if [ -d /sys/module/nvidia ] || lspci -n 2>/dev/null | grep -qi "10de:"; then
  export WEBKIT_DISABLE_DMABUF_RENDERER=1
elif command -v systemd-detect-virt >/dev/null 2>&1 && systemd-detect-virt -q; then
  export WEBKIT_DISABLE_DMABUF_RENDERER=1
fi

LABWC_PID="$(pgrep -x labwc | head -n1 || true)"
if [ -n "${LABWC_PID}" ] && [ -r "/proc/${LABWC_PID}/environ" ]; then
  if tr '\0' '\n' < "/proc/${LABWC_PID}/environ" | grep -qx "WLR_RENDERER=pixman"; then
    export WLR_RENDERER=pixman
    export GSK_RENDERER=cairo
    export WEBKIT_DISABLE_COMPOSITING_MODE=1
    export LIBGL_ALWAYS_SOFTWARE=1
  fi
fi

if [ "$MODE" = "full" ]; then
  pkill -HUP -x labwc 2>/dev/null || true
  if [ -e /dev/virtio-ports/com.redhat.spice.0 ] && command -v spice-vdagent >/dev/null 2>&1; then
    pkill -x spice-vdagent 2>/dev/null || true
    nohup spice-vdagent >/dev/null 2>&1 </dev/null &
  fi
  if [ -x /usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1 ]; then
    pkill -f polkit-gnome-authentication-agent-1 2>/dev/null || true
    sleep 0.1
    nohup /usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1 >/tmp/spinel-polkit.log 2>&1 </dev/null &
  fi
  if command -v swayidle >/dev/null 2>&1; then
    pkill -x swayidle 2>/dev/null || true
    if grep -qx "idle_lock=true" "$HOME/.config/spinelos/power.conf" 2>/dev/null; then
      nohup swayidle -w timeout 900 'spinel-lock' before-sleep 'spinel-lock' >/tmp/spinel-idle.log 2>&1 </dev/null &
    else
      nohup swayidle -w before-sleep 'spinel-lock' >/tmp/spinel-idle.log 2>&1 </dev/null &
    fi
  fi
  if command -v systemctl >/dev/null 2>&1; then
    dbus-update-activation-environment --systemd DISPLAY WAYLAND_DISPLAY XDG_CURRENT_DESKTOP GTK_THEME 2>/dev/null || true
    systemctl --user restart xdg-desktop-portal xdg-desktop-portal-gtk xdg-desktop-portal-wlr >/dev/null 2>&1 || true
  fi
fi

if command -v fcitx5 >/dev/null 2>&1; then
  pkill -x fcitx5 2>/dev/null || true
  sleep 0.2
fi

pkill -x spinel-webui 2>/dev/null || true
pkill -x spinel-shell 2>/dev/null || true
sleep 0.3
nohup /usr/local/bin/spinel-shell >/tmp/spinel-shell.log 2>&1 </dev/null &

if command -v mako >/dev/null 2>&1; then
  pkill -x mako 2>/dev/null || true
  nohup mako >/tmp/spinel-mako.log 2>&1 </dev/null &
fi

if command -v wl-clip-persist >/dev/null 2>&1; then
  pkill -x wl-clip-persist 2>/dev/null || true
  nohup wl-clip-persist --clipboard regular >/tmp/spinel-clip.log 2>&1 </dev/null &
fi

if command -v fcitx5 >/dev/null 2>&1; then
  sleep 0.4
  setsid -f fcitx5 >/tmp/spinel-fcitx5.log 2>&1 </dev/null || true
fi
echo "Done! spinel-shell restarted on ${WAYLAND_DISPLAY}."
EOF
chmod +x "${APPLY_SCRIPT}"

echo "==> [2/3] Uploading assets to ${DEV_HOST}:${DEV_PORT}..."
SSH_OPTS=(-p "${DEV_PORT}" -o "ControlMaster=auto" -o "ControlPath=${CTL_SOCK}" -o "ControlPersist=120" -o "StrictHostKeyChecking=accept-new")
SSH_CMD="ssh -p ${DEV_PORT} -o ControlMaster=auto -o ControlPath=${CTL_SOCK} -o ControlPersist=120 -o StrictHostKeyChecking=accept-new"
ssh "${SSH_OPTS[@]}" "${DEV_HOST}" "rm -rf /tmp/spinel-dev && mkdir -p /tmp/spinel-dev/ui"
if [[ "${MODE}" != "ui" ]]; then
  rsync -avz -e "${SSH_CMD}" "${ROOT_DIR}/build/linux-amd64/spinel-shell" "${DEV_HOST}:/tmp/spinel-dev/spinel-shell"
  rsync -avz -e "${SSH_CMD}" "${ROOT_DIR}/build/linux-amd64/spinel-webui" "${DEV_HOST}:/tmp/spinel-dev/spinel-webui"
  rsync -avz -e "${SSH_CMD}" "${ROOT_DIR}/build/linux-amd64/spinel-terminal" "${DEV_HOST}:/tmp/spinel-dev/spinel-terminal"
fi
rsync -avz --delete -e "${SSH_CMD}" "${ROOT_DIR}/desktop/ui/" "${DEV_HOST}:/tmp/spinel-dev/ui/"
rsync -avz -e "${SSH_CMD}" "${APPLY_SCRIPT}" "${DEV_HOST}:/tmp/spinel-dev/apply.sh"

if [[ "${MODE}" == "full" ]]; then
  rsync -avz -e "${SSH_CMD}" \
    "${ROOT_DIR}/archiso/airootfs/etc/xdg/labwc/" "${DEV_HOST}:/tmp/spinel-dev/labwc/"
  rsync -avz -e "${SSH_CMD}" \
    "${ROOT_DIR}/archiso/airootfs/etc/xdg/xdg-desktop-portal/" "${DEV_HOST}:/tmp/spinel-dev/portals/"
  rsync -avz -e "${SSH_CMD}" \
    "${ROOT_DIR}/archiso/airootfs/usr/share/themes/SpinelDark/" "${DEV_HOST}:/tmp/spinel-dev/SpinelDark/"
  rsync -avz -e "${SSH_CMD}" \
    "${ROOT_DIR}/archiso/airootfs/usr/share/themes/SpinelLight/" "${DEV_HOST}:/tmp/spinel-dev/SpinelLight/"
  rsync -avz -e "${SSH_CMD}" \
    "${ROOT_DIR}/archiso/airootfs/usr/share/fcitx5/themes/" "${DEV_HOST}:/tmp/spinel-dev/fcitx5-themes/"
  rsync -avz -e "${SSH_CMD}" \
    "${ROOT_DIR}/archiso/airootfs/usr/share/plymouth/themes/spinelos/" "${DEV_HOST}:/tmp/spinel-dev/plymouth-theme/"
  rsync -avz -e "${SSH_CMD}" \
    "${ROOT_DIR}/archiso/airootfs/usr/share/grub/themes/spinelos/" "${DEV_HOST}:/tmp/spinel-dev/grub-theme/"
  rsync -avz -e "${SSH_CMD}" \
    "${ROOT_DIR}/archiso/airootfs/etc/plymouth/plymouthd.conf" \
    "${ROOT_DIR}/archiso/airootfs/etc/security/faillock.conf" \
    "${ROOT_DIR}/archiso/airootfs/usr/share/pixmaps/spinelos.png" \
    "${ROOT_DIR}/archiso/airootfs/usr/share/icons/hicolor/scalable/apps/spinelos.svg" \
    "${ROOT_DIR}/archiso/airootfs/etc/systemd/zram-generator.conf" \
    "${ROOT_DIR}/archiso/airootfs/etc/udev/rules.d/90-spinel-backlight.rules" \
    "${ROOT_DIR}/archiso/airootfs/etc/fonts/conf.d/75-spinel-fonts.conf" \
    "${ROOT_DIR}/archiso/airootfs/usr/local/bin/start-spinel-desktop" \
    "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-install" \
    "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-screenshot" \
    "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-lock" \
    "${DEV_HOST}:/tmp/spinel-dev/"
fi

echo "==> [3/3] Installing & restarting spinel-shell on live Wayland session..."
ssh -t "${SSH_OPTS[@]}" "${DEV_HOST}" "bash /tmp/spinel-dev/apply.sh '${MODE}'"
