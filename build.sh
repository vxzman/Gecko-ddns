#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:-dev}"
COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "")
BUILD_DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)

# Compiler selection (defaults to gcc/g++, can be overridden)
: "${CXX_COMPILER:=g++}"
: "${C_COMPILER:=gcc}"

# Function to check if a compiler exists
check_compiler() {
    local compiler="$1"
    local display_name="$2"
    
    if ! command -v "$compiler" &> /dev/null; then
        echo "Warning: $display_name ($compiler) not found"
        read -rp "Switch back to default compiler (gcc/g++)? [Y/n] " -n 1
        echo
        if [[ $REPLY =~ ^[Nn]$ ]]; then
            echo "Build cancelled."
            exit 1
        else
            if command -v g++ &> /dev/null && command -v gcc &> /dev/null; then
                echo "Switching to gcc/g++..."
                CXX_COMPILER="g++"
                C_COMPILER="gcc"
            else
                echo "Error: Default compiler (gcc/g++) also not found!"
                exit 1
            fi
        fi
    fi
}

# If clang++ is requested or auto-detected, check availability
if [[ "${CXX_COMPILER}" == "clang++" ]]; then
    check_compiler "clang++" "Clang++"
    check_compiler "clang" "Clang"
elif [[ "${CXX_COMPILER}" == "g++" ]]; then
    check_compiler "g++" "G++"
    check_compiler "gcc" "GCC"
fi

# Resolve full path for display
CXX_FULL_PATH=$(command -v "$CXX_COMPILER" 2>/dev/null || echo "$CXX_COMPILER")
CXX_VERSION=$("$CXX_COMPILER" --version 2>/dev/null | head -n1 || echo "unknown")

echo "Building alasia ${VERSION} ..."
echo "Compiler: ${CXX_COMPILER} (${CXX_VERSION})"

cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="${CXX_COMPILER}" \
    -DCMAKE_C_COMPILER="${C_COMPILER}" \
    -DAPP_VERSION="${VERSION}" \
    -DAPP_COMMIT="${COMMIT}" \
    -DAPP_BUILD_DATE="${BUILD_DATE}" \
    -DAPP_COMPILER="${CXX_VERSION}"

cmake --build build -j"$(nproc)"

echo "Build successful: $(pwd)/build/alasia"
