#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:-dev}"
COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "")
BUILD_DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)

# Default compiler by platform
if [[ "$(uname -s)" == "Linux" ]]; then
    : "${CXX_COMPILER:=g++}"
else
    : "${CXX_COMPILER:=clang++}"
fi

# Validate compiler exists
if ! command -v "$CXX_COMPILER" &> /dev/null; then
    echo "Error: Compiler '$CXX_COMPILER' not found"
    exit 1
fi

# Get compiler version
CXX_VERSION=$("$CXX_COMPILER" --version 2>/dev/null | head -n1 || echo "unknown")

echo "Building alasia ${VERSION} ..."
echo "Compiler: ${CXX_COMPILER} (${CXX_VERSION})"

# Clean build directory if compiler changes to avoid cache issues
if [ -f build/CMakeCache.txt ]; then
    LAST_COMPILER=$(grep "CMAKE_CXX_COMPILER:FILEPATH=" build/CMakeCache.txt 2>/dev/null | cut -d'=' -f2 || echo "")
    if [ "$LAST_COMPILER" != "$(command -v "$CXX_COMPILER" 2>/dev/null || echo "$CXX_COMPILER")" ]; then
        echo "Compiler changed, cleaning build cache..."
        rm -rf build/
    fi
fi

cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="${CXX_COMPILER}" \
    -DAPP_VERSION="${VERSION}" \
    -DAPP_COMMIT="${COMMIT}" \
    -DAPP_BUILD_DATE="${BUILD_DATE}" \
    -DAPP_COMPILER="${CXX_VERSION}"

if command -v nproc &> /dev/null; then
    JOBS=$(nproc)
elif sysctl -n hw.ncpu &> /dev/null 2>&1; then
    JOBS=$(sysctl -n hw.ncpu)
else
    JOBS=1
fi

cmake --build build -j"${JOBS}"

echo "Build successful: $(pwd)/build/alasia"
