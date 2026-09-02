#!/usr/bin/env bash

# Copyright (c) 2025 Junocash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

# Creates a DMG disk image for macOS distribution
# This script builds the necessary tools (libdmg-hfsplus) if not available

set -eu

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
print_error() { echo -e "${RED}[ERROR]${NC} $1"; }
print_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }

# Default values
APP_NAME="Junocash"
DMG_NAME=""
VOLUME_NAME="Junocash"
SOURCE_DIR=""
OUTPUT_DIR="."
TOOLS_DIR="${REPO_ROOT}/build-tools"
VERSION=""

usage() {
    cat <<EOF
Usage: $0 [OPTIONS] --source <directory>

Create a DMG disk image for macOS distribution.

OPTIONS:
    -s, --source DIR        Source directory containing binaries (required)
    -o, --output DIR        Output directory for DMG (default: current dir)
    -n, --name NAME         DMG filename without extension (default: auto-generated)
    -v, --version VER       Version string for DMG name
    -V, --volume NAME       Volume name shown in Finder (default: "Junocash")
    -h, --help              Show this help

EXAMPLE:
    $0 --source ./release/junocash-1.0.0-macos-arm64 --version 1.0.0 --output ./release
EOF
    exit 0
}

# Parse arguments
parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            -s|--source)
                SOURCE_DIR="$2"
                shift 2
                ;;
            -o|--output)
                OUTPUT_DIR="$2"
                shift 2
                ;;
            -n|--name)
                DMG_NAME="$2"
                shift 2
                ;;
            -v|--version)
                VERSION="$2"
                shift 2
                ;;
            -V|--volume)
                VOLUME_NAME="$2"
                shift 2
                ;;
            -h|--help)
                usage
                ;;
            *)
                print_error "Unknown option: $1"
                exit 1
                ;;
        esac
    done

    if [ -z "$SOURCE_DIR" ]; then
        print_error "Source directory is required. Use --source <dir>"
        exit 1
    fi

    if [ ! -d "$SOURCE_DIR" ]; then
        print_error "Source directory does not exist: $SOURCE_DIR"
        exit 1
    fi
}

# Check/install system dependencies
check_dependencies() {
    print_info "Checking dependencies..."

    # Check for genisoimage
    if ! command -v genisoimage >/dev/null 2>&1; then
        print_warn "genisoimage not found. Attempting to install..."
        if command -v apt-get >/dev/null 2>&1; then
            sudo apt-get update && sudo apt-get install -y genisoimage
        else
            print_error "Please install genisoimage manually"
            exit 1
        fi
    fi

    # Check for cmake (needed to build libdmg-hfsplus)
    if ! command -v cmake >/dev/null 2>&1; then
        print_warn "cmake not found. Attempting to install..."
        if command -v apt-get >/dev/null 2>&1; then
            sudo apt-get update && sudo apt-get install -y cmake
        else
            print_error "Please install cmake manually"
            exit 1
        fi
    fi

    # Check for zlib development files
    if [ ! -f /usr/include/zlib.h ] && [ ! -f /usr/local/include/zlib.h ]; then
        print_warn "zlib development files not found. Attempting to install..."
        if command -v apt-get >/dev/null 2>&1; then
            sudo apt-get update && sudo apt-get install -y zlib1g-dev
        fi
    fi

    print_success "Dependencies OK"
}

# Build libdmg-hfsplus if not available
build_dmg_tool() {
    local DMG_TOOL="${TOOLS_DIR}/bin/dmg"

    if [ -x "$DMG_TOOL" ]; then
        print_info "DMG tool already built"
        return 0
    fi

    print_info "Building libdmg-hfsplus..."

    mkdir -p "${TOOLS_DIR}/src"
    cd "${TOOLS_DIR}/src"

    # Clone libdmg-hfsplus if not present
    if [ ! -d "libdmg-hfsplus" ]; then
        git clone https://github.com/fanquake/libdmg-hfsplus.git
    fi

    cd libdmg-hfsplus

    # Build
    mkdir -p build
    cd build
    cmake .. -DCMAKE_INSTALL_PREFIX="${TOOLS_DIR}"
    make -j$(nproc 2>/dev/null || echo 4)

    # Install locally
    mkdir -p "${TOOLS_DIR}/bin"
    cp dmg/dmg "${TOOLS_DIR}/bin/"
    # hfsplus is optional - only copy if built
    [ -f hfs/hfsplus ] && cp hfs/hfsplus "${TOOLS_DIR}/bin/" || true

    print_success "DMG tool built successfully"
}

