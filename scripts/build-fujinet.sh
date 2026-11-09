#!/bin/sh
# build-fujinet.sh - build FujiNet-PC (Atari target) inside the Alpine env
#
#   scripts/alpine-env.sh run scripts/build-fujinet.sh
#
# Output: build/fujinet/dist/{fujinet,data,fnconfig.ini,SD} (build/i686/... for ARCH=x86)
#
# FujiNet-PC wants Mbed TLS 3.x as static libraries; Alpine ships 4.x, so a
# 3.6 LTS copy is built once into build/mbedtls3. The musl fixes in
# fujinet/patches/ are applied to the submodule before building.

set -eu
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
[ -f /etc/alpine-release ] || { echo "error: run me through scripts/alpine-env.sh run $0 (or in an Alpine container)" >&2; exit 1; }
# ARCH=x86 (set by "ARCH=x86 scripts/alpine-env.sh run"): the 32-bit build,
# under build/i686/ (docs/specs/i686-image.md).
ARCH=${ARCH:-x86_64}
case "$ARCH" in
	x86_64) ABUILD=$PROJECT/build ;;
	x86) ABUILD=$PROJECT/build/i686 ;;
	*) echo "error: ARCH must be x86_64 or x86, not $ARCH" >&2; exit 1 ;;
esac
[ "$(apk --print-arch)" = "$ARCH" ] || { echo "error: ARCH=$ARCH, but this Alpine is $(apk --print-arch) (ARCH=$ARCH scripts/alpine-env.sh run ...)" >&2; exit 1; }
SRC=$PROJECT/third_party/fujinet-firmware
OUT=$ABUILD/fujinet
MBED=$ABUILD/mbedtls3
DL=$PROJECT/build/downloads
# 3.6.3.1 is the 3.6.3 LTS re-release (its headers say 3.6.3); the tarball
# is checked against this sha256 (override both together).
MBEDTLS_VERSION="${MBEDTLS_VERSION:-3.6.3.1}"
MBEDTLS_SHA256="${MBEDTLS_SHA256:-243ed496d5f88a5b3791021be2800aac821b9a4cc16e7134aa413c58b4c20e0c}"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

for p in "$PROJECT"/fujinet/patches/*.patch; do
	[ -f "$p" ] || continue
	if git -C "$SRC" apply --check --reverse "$p" >/dev/null 2>&1; then
		continue
	fi
	log "Applying $(basename "$p")"
	git -C "$SRC" apply "$p"
done

if [ ! -f "$MBED/lib/libmbedtls.a" ]; then
	log "Building Mbed TLS $MBEDTLS_VERSION"
	mkdir -p "$DL"
	tb="$DL/mbedtls-$MBEDTLS_VERSION.tar.bz2"
	if [ ! -f "$tb" ]; then
		curl -fsSL -o "$tb.part" \
			"https://github.com/Mbed-TLS/mbedtls/releases/download/v$MBEDTLS_VERSION/mbedtls-$MBEDTLS_VERSION.tar.bz2"
		mv "$tb.part" "$tb"
	fi
	[ "$(sha256sum "$tb" | cut -d' ' -f1)" = "$MBEDTLS_SHA256" ] || { echo "error: checksum mismatch for $tb" >&2; exit 1; }
	rm -rf "$DL/src-mbedtls"
	mkdir -p "$DL/src-mbedtls"
	tar --no-same-owner -xjf "$tb" -C "$DL/src-mbedtls" --strip-components=1
	cmake -G Ninja -S "$DL/src-mbedtls" -B "$DL/src-mbedtls/b" \
		-DCMAKE_BUILD_TYPE=Release -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
		-DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DMBEDTLS_FATAL_WARNINGS=OFF -DCMAKE_INSTALL_PREFIX="$MBED" >/dev/null
	cmake --build "$DL/src-mbedtls/b" --parallel >/dev/null
	cmake --install "$DL/src-mbedtls/b" >/dev/null
fi

mkdir -p "$OUT"
if [ ! -f "$OUT/build.ninja" ]; then
	log "Configuring FujiNet-PC"
	# DEBUG_NO_REBOOT: FujiNet-PC normally exits and restarts itself on every
	# Atari cold reset (NetSIO 0xFF); on the appliance it simply keeps running.
	cmake -G Ninja -S "$SRC" -B "$OUT" -DFUJINET_TARGET=ATARI \
		-DCMAKE_BUILD_TYPE=Release -DMBEDTLS_ROOT_DIR="$MBED" \
		-DCMAKE_CXX_FLAGS=-DDEBUG_NO_REBOOT -DCMAKE_C_FLAGS=-DDEBUG_NO_REBOOT >/dev/null
fi
log "Building FujiNet-PC"
cmake --build "$OUT" --target dist --parallel | grep -vE '^\[[0-9]+/[0-9]+\]' || true
ls -la "$OUT/dist/fujinet"
