#!/usr/bin/env bash

export LC_ALL=C
set -eu

# JunoCash Multi-Platform Release Builder
# Builds release binaries for Windows, Linux, and macOS (including Apple Silicon)

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Default settings
BUILD_WINDOWS=false
BUILD_LINUX=false
BUILD_LINUX_DEBUG=false
BUILD_MACOS_INTEL=false
BUILD_MACOS_ARM=false
BUILD_ALL=false
BUILD_ALL_DEBUG=false
CLEAN_BUILD=false
CREATE_DMG=true
USE_GITIAN=false
FORCE_LXC=false
GIT_REF="HEAD"
# Pinned Debian Bullseye image for reproducible builds (glibc 2.31)
DEBIAN_BULLSEYE_DIGEST="ee239c601913c0d3962208299eef70dcffcb7aac1787f7a02f6d3e2b518755e6"
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
RELEASE_DIR="${REPO_ROOT}/release"
SDK_PATH="${REPO_ROOT}/depends/SDKs"

# Version detection - calculate from configure.ac defines with BUILD suffix logic
# This replicates the m4 macros in configure.ac to get the correct version string
calculate_version() {
    local MAJOR=$(grep "define(_CLIENT_VERSION_MAJOR" "${REPO_ROOT}/configure.ac" | sed 's/[^0-9]*//g' || echo "0")
    local MINOR=$(grep "define(_CLIENT_VERSION_MINOR" "${REPO_ROOT}/configure.ac" | sed 's/[^0-9]*//g' || echo "0")
    local REVISION=$(grep "define(_CLIENT_VERSION_REVISION" "${REPO_ROOT}/configure.ac" | sed 's/[^0-9]*//g' || echo "0")
    local BUILD=$(grep "^define(_CLIENT_VERSION_BUILD" "${REPO_ROOT}/configure.ac" | sed 's/[^0-9]*//g' || echo "0")

    # Replicate m4 logic for version suffix:
    # BUILD < 25: beta(BUILD+1)
    # BUILD 25-49: rc(BUILD-24)
    # BUILD == 50: stable (no suffix)
    # BUILD > 50: -(BUILD-50)
    local SUFFIX=""
    if [ "$BUILD" -lt 25 ]; then
        SUFFIX="-beta$((BUILD + 1))"
    elif [ "$BUILD" -lt 50 ]; then
        SUFFIX="-rc$((BUILD - 24))"
    elif [ "$BUILD" -eq 50 ]; then
        SUFFIX=""
    else
        SUFFIX="-$((BUILD - 50))"
    fi

    echo "${MAJOR}.${MINOR}.${REVISION}${SUFFIX}"
}

FULL_VERSION=$(calculate_version)

# Print functions
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_step() {
    echo -e "\n${GREEN}==>${NC} ${BLUE}$1${NC}\n"
}

# Usage information
usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Build JunoCash release binaries for multiple platforms.

OPTIONS:
    -w, --windows           Build for Windows (x86_64-w64-mingw32)
    -l, --linux             Build for Linux (x86_64-pc-linux-gnu)
    --linux-debug           Build Linux with debug symbols
    -m, --macos-intel       Build for macOS Intel (x86_64-apple-darwin)
    -a, --macos-arm         Build for macOS Apple Silicon (aarch64-apple-darwin)
    -A, --all               Build for all platforms (default if no platform specified)
    --all-debug             Build for all platforms with debug symbols
    -g, --gitian            Use Gitian for reproducible Linux builds (glibc 2.31)
    -t, --tag REF           Git tag/branch/commit to build (default: HEAD, for --gitian)
    --lxc                   Force LXC for Gitian (default: Docker)
    -c, --clean             Clean before building
    -j, --jobs N            Number of parallel jobs (default: $JOBS)
    -o, --output DIR        Output directory for releases (default: ./release)
    -s, --sdk-path PATH     Path to macOS SDK directory (default: ./depends/SDKs)
    --no-dmg                Skip DMG creation for macOS builds
    -h, --help              Show this help message

EXAMPLES:
    # Build all platforms
    $0 --all

    # Build only Windows and Linux
    $0 --windows --linux

    # Build with 8 jobs and clean first
    $0 --all --clean -j 8

    # Build macOS with custom SDK path
    $0 --macos-arm --sdk-path /path/to/SDKs

    # Build reproducible Linux release with Gitian (glibc 2.31)
    $0 --linux --gitian

    # Build reproducible release from a specific tag
    $0 --linux --gitian --tag v1.0.0

