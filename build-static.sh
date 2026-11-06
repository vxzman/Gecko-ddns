#!/bin/sh
set -eu

VERSION="${1:-2.0}"
COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "")
BUILD_DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)

echo "Building gecko-ddns ${VERSION} (Static musl build via Alpine/Docker)..."

# Ensure output directory files belong to current user
USER_ID=$(id -u)
GROUP_ID=$(id -g)

docker run --rm -v "$(pwd)":/src -w /src alpine:latest sh -c "
  apk add --no-cache \
    g++ cmake make git linux-headers pkgconf \
    curl-dev curl-static \
    openssl-dev openssl-libs-static \
    brotli-static zlib-static nghttp2-static libpsl-static \
    zstd-static c-ares-dev libidn2-static libunistring-static \
    nlohmann-json
  rm -rf build-static
  cmake -B build-static \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_STATIC=ON \
    -DAPP_VERSION='${VERSION}' \
    -DAPP_COMMIT='${COMMIT}' \
    -DAPP_BUILD_DATE='${BUILD_DATE}'
  cmake --build build-static -j\$(nproc)
  strip -s build-static/gecko-ddns
  chown -R ${USER_ID}:${GROUP_ID} build-static
"

echo "======================================================="
echo "Static build successful: $(pwd)/build-static/gecko-ddns"
file "$(pwd)/build-static/gecko-ddns"
echo "======================================================="
