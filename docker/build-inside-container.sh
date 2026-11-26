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
find "${PROFILE_DIR}/airootfs" -name '.DS_Store' -delete
# Ensure custom os-release & branding replace releng defaults cleanly
cp -f "${WORKSPACE}/archiso/airootfs/etc/os-release" "${PROFILE_DIR}/airootfs/etc/os-release"
if [ -f "${WORKSPACE}/archiso/airootfs/usr/share/pixmaps/spinelos.png" ]; then
  mkdir -p "${PROFILE_DIR}/airootfs/usr/share/libalpm/hooks"
  cat > "${PROFILE_DIR}/airootfs/usr/share/libalpm/hooks/99-spinelos-logo.hook" <<'EOF'
[Trigger]
Operation = Install
Operation = Upgrade
Type = Package
Target = filesystem

[Action]
Description = Branding archlinux-logo.png with SpinelOS logo...
When = PostTransaction
Exec = /bin/sh -c 'if [ -f /usr/share/pixmaps/spinelos.png ]; then cp -f /usr/share/pixmaps/spinelos.png /usr/share/pixmaps/archlinux-logo.png; fi'
EOF
fi
mkdir -p "${PROFILE_DIR}/airootfs/usr/share/doc/spinelos"
cat > "${PROFILE_DIR}/airootfs/usr/share/doc/spinelos/NOTICE-THIRD-PARTY.txt" <<'EOF'
Spinel OS — Third-Party Software & Open-Source License Notice
============================================================
1. First-Party Components (Spinel Shell, Spinel WebUI, Spinel Terminal, Spinel Files, themes & scripts):
   Licensed under the MIT License (https://github.com/AllenZhanga/SpinelOS).

2. Base OS & Upstream Packages:
   Spinel OS bundles upstream Linux kernel, systemd, glibc, pacman, GRUB, Plymouth,
   labwc, Mesa, PipeWire, Fcitx5, Noto Fonts, and hardware drivers under their
   respective upstream licenses (GPLv2/GPLv3, LGPL, MIT, BSD, OFL-1.1, and vendor
   firmware/driver licenses).

3. GPL Source Correspondence & Package Manifest:
   A complete package version, license, and upstream URL manifest
   (spinelos-packages-licenses.tsv) is generated alongside each ISO build.
   Upstream package sources and PKGBUILDs are archived at:
   https://archive.archlinux.org/packages/
EOF
if [ -f /usr/share/fontconfig/conf.avail/70-no-bitmaps-except-emoji.conf ]; then
  mkdir -p "${PROFILE_DIR}/airootfs/etc/fonts/conf.d"
  ln -sf /usr/share/fontconfig/conf.avail/70-no-bitmaps-except-emoji.conf \
    "${PROFILE_DIR}/airootfs/etc/fonts/conf.d/70-no-bitmaps-except-emoji.conf"
fi

# Install compiled desktop shell, on-demand webui, first-party terminal & HTML/CSS/JS UI assets
install -Dm755 "${BUILD_DIR}/spinel-shell" "${PROFILE_DIR}/airootfs/usr/local/bin/spinel-shell"
install -Dm755 "${BUILD_DIR}/spinel-webui" "${PROFILE_DIR}/airootfs/usr/local/bin/spinel-webui"
install -Dm755 "${BUILD_DIR}/spinel-terminal" "${PROFILE_DIR}/airootfs/usr/local/bin/spinel-terminal"
install -Dm755 "${BUILD_DIR}/spinel-files" "${PROFILE_DIR}/airootfs/usr/local/bin/spinel-files"
install -Dm755 "${BUILD_DIR}/spinel-sandbox" "${PROFILE_DIR}/airootfs/usr/local/bin/spinel-sandbox"
[ -f "${WORKSPACE}/archiso/airootfs/usr/local/bin/spinel-update" ] && \
  chmod 755 "${PROFILE_DIR}/airootfs/usr/local/bin/spinel-update"
mkdir -p "${PROFILE_DIR}/airootfs/etc/spinelos"
if [ -d "${WORKSPACE}/.git" ] && command -v git >/dev/null 2>&1; then
  ISO_VER="0.1.$(git -C "${WORKSPACE}" rev-list --count HEAD 2>/dev/null || echo 0)-$(git -C "${WORKSPACE}" rev-parse --short HEAD 2>/dev/null || echo rolling)"
  printf '%s\n' "${ISO_VER}" > "${PROFILE_DIR}/airootfs/etc/spinelos/version"
fi
mkdir -p "${PROFILE_DIR}/airootfs/usr/share/spinel-shell/ui"
cp -a "${WORKSPACE}/desktop/ui/." "${PROFILE_DIR}/airootfs/usr/share/spinel-shell/ui/"

# Inject Plymouth into Live ISO initramfs hooks (after udev)
if [ -f "${PROFILE_DIR}/airootfs/etc/mkinitcpio.conf.d/archiso.conf" ]; then
  sed -i 's/\budev\b/udev plymouth/' "${PROFILE_DIR}/airootfs/etc/mkinitcpio.conf.d/archiso.conf"
elif [ -f "${PROFILE_DIR}/airootfs/etc/mkinitcpio.conf" ]; then
  sed -i 's/\budev\b/udev plymouth/' "${PROFILE_DIR}/airootfs/etc/mkinitcpio.conf"
fi

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
ln -sf /usr/lib/systemd/system/cups.service \
  "${PROFILE_DIR}/airootfs/etc/systemd/system/multi-user.target.wants/cups.service"

# Remove releng's systemd-networkd/iwd defaults so NetworkManager exclusively manages all NICs/Wi-Fi
rm -f "${PROFILE_DIR}/airootfs/etc/systemd/network"/*.network \
      "${PROFILE_DIR}/airootfs/etc/systemd/system/dbus-org.freedesktop.network1.service" \
      "${PROFILE_DIR}/airootfs/etc/systemd/system/multi-user.target.wants/systemd-networkd.service" \
      "${PROFILE_DIR}/airootfs/etc/systemd/system/sockets.target.wants/systemd-networkd.socket" \
      "${PROFILE_DIR}/airootfs/etc/systemd/system/network-online.target.wants/systemd-networkd-wait-online.service" \
      "${PROFILE_DIR}/airootfs/etc/systemd/system/multi-user.target.wants/iwd.service"

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
    sed -i 's/Arch Linux install medium/Spinel OS/g; s/Arch Linux/Spinel OS/g' "$cfg"
    sed -i '/^options / s/$/ quiet splash vt.global_cursor_default=0 loglevel=3 nvidia_drm.modeset=1 nvidia_drm.fbdev=1/' "$cfg"
  fi
done
if [ -f "${PROFILE_DIR}/efiboot/loader/entries/01-archiso-linux.conf" ]; then
  cat > "${PROFILE_DIR}/efiboot/loader/entries/02-archiso-safe-linux.conf" <<'EOF'
title    Spinel OS Safe Graphics & Debug (%ARCH%, UEFI)
sort-key 02
linux    /%INSTALL_DIR%/boot/%ARCH%/vmlinuz-linux
initrd   /%INSTALL_DIR%/boot/%ARCH%/initramfs-linux.img
options  archisobasedir=%INSTALL_DIR% archisosearchuuid=%ARCHISO_UUID% nomodeset modprobe.blacklist=nvidia,nvidia_drm,nvidia_modeset,nvidia_uvm loglevel=7 plymouth.enable=0 %KERNEL_PARAMS%
EOF
fi

for cfg in "${PROFILE_DIR}"/grub/*.cfg "${PROFILE_DIR}"/syslinux/*.cfg; do
  if [ -f "$cfg" ]; then
    sed -i 's/Arch Linux install medium/Spinel OS/g; s/Arch Linux live medium/Spinel OS Live/g; s/Arch Linux/Spinel OS/g' "$cfg"
    sed -i 's/^MENU BACKGROUND splash.png/# Spinel OS dark menu/' "$cfg"
    sed -i 's/^TIMEOUT 150/TIMEOUT 20/' "$cfg"
    sed -i 's/^timeout=15/timeout=2/' "$cfg"
    sed -i 's/archisosearchuuid=%ARCHISO_UUID%/archisosearchuuid=%ARCHISO_UUID% quiet splash vt.global_cursor_default=0 loglevel=3 nvidia_drm.modeset=1 nvidia_drm.fbdev=1/g' "$cfg"
    sed -i 's/img_loop="${iso_path}"/img_loop="${iso_path}" quiet splash vt.global_cursor_default=0 loglevel=3 nvidia_drm.modeset=1 nvidia_drm.fbdev=1/g' "$cfg"
  fi
done

for gcfg in "${PROFILE_DIR}/grub/grub.cfg" "${PROFILE_DIR}/grub/loopback.cfg"; do
  if [ -f "$gcfg" ]; then
    python3 - "$gcfg" <<'PY' || true
import pathlib, sys
p = pathlib.Path(sys.argv[1])
txt = p.read_text()
if p.name == "loopback.cfg":
    boot_loc = 'img_dev=UUID=${archiso_img_dev_uuid} img_loop="${iso_path}"'
else:
    boot_loc = 'archisosearchuuid=%ARCHISO_UUID%'
safe_block = f"""
menuentry "Spinel OS Safe Graphics & Debug (%ARCH%, UEFI/BIOS)" --class recovery --class gnu-linux --class os --id 'archlinux-safe' {{
    set gfxpayload=keep
    linux /%INSTALL_DIR%/boot/%ARCH%/vmlinuz-linux archisobasedir=%INSTALL_DIR% {boot_loc} nomodeset modprobe.blacklist=nvidia,nvidia_drm,nvidia_modeset,nvidia_uvm loglevel=7 plymouth.enable=0 %KERNEL_PARAMS%
    initrd /%INSTALL_DIR%/boot/%ARCH%/initramfs-linux.img
}}
"""
marker = "--id 'archlinux-accessibility'"
if marker in txt:
    idx = txt.find(marker)
    end = txt.find("}\n", idx)
    if end != -1:
        txt = txt[:end+2] + safe_block + txt[end+2:]
        p.write_text(txt)
PY
  fi
done

if [ -f "${PROFILE_DIR}/syslinux/archiso_sys-linux.cfg" ]; then
  cat >> "${PROFILE_DIR}/syslinux/archiso_sys-linux.cfg" <<'EOF'

LABEL archsafe
TEXT HELP
Boot Spinel OS with safe basic graphics (nomodeset, verbose logs, no NVIDIA/Plymouth).
ENDTEXT
MENU LABEL Spinel OS Safe Graphics & Debug (%ARCH%, BIOS)
LINUX /%INSTALL_DIR%/boot/%ARCH%/vmlinuz-linux
INITRD /%INSTALL_DIR%/boot/%ARCH%/initramfs-linux.img
APPEND archisobasedir=%INSTALL_DIR% archisosearchuuid=%ARCHISO_UUID% nomodeset modprobe.blacklist=nvidia,nvidia_drm,nvidia_modeset,nvidia_uvm loglevel=7 plymouth.enable=0 %KERNEL_PARAMS%
EOF
fi

cat <<'EOF' >> "${PROFILE_DIR}/profiledef.sh"

iso_name="spinelos"
iso_label="SPINELOS_$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y%m)"
iso_publisher="Spinel OS <https://github.com/AllenZhanga/SpinelOS>"
iso_application="Spinel OS Live ISO"
airootfs_image_type="squashfs"
airootfs_image_tool_options=('-comp' 'zstd' '-Xcompression-level' '10' '-b' '256K')
file_permissions["/etc/sudoers.d/gamer"]="0:0:0440"
file_permissions["/usr/local/bin/start-spinel-desktop"]="0:0:0755"
file_permissions["/usr/local/bin/spinel-shell"]="0:0:0755"
file_permissions["/usr/local/bin/spinel-webui"]="0:0:0755"
file_permissions["/usr/local/bin/spinel-terminal"]="0:0:0755"
file_permissions["/usr/local/bin/spinel-files"]="0:0:0755"
file_permissions["/usr/local/bin/spinel-sandbox"]="0:0:0755"
file_permissions["/usr/local/bin/spinel-wine"]="0:0:0755"
file_permissions["/usr/local/bin/spinel-install"]="0:0:0755"
file_permissions["/usr/local/bin/spinel-screenshot"]="0:0:0755"
file_permissions["/usr/local/bin/spinel-lock"]="0:0:0755"
EOF

echo "[5/5] Building ISO via mkarchiso..."
mkarchiso -v -w "${WORK_DIR}" -o "${OUT_DIR}" "${PROFILE_DIR}"

echo "[6/6] Exporting package & GPL/open-source license manifest..."
MANIFEST_TSV="${OUT_DIR}/spinelos-packages-licenses.tsv"
printf "package\tversion\tlicenses\turl\n" > "${MANIFEST_TSV}"
PACMAN_LOCAL_DIR="${WORK_DIR}/x86_64/airootfs/var/lib/pacman/local"
if [ -d "${PACMAN_LOCAL_DIR}" ]; then
  awk '
    FNR == 1 { if (name != "") printf "%s\t%s\t%s\t%s\n", name, ver, lic, url; name=""; ver=""; lic=""; url=""; sec="" }
    /^%NAME%$/ { sec="name"; next }
    /^%VERSION%$/ { sec="ver"; next }
    /^%LICENSE%$/ { sec="lic"; next }
    /^%URL%$/ { sec="url"; next }
    /^%/ { sec=""; next }
    NF == 0 { sec=""; next }
    {
      if (sec == "name") name = $0
      else if (sec == "ver") ver = $0
      else if (sec == "lic") lic = (lic == "" ? $0 : lic ", " $0)
      else if (sec == "url") url = $0
    }
    END { if (name != "") printf "%s\t%s\t%s\t%s\n", name, ver, lic, url }
  ' "${PACMAN_LOCAL_DIR}"/*/desc | sort >> "${MANIFEST_TSV}"
fi
cp -f "${PROFILE_DIR}/airootfs/usr/share/doc/spinelos/NOTICE-THIRD-PARTY.txt" "${OUT_DIR}/NOTICE-THIRD-PARTY.txt"

echo "Done! ISO & compliance manifests generated in ${OUT_DIR}:"
ls -lh "${OUT_DIR}"/*.iso "${MANIFEST_TSV}" "${OUT_DIR}/NOTICE-THIRD-PARTY.txt"