NOTES:
    - For macOS builds, you need to set up the SDK first. See doc/macos-sdk-setup.md
    - Windows builds require mingw-w64 toolchain
    - Cross-compilation dependencies will be built automatically via depends/
    - Gitian builds require LXC or Docker. See contrib/gitian-descriptors/README.md

EOF
    exit 0
}

# Parse command line arguments
parse_args() {
    if [ $# -eq 0 ]; then
        BUILD_ALL=true
    fi

    while [ $# -gt 0 ]; do
        case "$1" in
            -w|--windows)
                BUILD_WINDOWS=true
                ;;
            -l|--linux)
                BUILD_LINUX=true
                ;;
            --linux-debug)
                BUILD_LINUX_DEBUG=true
                ;;
            -m|--macos-intel)
                BUILD_MACOS_INTEL=true
                ;;
            -a|--macos-arm)
                BUILD_MACOS_ARM=true
                ;;
            -A|--all)
                BUILD_ALL=true
                ;;
            --all-debug)
                BUILD_ALL_DEBUG=true
                ;;
            -g|--gitian)
                USE_GITIAN=true
                ;;
            -t|--tag)
                GIT_REF="$2"
                shift
                ;;
            --lxc)
                FORCE_LXC=true
                ;;
            -c|--clean)
                CLEAN_BUILD=true
                ;;
            -j|--jobs)
                JOBS="$2"
                shift
                ;;
            -o|--output)
                RELEASE_DIR="$2"
                shift
                ;;
            -s|--sdk-path)
                SDK_PATH="$2"
                shift
                ;;
            --no-dmg)
                CREATE_DMG=false
                ;;
            -h|--help)
                usage
                ;;
            *)
                print_error "Unknown option: $1"
                echo "Use --help for usage information"
                exit 1
                ;;
        esac
        shift
    done

    # If --all or --all-debug is specified or no specific platform, build all
    if [ "$BUILD_ALL" = true ] || [ "$BUILD_ALL_DEBUG" = true ]; then
        BUILD_WINDOWS=true
        BUILD_LINUX=true
        BUILD_MACOS_INTEL=true
        BUILD_MACOS_ARM=true
        if [ "$BUILD_ALL_DEBUG" = true ]; then
            BUILD_LINUX_DEBUG=true
        fi
    fi
}

# Check prerequisites
check_prerequisites() {
    print_step "Checking prerequisites"

    # Check for required tools
    # Note: libtool script is generated by configure, we check for libtoolize instead
    local required_tools="make gcc g++ autoconf automake libtoolize pkg-config"
    for tool in $required_tools; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            print_error "Required tool not found: $tool"
            exit 1
        fi
    done

    # Check for Rust
    if ! command -v cargo >/dev/null 2>&1; then
        print_error "Rust/Cargo not found. Please install Rust: https://rustup.rs"
        exit 1
    fi

    # Check Windows cross-compiler if needed
    if [ "$BUILD_WINDOWS" = true ]; then
        if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
            print_warn "MinGW-w64 not found. Install with: sudo apt-get install mingw-w64"
            print_warn "Skipping Windows build"
            BUILD_WINDOWS=false
        fi
    fi

    # Check macOS SDK if needed
    if [ "$BUILD_MACOS_INTEL" = true ] || [ "$BUILD_MACOS_ARM" = true ]; then
        if [ ! -d "$SDK_PATH" ]; then
            print_warn "macOS SDK not found at: $SDK_PATH"
            print_warn "See doc/macos-sdk-setup.md for setup instructions"
            print_warn "Skipping macOS builds"
            BUILD_MACOS_INTEL=false
            BUILD_MACOS_ARM=false
        else
            # Look for SDK
            SDK_FOUND=false
            for sdk in "$SDK_PATH"/MacOSX*.sdk "$SDK_PATH"/Xcode-*.sdk; do
                if [ -d "$sdk" ]; then
                    SDK_FOUND=true
                    break
                fi
            done
            if [ "$SDK_FOUND" = false ]; then
                print_warn "No macOS SDK found in: $SDK_PATH"
                print_warn "See doc/macos-sdk-setup.md for setup instructions"
                print_warn "Skipping macOS builds"
                BUILD_MACOS_INTEL=false
                BUILD_MACOS_ARM=false
            fi
        fi
    fi

    print_success "Prerequisites check complete"
}

