#!/usr/bin/env bash
set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
PROFILE_DIR="/tmp/spinel-archiso-profile"
WORK_DIR="/tmp/spinel-archiso-work"
BUILD_DIR="/tmp/spinel-shell-build"
OUT_DIR="${WORKSPACE}/out"

rm -rf "${PROFILE_DIR}" "${WORK_DIR}" "${BUILD_DIR}"
mkdir -p "${OUT_DIR}"

echo "[1/5] Building custom C++20 Wayland Desktop Shell (spinel-shell)..."
cmake -S "${WORKSPACE}/desktop" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "[2/5] Preparing ArchISO profile from releng..."
cp -r /usr/share/archiso/configs/releng "${PROFILE_DIR}"

# Ensure [multilib] is enabled, enable parallel downloads, and disable seccomp sandbox
if ! grep -q '^DisableSandboxSyscalls' "${PROFILE_DIR}/pacman.conf"; then
  sed -i '/^\[options\]/a DisableSandboxSyscalls' "${PROFILE_DIR}/pacman.conf"
fi
sed -i 's/^#*\s*ParallelDownloads\s*=.*/ParallelDownloads = 10/' "${PROFILE_DIR}/pacman.conf"
if ! grep -q '^ParallelDownloads' "${PROFILE_DIR}/pacman.conf"; then
  sed -i '/^\[options\]/a ParallelDownloads = 10' "${PROFILE_DIR}/pacman.conf"
fi
if ! grep -q '^\[multilib\]' "${PROFILE_DIR}/pacman.conf"; then
  printf '\n[multilib]\nInclude = /etc/pacman.d/mirrorlist\n' >> "${PROFILE_DIR}/pacman.conf"
fi

echo "[3/5] Merging packages and custom airootfs..."
grep -vE '^\s*(#|$)' "${WORKSPACE}/archiso/packages.custom.x86_64" >> "${PROFILE_DIR}/packages.x86_64"
sort -u "${PROFILE_DIR}/packages.x86_64" -o "${PROFILE_DIR}/packages.x86_64"

cp -a "${WORKSPACE}/archiso/airootfs/." "${PROFILE_DIR}/airootfs/"
# Ensure custom os-release replaces releng symlink cleanly
cp -f "${WORKSPACE}/archiso/airootfs/etc/os-release" "${PROFILE_DIR}/airootfs/etc/os-release"
if [ -f /usr/share/fontconfig/conf.avail/70-no-bitmaps-except-emoji.conf ]; then
  mkdir -p "${PROFILE_DIR}/airootfs/etc/fonts/conf.d"
  ln -sf /usr/share/fontconfig/conf.avail/70-no-bitmaps-except-emoji.conf \
    "${PROFILE_DIR}/airootfs/etc/fonts/conf.d/70-no-bitmaps-except-emoji.conf"
fi

# Install compiled desktop shell binary and HTML/CSS/JS UI assets
install -Dm755 "${BUILD_DIR}/spinel-shell" "${PROFILE_DIR}/airootfs/usr/local/bin/spinel-shell"
mkdir -p "${PROFILE_DIR}/airootfs/usr/share/spinel-shell/ui"
cp -a "${WORKSPACE}/desktop/ui/." "${PROFILE_DIR}/airootfs/usr/share/spinel-shell/ui/"

# Enable NetworkManager and Bluetooth systemd services
mkdir -p "${PROFILE_DIR}/airootfs/etc/systemd/system/multi-user.target.wants"
mkdir -p "${PROFILE_DIR}/airootfs/etc/systemd/system/bluetooth.target.wants"
ln -sf /usr/lib/systemd/system/NetworkManager.service \
  "${PROFILE_DIR}/airootfs/etc/systemd/system/multi-user.target.wants/NetworkManager.service"
ln -sf /usr/lib/systemd/system/bluetooth.service \
  "${PROFILE_DIR}/airootfs/etc/systemd/system/multi-user.target.wants/bluetooth.service"
