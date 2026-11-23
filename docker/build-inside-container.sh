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

# Ensure [multilib] is enabled inside the ISO's pacman.conf
if ! grep -q '^\[multilib\]' "${PROFILE_DIR}/pacman.conf"; then
  printf '\n[multilib]\nInclude = /etc/pacman.d/mirrorlist\n' >> "${PROFILE_DIR}/pacman.conf"
fi

echo "[3/5] Merging packages and custom airootfs..."
grep -vE '^\s*(#|$)' "${WORKSPACE}/archiso/packages.custom.x86_64" >> "${PROFILE_DIR}/packages.x86_64"
sort -u "${PROFILE_DIR}/packages.x86_64" -o "${PROFILE_DIR}/packages.x86_64"

cp -a "${WORKSPACE}/archiso/airootfs/." "${PROFILE_DIR}/airootfs/"

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

echo "[4/5] Configuring bootloader kernel parameters & profiledef..."
for cfg in "${PROFILE_DIR}"/efiboot/loader/entries/*.conf; do
  [ -f "$cfg" ] && sed -i '/^options / s/$/ nvidia_drm.modeset=1 nvidia_drm.fbdev=1/' "$cfg"
done
for cfg in "${PROFILE_DIR}"/grub/*.cfg "${PROFILE_DIR}"/syslinux/*.cfg; do
  [ -f "$cfg" ] && sed -i 's/archisosearchuuid=%ARCHISO_UUID%/archisosearchuuid=%ARCHISO_UUID% nvidia_drm.modeset=1 nvidia_drm.fbdev=1/g' "$cfg"
done

cat <<'EOF' >> "${PROFILE_DIR}/profiledef.sh"

iso_name="spinelos"
iso_label="SPINELOS_$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y%m)"
iso_publisher="SpinelOS <https://github.com/AllenZhanga/SpinelOS>"
iso_application="SpinelOS Gaming Linux Live ISO"
file_permissions["/etc/sudoers.d/gamer"]="0:0:0440"
file_permissions["/usr/local/bin/start-spinel-desktop"]="0:0:0755"
file_permissions["/usr/local/bin/spinel-shell"]="0:0:0755"
file_permissions["/usr/local/bin/spinel-install"]="0:0:0755"
EOF

echo "[5/5] Building ISO via mkarchiso..."
mkarchiso -v -w "${WORK_DIR}" -o "${OUT_DIR}" "${PROFILE_DIR}"

echo "Done! ISO generated in ${OUT_DIR}:"
ls -lh "${OUT_DIR}"/*.iso
