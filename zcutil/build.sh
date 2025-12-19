#!/bin/sh

export LC_ALL=C
set -eu
set +x

cmd_pref() {
    if command -v "$2" >/dev/null; then
        eval "$1=$2"
    else
        eval "$1=$3"
    fi
}

# If a g-prefixed version of the command exists, use it preferentially.
gprefix() {
    cmd_pref "$1" "g$2" "$2"
}

gprefix READLINK readlink
cd "$(dirname "$("$READLINK" -f "$0")")/.."

# Allow user overrides to $MAKE. Typical usage for users who need it:
#   MAKE=gmake ./zcutil/build.sh -j$(nproc)
if [ -z "${MAKE-}" ]; then
    MAKE="make"
    # macOS ships with GNU Make 3.81 which has broken job server pipe handling,
    # causing "Resource temporarily unavailable" errors with parallel builds.
    # Automatically use gmake (GNU Make 4.x) if available on macOS.
    if [ "$(uname -s)" = "Darwin" ]; then
        make_version=$(make --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)
        make_major=$(echo "$make_version" | cut -d. -f1)
        if [ "$make_major" -lt 4 ] 2>/dev/null && command -v gmake >/dev/null 2>&1; then
            MAKE="gmake"
            echo "Detected macOS with Make $make_version; using gmake for reliable parallel builds."
        fi
    fi
fi

# Allow overrides to $BUILD and $HOST for porters. Most users will not need it.
#   BUILD=i686-pc-linux-gnu ./zcutil/build.sh
if [ -z "${BUILD-}" ]; then
    BUILD="$(./depends/config.guess)"
fi
if [ -z "${HOST-}" ]; then
    HOST="$BUILD"
fi

# Allow users to set arbitrary compile flags. Most users will not need this.
if [ -z "${CONFIGURE_FLAGS-}" ]; then
    # If the user did not set CONFIGURE_FLAGS, then use "--quiet" unless V=1 was given.
    CONFIGURE_FLAGS="--quiet"
    for arg in "$@"
    do
        if [ "$arg" = "V=1" ]; then
            CONFIGURE_FLAGS=""
        fi
    done
fi

if [ "$*" = '--help' ]
then
    cat <<EOF
Usage:

$0 --help
  Show this help message and exit.

$0 [ MAKEARGS... ]
  Build Zcash and most of its transitive dependencies from
  source. MAKEARGS are applied to both dependencies and Zcash itself.

  Pass flags to ./configure using the CONFIGURE_FLAGS environment variable.
  For example, to enable coverage instrumentation (thus enabling "make cov"
  to work), call:

      CONFIGURE_FLAGS="--enable-lcov --disable-hardening" ./zcutil/build.sh

  For verbose output, use:
      ./zcutil/build.sh V=1
EOF
    exit 0
fi

set -x

eval "$MAKE" --version
as --version

case "$CONFIGURE_FLAGS" in
(*"--enable-debug"*)
    DEBUG=1
;;
(*)
    DEBUG=
;;esac

HOST="$HOST" BUILD="$BUILD" "$MAKE" "$@" -C ./depends/ DEBUG="$DEBUG"

if [ "${BUILD_STAGE:-all}" = "depends" ]
then
    exit 0
fi

./zcutil/clean.sh
./autogen.sh
CONFIG_SITE="$PWD/depends/$HOST/share/config.site" ./configure $CONFIGURE_FLAGS
"$MAKE" "$@"
