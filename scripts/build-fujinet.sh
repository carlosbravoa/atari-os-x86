#!/bin/sh
# build-fujinet.sh - build FujiNet-PC (Atari target) inside the Alpine env
#
#   scripts/alpine-env.sh run scripts/build-fujinet.sh
#
# Output: build/fujinet/dist/{fujinet,data,fnconfig.ini,SD}
#
# FujiNet-PC wants Mbed TLS 3.x as static libraries; Alpine ships 4.x, so a
# 3.6 LTS copy is built once into build/mbedtls3. The musl fixes in
# fujinet/patches/ are applied to the submodule before building.

set -eu
SRC=/work/third_party/fujinet-firmware
OUT=/work/build/fujinet
MBED=/work/build/mbedtls3
DL=/work/build/downloads
MBEDTLS_VERSION="${MBEDTLS_VERSION:-3.6.3}"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

for p in /work/fujinet/patches/*.patch; do
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
	[ -f "$tb" ] || curl -fsSL -o "$tb" \
		"https://github.com/Mbed-TLS/mbedtls/releases/download/v$MBEDTLS_VERSION/mbedtls-$MBEDTLS_VERSION.tar.bz2"
	rm -rf "$DL/src-mbedtls"
	mkdir -p "$DL/src-mbedtls"
	tar -xjf "$tb" -C "$DL/src-mbedtls" --strip-components=1
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