# Clean build directories
clean_build() {
    if [ "$CLEAN_BUILD" = true ]; then
        print_step "Cleaning build directories"
        cd "$REPO_ROOT"
        ./zcutil/clean.sh || true
        make clean || true
        make distclean || true
        print_success "Clean complete"
    fi
}

# Build Linux with Gitian for reproducible builds
build_gitian_linux() {
    print_step "Building Linux with Gitian (reproducible, glibc 2.31)"

    cd "$REPO_ROOT"

    # Check for gitian-builder
    local GITIAN_BUILDER="${REPO_ROOT}/../gitian-builder"
    if [ ! -d "$GITIAN_BUILDER" ]; then
        print_info "Cloning gitian-builder..."
        git clone https://github.com/devrandom/gitian-builder.git "$GITIAN_BUILDER"
    fi

    # Check for Docker or LXC (Docker is preferred for reproducibility)
    local USE_LXC=0
    local USE_DOCKER=0

    if [ "$FORCE_LXC" = true ]; then
        if command -v lxc-create >/dev/null 2>&1; then
            USE_LXC=1
            print_info "Using LXC for Gitian build (forced)"
        else
            print_error "LXC requested but not found. Please install LXC."
            print_info "  LXC: sudo apt-get install lxc"
            exit 1
        fi
    elif command -v docker >/dev/null 2>&1; then
        USE_DOCKER=1
        print_info "Using Docker for Gitian build (pinned image: debian@sha256:${DEBIAN_BULLSEYE_DIGEST:0:12}...)"
    elif command -v lxc-create >/dev/null 2>&1; then
        USE_LXC=1
        print_info "Using LXC for Gitian build (Docker not available)"
    else
        print_error "Gitian requires Docker (preferred) or LXC. Please install one of them."
        print_info "  Docker: https://docs.docker.com/get-docker/"
        print_info "  LXC: sudo apt-get install lxc"
        exit 1
    fi

    # Create base VM if needed
    cd "$GITIAN_BUILDER"

    # Use direct Debian mirror (bypass apt-cacher-ng proxy)
    export DIRECT_MIRROR=1

    # Expected image name based on pinned hash
    local PINNED_IMAGE_NAME="base-${DEBIAN_BULLSEYE_DIGEST}-amd64"

    if [ "$USE_LXC" = 1 ]; then
        if [ ! -e "base-bullseye-amd64" ]; then
            print_info "Creating Gitian base VM (this may take a while)..."
            export USE_LXC=1
            bin/make-base-vm --distro debian --suite bullseye --arch amd64 --lxc
        fi
    elif [ "$USE_DOCKER" = 1 ]; then
        if ! docker images | grep -q "$PINNED_IMAGE_NAME"; then
            print_info "Creating Gitian Docker image with pinned base..."
            export USE_DOCKER=1
            export DOCKER_IMAGE_HASH="$DEBIAN_BULLSEYE_DIGEST"
            bin/make-base-vm --distro debian --suite bullseye --arch amd64 --docker
        fi
    fi

    # Set reference datetime for reproducibility
    local REF_DATETIME
    REF_DATETIME=$(date -u +"%Y-%m-%d %H:%M:%S")

    # Run Gitian build
    print_info "Running Gitian build..."
    mkdir -p inputs

    if [ "$USE_LXC" = 1 ]; then
        export USE_LXC=1
    else
        export USE_DOCKER=1
        export DOCKER_IMAGE_HASH="$DEBIAN_BULLSEYE_DIGEST"
    fi

    print_info "Building from git ref: $GIT_REF"
    bin/gbuild --commit junocash="$GIT_REF" \
        --url junocash="$REPO_ROOT" \
        "$REPO_ROOT/contrib/gitian-descriptors/gitian-linux-parallel.yml"

    # Copy outputs
    mkdir -p "$RELEASE_DIR"
    cp -v build/out/*.tar.gz "$RELEASE_DIR/" 2>/dev/null || true

    cd "$REPO_ROOT"
    print_success "Gitian build complete"

    # Verify symbol versions
    print_info "Verifying glibc symbols..."
    if [ -f "$RELEASE_DIR/junocash-${FULL_VERSION}-linux64.tar.gz" ]; then
        local TEMP_DIR
        TEMP_DIR=$(mktemp -d)
        tar -xzf "$RELEASE_DIR/junocash-${FULL_VERSION}-linux64.tar.gz" -C "$TEMP_DIR"
        python3 "$REPO_ROOT/contrib/devtools/symbol-check.py" "$TEMP_DIR"/*/bin/junocashd || true
        rm -rf "$TEMP_DIR"
    fi
}

