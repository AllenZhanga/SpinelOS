#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT_DIR}/scripts/manifest.env"

QUICK=0
for arg in "$@"; do
  case "${arg}" in
    --quick) QUICK=1 ;;
    -h|--help)
      echo "Usage: ./scripts/check.sh [--quick]"
      echo "  --quick   Skip Docker C++20 build & ctest (run JS, bash, manifest, and Go relay checks only)"
      exit 0
      ;;
    *)
      echo "Unknown option: ${arg}" >&2
      exit 1
      ;;
  esac
done

echo "==> [1/5] Checking UI JS syntax & unit tests..."
if command -v node >/dev/null 2>&1; then
  for f in "${ROOT_DIR}"/desktop/ui/js/*.js; do
    node --check "${f}"
  done
  if compgen -G "${ROOT_DIR}/desktop/ui/tests/*.test.mjs" >/dev/null; then
    node --test "${ROOT_DIR}"/desktop/ui/tests/*.test.mjs
  fi
else
  echo "ERROR: node is required for JS syntax and unit validation" >&2
  exit 1
fi

echo "==> [2/5] Checking shell script syntax..."
for f in \
  "${ROOT_DIR}"/scripts/*.sh \
  "${ROOT_DIR}/docker/build-inside-container.sh" \
  "${ROOT_DIR}/archiso/airootfs/usr/local/bin/start-spinel-desktop" \
  "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-install" \
  "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-update" \
  "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-wine" \
  "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-lock" \
  "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-screenshot"; do
  bash -n "${f}"
done

echo "==> [3/5] Verifying multi-target manifest consistency..."
consistency_errors=0
require_token() {
  local file="$1"
  local token="$2"
  if ! grep -Fq "${token}" "${file}"; then
    echo "MISSING '${token}' in ${file#${ROOT_DIR}/}" >&2
    consistency_errors=1
  fi
}

for bin in "${SPINEL_NATIVE_BINS[@]}"; do
  require_token "${ROOT_DIR}/desktop/CMakeLists.txt" "add_executable(${bin}"
  require_token "${ROOT_DIR}/docker/build-inside-container.sh" "${bin}"
  require_token "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-install" "${bin}"
  require_token "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-update" "${bin}"
done

for pkg in "${SPINEL_REQUIRED_IME_PKGS[@]}"; do
  require_token "${ROOT_DIR}/archiso/packages.custom.x86_64" "${pkg}"
  require_token "${ROOT_DIR}/archiso/airootfs/usr/local/bin/spinel-update" "${pkg}"
  require_token "${ROOT_DIR}/scripts/dev-deploy.sh" "${pkg}"
done

if [[ "${consistency_errors}" -ne 0 ]]; then
  echo "ERROR: Multi-end manifest consistency check failed!" >&2
  exit 1
fi

echo "==> [4/5] Running Go relay tests (server/spinel-relay)..."
if command -v go >/dev/null 2>&1; then
  (cd "${ROOT_DIR}/server/spinel-relay" && go test ./...)
else
  echo "SKIP: go compiler not found in host PATH"
fi

if [[ "${QUICK}" -eq 1 ]]; then
  echo "==> [5/5] Quick mode enabled; skipping Docker C++20 build + ctest."
  exit 0
fi

echo "==> [5/5] Running Docker C++20 Release build + CTest suites..."
docker run --rm --platform linux/amd64 \
  -v "${ROOT_DIR}:/workspace" \
  -w /workspace \
  spinelos-builder \
  bash -c "cmake -S desktop -B build/linux-amd64 -DCMAKE_BUILD_TYPE=Release && cmake --build build/linux-amd64 -j4 && ctest --test-dir build/linux-amd64 --output-on-failure -V"

echo "All SpinelOS checks passed cleanly!"
