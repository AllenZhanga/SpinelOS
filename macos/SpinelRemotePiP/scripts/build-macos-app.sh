#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
PKG_DIR="${ROOT_DIR}/macos/SpinelRemotePiP"
OUT_APP="${ROOT_DIR}/out/SpinelRemotePiP.app"

swift run --package-path "${PKG_DIR}" --build-system native SpinelRemoteSelfTest
swift build --package-path "${PKG_DIR}" --build-system native -c release --product SpinelRemotePiP

BIN_DIR="$(swift build --package-path "${PKG_DIR}" --build-system native -c release --show-bin-path)"
BIN_PATH="${BIN_DIR}/SpinelRemotePiP"

rm -rf "${OUT_APP}"
mkdir -p "${OUT_APP}/Contents/MacOS" "${OUT_APP}/Contents/Resources"
cp "${BIN_PATH}" "${OUT_APP}/Contents/MacOS/SpinelRemotePiP"
chmod 755 "${OUT_APP}/Contents/MacOS/SpinelRemotePiP"

cat > "${OUT_APP}/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>zh_CN</string>
  <key>CFBundleExecutable</key>
  <string>SpinelRemotePiP</string>
  <key>CFBundleIdentifier</key>
  <string>org.spinelos.SpinelRemotePiP</string>
  <key>CFBundleName</key>
  <string>SpinelRemotePiP</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>0.1.0</string>
  <key>CFBundleVersion</key>
  <string>1</string>
  <key>LSMinimumSystemVersion</key>
  <string>14.0</string>
  <key>LSUIElement</key>
  <true/>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>
EOF

if command -v codesign >/dev/null 2>&1; then
  codesign --force --deep --sign - "${OUT_APP}" || true
fi

echo "==> macOS app bundle created: ${OUT_APP}"
