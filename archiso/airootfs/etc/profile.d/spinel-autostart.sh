# Auto-launch SpinelOS Wayland desktop on tty1 for gamer user
if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ] && [ "$(tty 2>/dev/null)" = "/dev/tty1" ] && [ "$(id -un)" = "gamer" ]; then
  exec /usr/local/bin/start-spinel-desktop
fi