# Build for a specific platform
build_platform() {
    local PLATFORM_NAME="$1"
    local HOST_TRIPLET="$2"
    local EXTRA_OPTS="${3:-}"

    print_step "Building for $PLATFORM_NAME ($HOST_TRIPLET)"

    cd "$REPO_ROOT"

    # Build dependencies
    print_info "Building dependencies for $PLATFORM_NAME..."
    cd depends
    if [ -n "$EXTRA_OPTS" ]; then
        # shellcheck disable=SC2086
        make HOST="$HOST_TRIPLET" -j"$JOBS" $EXTRA_OPTS
    else
        make HOST="$HOST_TRIPLET" -j"$JOBS"
    fi
    cd "$REPO_ROOT"

    # Configure and build
    print_info "Configuring JunoCash for $PLATFORM_NAME..."
    ./zcutil/clean.sh || true
    ./autogen.sh
    CONFIG_SITE="$PWD/depends/$HOST_TRIPLET/share/config.site" ./configure --quiet --disable-tests

    print_info "Building JunoCash for $PLATFORM_NAME..."
    make -j"$JOBS"

    print_success "Build complete for $PLATFORM_NAME"
}

# Build all requested platforms
build_all_platforms() {
    local BUILD_COUNT=0

    # Count how many platforms we're building
    [ "$BUILD_LINUX" = true ] && BUILD_COUNT=$((BUILD_COUNT + 1))
    [ "$BUILD_LINUX_DEBUG" = true ] && BUILD_COUNT=$((BUILD_COUNT + 1))
    [ "$BUILD_WINDOWS" = true ] && BUILD_COUNT=$((BUILD_COUNT + 1))
    [ "$BUILD_MACOS_INTEL" = true ] && BUILD_COUNT=$((BUILD_COUNT + 1))
    [ "$BUILD_MACOS_ARM" = true ] && BUILD_COUNT=$((BUILD_COUNT + 1))

    print_info "Building $BUILD_COUNT platform(s)"

    # Linux (gitian builds both release and debug automatically)
    if [ "$BUILD_LINUX" = true ]; then
        if [ "$USE_GITIAN" = true ]; then
            build_gitian_linux
        else
            build_platform "Linux x86_64" "x86_64-pc-linux-gnu"
            package_release "linux-x86_64" "x86_64-pc-linux-gnu"
            # Build debug version if requested (for non-gitian)
            if [ "$BUILD_LINUX_DEBUG" = true ]; then
                package_release_debug "linux-x86_64" "x86_64-pc-linux-gnu"
            fi
        fi
    fi

    # Windows
    if [ "$BUILD_WINDOWS" = true ]; then
        build_platform "Windows x86_64" "x86_64-w64-mingw32"
        package_release "win64" "x86_64-w64-mingw32"
    fi

    # macOS Intel
    if [ "$BUILD_MACOS_INTEL" = true ]; then
        build_platform "macOS x86_64 (Intel)" "x86_64-apple-darwin18" "SDK_PATH=$SDK_PATH"
        package_release "macos-x86_64" "x86_64-apple-darwin18"
    fi

    # macOS Apple Silicon
    if [ "$BUILD_MACOS_ARM" = true ]; then
        build_platform "macOS ARM64 (Apple Silicon)" "aarch64-apple-darwin" "SDK_PATH=$SDK_PATH"
        package_release "macos-arm64" "aarch64-apple-darwin"
    fi
}

