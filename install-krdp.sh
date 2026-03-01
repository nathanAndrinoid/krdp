#!/usr/bin/env bash
# Build and install the KRdp module.
# Usage: ./install-krdp.sh [--prefix DIR] [--no-install]
#
# For system-wide install (default prefix), run with: sudo ./install-krdp.sh
# For user install: ./install-krdp.sh --prefix "$HOME/.local"

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
PREFIX=""
DO_INSTALL=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)
            PREFIX="$2"
            shift 2
            ;;
        --no-install)
            DO_INSTALL=0
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--prefix DIR] [--no-install]"
            exit 1
            ;;
    esac
done

cd "$SCRIPT_DIR"

if ! command -v cmake &>/dev/null; then
    echo "Error: cmake is required but not found." >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=RelWithDebInfo)
if [[ -n "$PREFIX" ]]; then
    CMAKE_ARGS+=(-DCMAKE_INSTALL_PREFIX="$PREFIX")
fi

if command -v ninja &>/dev/null; then
    CMAKE_ARGS+=(-G Ninja)
    echo "Configuring with Ninja..."
else
    echo "Configuring with Unix Makefiles..."
fi

cmake "${CMAKE_ARGS[@]}" ..

echo "Building..."
if command -v ninja &>/dev/null; then
    ninja
else
    make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)"
fi

if [[ $DO_INSTALL -eq 1 ]]; then
    echo "Installing..."
    if command -v ninja &>/dev/null; then
        ninja install
    else
        make install
    fi
    echo "KRdp has been installed successfully."
else
    echo "Build finished (install skipped)."
fi
