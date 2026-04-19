#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:-dev}"
COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "")
BUILD_DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)

echo "Building alasia ${VERSION} ..."

cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DAPP_VERSION="${VERSION}" \
    -DAPP_COMMIT="${COMMIT}" \
    -DAPP_BUILD_DATE="${BUILD_DATE}"

cmake --build build -j"$(nproc)"

echo "Build successful: $(pwd)/build/alasia"
