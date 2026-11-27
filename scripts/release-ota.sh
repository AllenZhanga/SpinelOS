#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

if [[ -f "${ROOT_DIR}/.env.local" ]]; then
  # shellcheck disable=SC1091
  source "${ROOT_DIR}/.env.local"
fi

OTA_HOST="${SPINEL_OTA_HOST:-}"
OTA_REMOTE_DIR="${SPINEL_OTA_REMOTE_DIR:-/var/lib/spinel-relay/ota}"
KEY_DIR="${ROOT_DIR}/.secrets"
PRIV_KEY="${KEY_DIR}/ota-signing-key.pem"
PUB_KEY="${ROOT_DIR}/archiso/airootfs/etc/spinelos/ota-pubkey.pem"
OTA_LOCAL_DIR="${ROOT_DIR}/out/ota"

DO_UPLOAD=1
RUN_TESTS=1
NOTES=""
VERSION=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="${2:-}"; shift 2 ;;
    --notes) NOTES="${2:-}"; shift 2 ;;
    --no-upload) DO_UPLOAD=0; shift ;;
    --skip-tests) RUN_TESTS=0; shift ;;
    --host) OTA_HOST="${2:-}"; shift 2 ;;
    --remote-dir) OTA_REMOTE_DIR="${2:-}"; shift 2 ;;
    -h|--help)
      cat <<'EOF'
Usage: ./scripts/release-ota.sh [--version <ver>] [--notes <text>] [--host user@server] [--no-upload] [--skip-tests]
Builds Release binaries inside spinelos-builder, packages a signed OTA bundle (with full configs/themes),
and uploads to SPINEL_OTA_HOST (set in gitignored .env.local or --host).
EOF
      exit 0
      ;;
    *)
      echo "[ERROR] Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

if [[ "${DO_UPLOAD}" -eq 1 && -z "${OTA_HOST}" ]]; then
  echo "[ERROR] SPINEL_OTA_HOST is not set. Add SPINEL_OTA_HOST=user@server to .env.local (gitignored) or pass --host." >&2
  exit 1
fi

GIT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo 'nogit')"
GIT_COUNT="$(git rev-list --count HEAD 2>/dev/null || echo '0')"
if [[ -z "${VERSION}" ]]; then
  VERSION="0.1.${GIT_COUNT}-${GIT_SHA}"
fi
if [[ -z "${NOTES}" ]]; then
  NOTES="$(git log -1 --pretty=%s 2>/dev/null || echo 'SpinelOS OTA Release')"
fi
BUILT_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

mkdir -p "${KEY_DIR}" "$(dirname "${PUB_KEY}")" "${OTA_LOCAL_DIR}"
if [[ ! -f "${PRIV_KEY}" ]]; then
  echo "==> Generating ECDSA P-256 OTA signing keypair in .secrets/..."
  openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "${PRIV_KEY}"
  chmod 600 "${PRIV_KEY}"
fi
openssl pkey -in "${PRIV_KEY}" -pubout -out "${PUB_KEY}" >/dev/null 2>&1

echo "==> [1/4] Building Release binaries inside spinelos-builder..."
docker run --rm --platform linux/amd64 \
  -v "${ROOT_DIR}:/workspace" \
  -w /workspace \
  spinelos-builder \
  bash -c "cmake -S desktop -B build/linux-amd64 -DCMAKE_BUILD_TYPE=Release && cmake --build build/linux-amd64 -j\$(nproc) $( [[ ${RUN_TESTS} -eq 1 ]] && echo '&& ctest --test-dir build/linux-amd64 --output-on-failure' )"

BUNDLE_NAME="spinelos-ota-${VERSION}.tar.zst"
STAGE_REL="out/.ota-stage"
STAGE_ABS="${ROOT_DIR}/${STAGE_REL}"
rm -rf "${STAGE_ABS}" "${OTA_LOCAL_DIR}"
mkdir -p "${STAGE_ABS}/bin" "${STAGE_ABS}/assets" "${STAGE_ABS}/ui" "${STAGE_ABS}/etc/spinelos" "${STAGE_ABS}/full" "${OTA_LOCAL_DIR}"

echo "==> [2/4] Staging binaries, UI assets & full system configs..."
for bin in spinel-shell spinel-webui spinel-terminal spinel-files spinel-sandbox spinel-agent-ctl spinel-os-mcp; do
  cp -f "${ROOT_DIR}/build/linux-amd64/${bin}" "${STAGE_ABS}/bin/${bin}"
done
cp -f "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-wine" "${STAGE_ABS}/bin/spinel-wine"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-update" "${STAGE_ABS}/bin/spinel-update"