# Package release for a platform
package_release() {
    local PLATFORM_NAME="$1"
    local HOST_TRIPLET="$2"

    print_info "Packaging release for $PLATFORM_NAME..."

    # Create release directory
    mkdir -p "$RELEASE_DIR"

    # Determine file extension
    local EXE_EXT=""
    local ARCHIVE_EXT="tar.gz"
    if [[ "$PLATFORM_NAME" == win* ]]; then
        EXE_EXT=".exe"
        ARCHIVE_EXT="zip"
    fi

    # Create temporary packaging directory
    local PKG_DIR="$RELEASE_DIR/junocash-${FULL_VERSION}-${PLATFORM_NAME}"
    rm -rf "$PKG_DIR"
    mkdir -p "$PKG_DIR/bin"

    # Copy binaries
    cp "$REPO_ROOT/src/junocashd${EXE_EXT}" "$PKG_DIR/bin/" 2>/dev/null || true
    cp "$REPO_ROOT/src/junocash-cli${EXE_EXT}" "$PKG_DIR/bin/" 2>/dev/null || true
    cp "$REPO_ROOT/src/junocash-tx${EXE_EXT}" "$PKG_DIR/bin/" 2>/dev/null || true
    cp "$REPO_ROOT/target/release/junocashd-wallet-tool${EXE_EXT}" "$PKG_DIR/bin/" 2>/dev/null || true

    # Copy documentation
    cp "$REPO_ROOT/README.md" "$PKG_DIR/"
    cp "$REPO_ROOT/COPYING" "$PKG_DIR/"

    # Create example config
    cat > "$PKG_DIR/junocash.conf.example" <<'CONFEOF'
# JunoCash Configuration Example

# RPC Settings
#rpcuser=your_username
#rpcpassword=your_secure_password
#rpcallowip=127.0.0.1

# Mining
#gen=1

# Logging
#debug=1
CONFEOF

    # Create archive
    cd "$RELEASE_DIR"
    local ARCHIVE_NAME="junocash-${FULL_VERSION}-${PLATFORM_NAME}.${ARCHIVE_EXT}"

    if [ "$ARCHIVE_EXT" = "zip" ]; then
        if command -v zip >/dev/null 2>&1; then
            zip -r "$ARCHIVE_NAME" "$(basename "$PKG_DIR")"
        else
            print_warn "zip command not found, creating tar.gz instead"
            tar -czf "junocash-${FULL_VERSION}-${PLATFORM_NAME}.tar.gz" "$(basename "$PKG_DIR")"
        fi
    else
        tar -czf "$ARCHIVE_NAME" "$(basename "$PKG_DIR")"
    fi

    # Generate SHA256 checksum
    # Remove old checksum for this file if it exists
    if [ -f "SHA256SUMS.txt" ]; then
        grep -v "$(basename "$ARCHIVE_NAME")" "SHA256SUMS.txt" > "SHA256SUMS.txt.tmp" 2>/dev/null || true
        mv "SHA256SUMS.txt.tmp" "SHA256SUMS.txt"
    fi
    # Add new checksum
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$ARCHIVE_NAME" >> "SHA256SUMS.txt"
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$ARCHIVE_NAME" >> "SHA256SUMS.txt"
    fi

    # Create DMG for macOS platforms
    if [[ "$PLATFORM_NAME" == macos* ]] && [ "$CREATE_DMG" = true ]; then
        create_dmg "$PLATFORM_NAME"
    fi

    # Clean up temp directory
    rm -rf "$PKG_DIR"

    cd "$REPO_ROOT"
    print_success "Package created: $ARCHIVE_NAME"
}

# Package debug symbols for a platform
package_release_debug() {
    local PLATFORM_NAME="$1"
    local HOST_TRIPLET="$2"

    print_info "Packaging debug symbols for $PLATFORM_NAME..."

    # Create release directory
    mkdir -p "$RELEASE_DIR"

    # Create temporary packaging directory
    local PKG_DIR="$RELEASE_DIR/junocash-${FULL_VERSION}-${PLATFORM_NAME}-debug"
    rm -rf "$PKG_DIR"
    mkdir -p "$PKG_DIR/bin"

    # Extract debug symbols and create .dbg files
    for binary in junocashd junocash-cli junocash-tx; do
        if [ -f "$REPO_ROOT/src/${binary}" ]; then
            objcopy --only-keep-debug "$REPO_ROOT/src/${binary}" "$PKG_DIR/bin/${binary}.dbg" 2>/dev/null || true
        fi
    done

    # Also handle Rust wallet tool
    if [ -f "$REPO_ROOT/target/release/junocashd-wallet-tool" ]; then
        objcopy --only-keep-debug "$REPO_ROOT/target/release/junocashd-wallet-tool" "$PKG_DIR/bin/junocashd-wallet-tool.dbg" 2>/dev/null || true
    fi

    # Create archive
    cd "$RELEASE_DIR"
    local ARCHIVE_NAME="junocash-${FULL_VERSION}-${PLATFORM_NAME}-debug.tar.gz"
    tar -czf "$ARCHIVE_NAME" "$(basename "$PKG_DIR")"

    # Generate SHA256 checksum
    if [ -f "SHA256SUMS.txt" ]; then
        grep -v "$(basename "$ARCHIVE_NAME")" "SHA256SUMS.txt" > "SHA256SUMS.txt.tmp" 2>/dev/null || true
        mv "SHA256SUMS.txt.tmp" "SHA256SUMS.txt"
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$ARCHIVE_NAME" >> "SHA256SUMS.txt"
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$ARCHIVE_NAME" >> "SHA256SUMS.txt"
    fi

    # Clean up temp directory
    rm -rf "$PKG_DIR"

    cd "$REPO_ROOT"
    print_success "Debug package created: $ARCHIVE_NAME"
}