ln -sf /usr/lib/systemd/system/bluetooth.service \
  "${PROFILE_DIR}/airootfs/etc/systemd/system/bluetooth.target.wants/bluetooth.service"
ln -sf /usr/lib/systemd/system/power-profiles-daemon.service \
  "${PROFILE_DIR}/airootfs/etc/systemd/system/multi-user.target.wants/power-profiles-daemon.service"

# Disable slow boot-blocking wait-online / mirror-ranking services on Live ISO
ln -sf /dev/null "${PROFILE_DIR}/airootfs/etc/systemd/system/NetworkManager-wait-online.service"
ln -sf /dev/null "${PROFILE_DIR}/airootfs/etc/systemd/system/systemd-networkd-wait-online.service"
ln -sf /dev/null "${PROFILE_DIR}/airootfs/etc/systemd/system/reflector.service"
ln -sf /dev/null "${PROFILE_DIR}/airootfs/etc/systemd/system/choose-mirror.service"

echo "[4/5] Configuring bootloader kernel parameters, branding & profiledef..."
rm -f "${PROFILE_DIR}/syslinux/splash.png"
rm -f "${PROFILE_DIR}"/efiboot/loader/entries/*speech*.conf

if [ -f "${PROFILE_DIR}/efiboot/loader/loader.conf" ]; then
  sed -i 's/^timeout .*/timeout 2/' "${PROFILE_DIR}/efiboot/loader/loader.conf"
fi
for cfg in "${PROFILE_DIR}"/efiboot/loader/entries/*.conf; do
  if [ -f "$cfg" ]; then
    sed -i 's/Arch Linux install medium/SpinelOS Gaming Linux/g; s/Arch Linux/SpinelOS/g' "$cfg"
    sed -i '/^options / s/$/ quiet loglevel=3 nvidia_drm.modeset=1 nvidia_drm.fbdev=1/' "$cfg"
  fi
done
for cfg in "${PROFILE_DIR}"/grub/*.cfg "${PROFILE_DIR}"/syslinux/*.cfg; do
  if [ -f "$cfg" ]; then
    sed -i 's/Arch Linux install medium/SpinelOS Gaming Linux/g; s/Arch Linux live medium/SpinelOS Live/g; s/Arch Linux/SpinelOS/g' "$cfg"
    sed -i 's/^MENU BACKGROUND splash.png/# SpinelOS dark menu/' "$cfg"
    sed -i 's/^TIMEOUT 150/TIMEOUT 30/' "$cfg"
    sed -i 's/^timeout=15/timeout=3/' "$cfg"
    sed -i 's/archisosearchuuid=%ARCHISO_UUID%/archisosearchuuid=%ARCHISO_UUID% quiet loglevel=3 nvidia_drm.modeset=1 nvidia_drm.fbdev=1/g' "$cfg"
  fi
done

cat <<'EOF' >> "${PROFILE_DIR}/profiledef.sh"

iso_name="spinelos"
iso_label="SPINELOS_$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y%m)"
iso_publisher="SpinelOS <https://github.com/AllenZhanga/SpinelOS>"
iso_application="SpinelOS Gaming Linux Live ISO"
airootfs_image_type="squashfs"
airootfs_image_tool_options=('-comp' 'zstd' '-Xcompression-level' '10' '-b' '256K')
file_permissions["/etc/sudoers.d/gamer"]="0:0:0440"
file_permissions["/usr/local/bin/start-spinel-desktop"]="0:0:0755"
file_permissions["/usr/local/bin/spinel-shell"]="0:0:0755"
file_permissions["/usr/local/bin/spinel-install"]="0:0:0755"
file_permissions["/usr/local/bin/spinel-screenshot"]="0:0:0755"
file_permissions["/usr/local/bin/spinel-lock"]="0:0:0755"
EOF

echo "[5/5] Building ISO via mkarchiso..."
mkarchiso -v -w "${WORK_DIR}" -o "${OUT_DIR}" "${PROFILE_DIR}"

echo "Done! ISO generated in ${OUT_DIR}:"
ls -lh "${OUT_DIR}"/*.iso