cp -f "${ROOT_DIR}/archiso/airootfs/usr/share/applications/org.spinelos.files.desktop" "${STAGE_ABS}/assets/"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/share/applications/org.spinelos.Store.desktop" "${STAGE_ABS}/assets/"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/share/applications/org.spinelos.Games.desktop" "${STAGE_ABS}/assets/"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/share/applications/org.spinelos.WindowsApps.desktop" "${STAGE_ABS}/assets/"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/share/applications/org.spinelos.WineOpen.desktop" "${STAGE_ABS}/assets/"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/share/icons/hicolor/scalable/apps/spinel-files.svg" "${STAGE_ABS}/assets/"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/share/icons/hicolor/scalable/apps/org.spinelos.Store.svg" "${STAGE_ABS}/assets/"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/share/icons/hicolor/scalable/apps/org.spinelos.Games.svg" "${STAGE_ABS}/assets/"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/share/icons/hicolor/scalable/apps/org.spinelos.WindowsApps.svg" "${STAGE_ABS}/assets/"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/share/dbus-1/services/org.spinelos.FileManager1.service" "${STAGE_ABS}/assets/"
cp -f "${ROOT_DIR}/archiso/airootfs/etc/xdg/mimeapps.list" "${STAGE_ABS}/assets/"

cp -a "${ROOT_DIR}/desktop/ui/." "${STAGE_ABS}/ui/"
cp -f "${ROOT_DIR}/archiso/airootfs/etc/spinelos/ota.conf" "${STAGE_ABS}/etc/spinelos/"
cp -f "${PUB_KEY}" "${STAGE_ABS}/etc/spinelos/ota-pubkey.pem"
printf '%s\n' "${VERSION}" > "${STAGE_ABS}/etc/spinelos/bundle-version"

mkdir -p "${STAGE_ABS}/full/labwc" "${STAGE_ABS}/full/portals" "${STAGE_ABS}/full/SpinelDark" \
  "${STAGE_ABS}/full/SpinelLight" "${STAGE_ABS}/full/fcitx5-themes" \
  "${STAGE_ABS}/full/plymouth-theme" "${STAGE_ABS}/full/grub-theme"
cp -a "${ROOT_DIR}/archiso/airootfs/etc/xdg/labwc/." "${STAGE_ABS}/full/labwc/"
cp -a "${ROOT_DIR}/archiso/airootfs/etc/xdg/xdg-desktop-portal/." "${STAGE_ABS}/full/portals/"
cp -a "${ROOT_DIR}/archiso/airootfs/usr/share/themes/SpinelDark/." "${STAGE_ABS}/full/SpinelDark/"
cp -a "${ROOT_DIR}/archiso/airootfs/usr/share/themes/SpinelLight/." "${STAGE_ABS}/full/SpinelLight/"
cp -a "${ROOT_DIR}/archiso/airootfs/usr/share/fcitx5/themes/." "${STAGE_ABS}/full/fcitx5-themes/"
cp -a "${ROOT_DIR}/archiso/airootfs/usr/share/plymouth/themes/spinelos/." "${STAGE_ABS}/full/plymouth-theme/"
cp -a "${ROOT_DIR}/archiso/airootfs/usr/share/grub/themes/spinelos/." "${STAGE_ABS}/full/grub-theme/"
cp -f "${ROOT_DIR}/archiso/airootfs/etc/plymouth/plymouthd.conf" "${STAGE_ABS}/full/"
cp -f "${ROOT_DIR}/archiso/airootfs/etc/security/faillock.conf" "${STAGE_ABS}/full/"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/share/pixmaps/spinelos.png" "${STAGE_ABS}/full/"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/share/icons/hicolor/scalable/apps/spinelos.svg" "${STAGE_ABS}/full/"
cp -f "${ROOT_DIR}/archiso/airootfs/etc/systemd/zram-generator.conf" "${STAGE_ABS}/full/"
cp -f "${ROOT_DIR}/archiso/airootfs/etc/udev/rules.d/90-spinel-backlight.rules" "${STAGE_ABS}/full/"
cp -f "${ROOT_DIR}/archiso/airootfs/etc/polkit-1/rules.d/49-spinelos-udisks.rules" "${STAGE_ABS}/full/"
cp -f "${ROOT_DIR}/archiso/airootfs/etc/fonts/conf.d/75-spinel-fonts.conf" "${STAGE_ABS}/full/"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/local/bin/start-spinel-desktop" "${STAGE_ABS}/full/"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-install" "${STAGE_ABS}/full/"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-screenshot" "${STAGE_ABS}/full/"
cp -f "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-lock" "${STAGE_ABS}/full/"
find "${STAGE_ABS}" -name '.DS_Store' -delete

echo "==> [3/4] Stripping ELF binaries and creating ${BUNDLE_NAME}..."
docker run --rm --platform linux/amd64 \
  -v "${ROOT_DIR}:/workspace" \
  -w /workspace \
  spinelos-builder \
  bash -c "strip --strip-unneeded ${STAGE_REL}/bin/spinel-shell ${STAGE_REL}/bin/spinel-webui ${STAGE_REL}/bin/spinel-terminal ${STAGE_REL}/bin/spinel-files ${STAGE_REL}/bin/spinel-sandbox ${STAGE_REL}/bin/spinel-agent-ctl ${STAGE_REL}/bin/spinel-os-mcp && tar --zstd -cf out/ota/${BUNDLE_NAME} -C ${STAGE_REL} ."

rm -rf "${STAGE_ABS}"