# Create DMG for macOS
create_dmg() {
    local PLATFORM_NAME="$1"
    local DMG_NAME="junocash-${FULL_VERSION}-${PLATFORM_NAME}-unsigned.dmg"

    print_info "Creating DMG: $DMG_NAME..."

    # Recreate the package directory for DMG (was just deleted for tar)
    local PKG_DIR="$RELEASE_DIR/junocash-${FULL_VERSION}-${PLATFORM_NAME}"
    mkdir -p "$PKG_DIR/bin"

    # Copy binaries
    cp "$REPO_ROOT/src/junocashd" "$PKG_DIR/bin/" 2>/dev/null || true
    cp "$REPO_ROOT/src/junocash-cli" "$PKG_DIR/bin/" 2>/dev/null || true
    cp "$REPO_ROOT/src/junocash-tx" "$PKG_DIR/bin/" 2>/dev/null || true
    cp "$REPO_ROOT/target/release/junocashd-wallet-tool" "$PKG_DIR/bin/" 2>/dev/null || true

    # Copy documentation
    cp "$REPO_ROOT/README.md" "$PKG_DIR/" 2>/dev/null || true
    cp "$REPO_ROOT/COPYING" "$PKG_DIR/" 2>/dev/null || true

    # Create install instructions
    cat > "$PKG_DIR/INSTALL.txt" <<'INSTALLEOF'
Juno Cash Installation
======================

1. Copy the 'bin' folder to a location of your choice
   (e.g., /Applications/JunoCash or ~/Applications/JunoCash)

2. Add the bin folder to your PATH, or run binaries directly:

   ./bin/junocashd --help
   ./bin/junocash-cli --help

3. Create a configuration file at ~/.junocash/junocashd.conf

For more information, visit: https://github.com/user/juno
INSTALLEOF

    # Call create-dmg.sh script
    if [ -x "$REPO_ROOT/zcutil/create-dmg.sh" ]; then
        "$REPO_ROOT/zcutil/create-dmg.sh" \
            --source "$PKG_DIR" \
            --output "$RELEASE_DIR" \
            --version "$FULL_VERSION" \
            --name "junocash-${FULL_VERSION}-${PLATFORM_NAME}-unsigned"
    else
        print_warn "create-dmg.sh not found, skipping DMG creation"
    fi

    # Add DMG checksum if it was created
    if [ -f "$RELEASE_DIR/$DMG_NAME" ]; then
        cd "$RELEASE_DIR"
        # Remove old checksum for this DMG if it exists
        if [ -f "SHA256SUMS.txt" ]; then
            grep -v "$DMG_NAME" "SHA256SUMS.txt" > "SHA256SUMS.txt.tmp" 2>/dev/null || true
            mv "SHA256SUMS.txt.tmp" "SHA256SUMS.txt"
        fi
        # Add new checksum
        if command -v sha256sum >/dev/null 2>&1; then
            sha256sum "$DMG_NAME" >> "SHA256SUMS.txt"
        elif command -v shasum >/dev/null 2>&1; then
            shasum -a 256 "$DMG_NAME" >> "SHA256SUMS.txt"
        fi
        cd "$REPO_ROOT"
        print_success "DMG created: $DMG_NAME"
    fi

    # Clean up temp directory
    rm -rf "$PKG_DIR"
}

# Main execution
main() {
    print_info "JunoCash Multi-Platform Release Builder"
    print_info "Version: $FULL_VERSION"

    parse_args "$@"
    check_prerequisites
    clean_build

    # Create release directory (preserve existing SHA256SUMS.txt)
    mkdir -p "$RELEASE_DIR"

    build_all_platforms

    print_step "Build Summary"
    print_success "All builds complete!"
    print_info "Release artifacts are in: $RELEASE_DIR"

    if [ -f "$RELEASE_DIR/SHA256SUMS.txt" ]; then
        echo ""
        print_info "SHA256 Checksums:"
        cat "$RELEASE_DIR/SHA256SUMS.txt"
    fi
}

# Run main
main "$@"