# Create the DMG
create_dmg() {
    local DMG_TOOL="${TOOLS_DIR}/bin/dmg"
    local TEMP_DIR=$(mktemp -d)
    local ISO_FILE="${TEMP_DIR}/${APP_NAME}.iso"
    local DMG_TEMP="${TEMP_DIR}/${APP_NAME}.dmg"

    # Auto-generate DMG name if not specified
    if [ -z "$DMG_NAME" ]; then
        if [ -n "$VERSION" ]; then
            DMG_NAME="junocash-${VERSION}-macos"
        else
            DMG_NAME="junocash-macos"
        fi
    fi

    local OUTPUT_DMG="${OUTPUT_DIR}/${DMG_NAME}.dmg"

    print_info "Creating DMG: ${OUTPUT_DMG}"

    # Create staging directory with proper structure
    local STAGING="${TEMP_DIR}/staging"
    mkdir -p "${STAGING}"

    # Copy binaries to staging
    if [ -d "${SOURCE_DIR}/bin" ]; then
        cp -r "${SOURCE_DIR}/bin" "${STAGING}/"
    else
        # If source doesn't have bin subdir, copy everything
        cp -r "${SOURCE_DIR}"/* "${STAGING}/" 2>/dev/null || true
    fi

    # Copy docs if present
    for doc in README.md README COPYING LICENSE; do
        if [ -f "${SOURCE_DIR}/${doc}" ]; then
            cp "${SOURCE_DIR}/${doc}" "${STAGING}/"
        elif [ -f "${REPO_ROOT}/${doc}" ]; then
            cp "${REPO_ROOT}/${doc}" "${STAGING}/"
        fi
    done

    # Create a simple README for macOS users
    cat > "${STAGING}/INSTALL.txt" <<'INSTALLEOF'
Junocash Installation
======================

1. Copy the 'bin' folder to a location of your choice
   (e.g., /Applications/Junocash or ~/Applications/Junocash)

2. Add the bin folder to your PATH, or run binaries directly:

   ./bin/junocashd --help
   ./bin/junocash-cli --help

3. Create a configuration file at ~/.junocash/junocashd.conf

For more information, visit: https://github.com/user/juno
INSTALLEOF

    # Create ISO using genisoimage
    print_info "Creating ISO image..."
    genisoimage -V "${VOLUME_NAME}" \
        -D -R -apple -no-pad \
        -o "${ISO_FILE}" \
        "${STAGING}"

    # Convert to DMG using dmg tool
    if [ -x "$DMG_TOOL" ]; then
        print_info "Converting to compressed DMG..."
        "${DMG_TOOL}" "${ISO_FILE}" "${DMG_TEMP}"

        # Move to output
        mkdir -p "${OUTPUT_DIR}"
        mv "${DMG_TEMP}" "${OUTPUT_DMG}"
    else
        # Fallback: just rename ISO to DMG (uncompressed but works)
        print_warn "DMG tool not available, creating uncompressed DMG"
        mkdir -p "${OUTPUT_DIR}"
        mv "${ISO_FILE}" "${OUTPUT_DMG}"
    fi

    # Cleanup
    rm -rf "${TEMP_DIR}"

    print_success "DMG created: ${OUTPUT_DMG}"

    # Show size
    local SIZE=$(ls -lh "${OUTPUT_DMG}" | awk '{print $5}')
    print_info "DMG size: ${SIZE}"
}

# Main
main() {
    print_info "Junocash DMG Creator"

    parse_args "$@"
    check_dependencies
    build_dmg_tool
    create_dmg

    print_success "Done!"
}

main "$@"
