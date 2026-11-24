#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

echo "==> Building Arch Linux ISO builder image..."
docker build --platform linux/amd64 -t spinelos-builder -f docker/Dockerfile.builder .

mkdir -p "${ROOT_DIR}/.cache/pacman"

echo "==> Running mkarchiso inside privileged container..."
docker run --rm --privileged --platform linux/amd64 \
  -v "${ROOT_DIR}:/workspace" \
  -v "${ROOT_DIR}/.cache/pacman:/var/cache/pacman/pkg" \
  spinelos-builder \
  bash /workspace/docker/build-inside-container.sh
