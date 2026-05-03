#!/usr/bin/env bash
set -euo pipefail

# Check for --install-deps flag
if [[ "${1:-}" == "--install-deps" ]]; then
    echo "Installing system dependencies..."
    
    if command -v apt-get &> /dev/null; then
        sudo apt-get update
        sudo apt-get install -y cmake build-essential libcurl4-openssl-dev libssl-dev
    elif command -v dnf &> /dev/null; then
        sudo dnf install -y cmake gcc-c++ libcurl-devel openssl-devel
    elif command -v yum &> /dev/null; then
        sudo yum install -y cmake gcc-c++ libcurl-devel openssl-devel
    elif command -v pacman &> /dev/null; then
        sudo pacman -S --noconfirm cmake base-devel curl openssl
    elif command -v brew &> /dev/null; then
        brew install cmake curl openssl
    else
        echo "Unsupported package manager. Please install dependencies manually."
        exit 1
    fi
    
    echo "Dependencies installed successfully!"
    exit 0
fi

VERSION="${1:-dev}"
COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "")
BUILD_DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)

# Compiler selection (defaults to gcc/g++, can be overridden)
: "${CXX_COMPILER:=g++}"

# Function to check if a compiler exists
check_compiler() {
    local compiler="$1"
    local display_name="$2"

    if ! command -v "$compiler" &> /dev/null; then
        echo "Warning: $display_name ($compiler) not found"
        read -rp "Switch back to default compiler (g++)? [Y/n] " -n 1
        echo
        if [[ $REPLY =~ ^[Nn]$ ]]; then
            echo "Build cancelled."
            exit 1
        else
            if command -v g++ &> /dev/null; then
                echo "Switching to g++..."
                CXX_COMPILER="g++"
            else
                echo "Error: Default compiler (g++) also not found!"
                exit 1
            fi
        fi
    fi
}

# Check compiler availability
check_compiler "$CXX_COMPILER" "${CXX_COMPILER}"

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

cmake --build build -j"$(nproc)"

echo "Build successful: $(pwd)/build/alasia"