BUNDLE_PATH="${OTA_LOCAL_DIR}/${BUNDLE_NAME}"
BUNDLE_SHA="$(shasum -a 256 "${BUNDLE_PATH}" | awk '{print $1}')"
BUNDLE_SIZE="$(wc -c < "${BUNDLE_PATH}" | tr -d ' ')"

python3 - "${OTA_LOCAL_DIR}/latest.json" "${VERSION}" "${GIT_SHA}" "${BUILT_AT}" "${BUNDLE_NAME}" "${BUNDLE_SHA}" "${BUNDLE_SIZE}" "${NOTES}" <<'PY'
import json, sys
out, ver, sha_git, built, bname, bsha, bsz, notes = sys.argv[1:]
data = {
    "version": ver,
    "commit": sha_git,
    "built_at": built,
    "bundle": bname,
    "sha256": bsha,
    "size_bytes": int(bsz),
    "notes": notes,
}
with open(out, "w", encoding="utf-8") as f:
    json.dump(data, f, ensure_ascii=False, indent=2)
    f.write("\n")
PY

openssl dgst -sha256 -sign "${PRIV_KEY}" -out "${OTA_LOCAL_DIR}/latest.json.sig" "${OTA_LOCAL_DIR}/latest.json"

cat > "${OTA_LOCAL_DIR}/bootstrap.sh" <<'BOOT_EOF'
#!/usr/bin/env bash
set -euo pipefail
BASE_URL="${1:-}"
ACCOUNT="${2:-}"
PASSWORD="${3:-}"
MODE="${4:---full}"
if [[ "${EUID}" -ne 0 || -z "${BASE_URL}" || -z "${ACCOUNT}" || -z "${PASSWORD}" ]]; then
  echo "Usage: curl -fsSLk -u <acc>:<pwd> <url>/bootstrap.sh | sudo bash -s -- <url> <acc> <pwd> [--full]" >&2
  exit 1
fi
BASE_URL="${BASE_URL%/}"
TMP_DIR="$(mktemp -d /tmp/spinel-bootstrap-XXXXXX)"
trap 'rm -rf "${TMP_DIR}"' EXIT
curl -fsSLk -u "${ACCOUNT}:${PASSWORD}" -o "${TMP_DIR}/latest.json" "${BASE_URL}/latest.json"
if command -v python3 >/dev/null 2>&1; then
  BUNDLE_NAME="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["bundle"])' "${TMP_DIR}/latest.json")"
  BUNDLE_SHA="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["sha256"])' "${TMP_DIR}/latest.json")"
else
  BUNDLE_NAME="$(sed -n 's/.*"bundle"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${TMP_DIR}/latest.json" | head -n1)"
  BUNDLE_SHA="$(sed -n 's/.*"sha256"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${TMP_DIR}/latest.json" | head -n1)"
fi
echo "==> Bootstrapping SpinelOS OTA bundle: ${BUNDLE_NAME}"
curl -fSLk --progress-bar -u "${ACCOUNT}:${PASSWORD}" -o "${TMP_DIR}/${BUNDLE_NAME}" "${BASE_URL}/${BUNDLE_NAME}"
GOT_SHA="$(sha256sum "${TMP_DIR}/${BUNDLE_NAME}" | awk '{print $1}')"
if [[ "${GOT_SHA}" != "${BUNDLE_SHA}" ]]; then
  echo "[ERROR] SHA256 mismatch!" >&2
  exit 1
fi
mkdir -p "${TMP_DIR}/extract"
tar --zstd -xf "${TMP_DIR}/${BUNDLE_NAME}" -C "${TMP_DIR}/extract"
install -Dm755 "${TMP_DIR}/extract/bin/spinel-update" /usr/local/bin/spinel-update
install -Dm644 "${TMP_DIR}/extract/etc/spinelos/ota.conf" /etc/spinelos/ota.conf
install -Dm644 "${TMP_DIR}/extract/etc/spinelos/ota-pubkey.pem" /etc/spinelos/ota-pubkey.pem
exec /usr/local/bin/spinel-update offline "${TMP_DIR}/${BUNDLE_NAME}" "${MODE}" --yes --sha256 "${BUNDLE_SHA}"
BOOT_EOF
chmod +x "${OTA_LOCAL_DIR}/bootstrap.sh"

echo "==> Bundle ready: ${BUNDLE_PATH} ($(awk -v b="${BUNDLE_SIZE}" 'BEGIN{printf "%.2f MB", b/1048576}'))"

if [[ "${DO_UPLOAD}" -eq 1 ]]; then
  echo "==> [4/4] Uploading OTA release to ${OTA_HOST}:${OTA_REMOTE_DIR} ..."
  ssh "${OTA_HOST}" "sudo mkdir -p '${OTA_REMOTE_DIR}' && sudo chmod 755 '${OTA_REMOTE_DIR}'"
  rsync -avz --delete --rsync-path="sudo rsync" "${OTA_LOCAL_DIR}/" "${OTA_HOST}:${OTA_REMOTE_DIR}/"
  echo "==> Published OTA release ${VERSION} cleanly!"
else
  echo "==> [4/4] Skipped upload (--no-upload). Files are in ${OTA_LOCAL_DIR}/"
fi
