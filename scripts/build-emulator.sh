#!/bin/sh
# build-emulator.sh - build atari800 with the kms/alsa port inside the Alpine env
#
#   scripts/alpine-env.sh run scripts/build-emulator.sh
#
# Output: build/emulator/src/atari800 (musl, dynamically linked against
# libdrm and alsa-lib from the same Alpine branch that the image uses).

set -eu
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
[ -f /etc/alpine-release ] || { echo "error: run me through scripts/alpine-env.sh run $0 (or in an Alpine container)" >&2; exit 1; }
SRC=$PROJECT/third_party/atari800
OUT=$PROJECT/build/emulator

# Our port lives outside the atari800 tree; link it in and teach the
# build system about --with-video=kms / --with-sound=alsa.
ln -sfn ../../../emulator/kms "$SRC/src/kms"
ln -sfn ../../../emulator/sound_alsa.c "$SRC/src/sound_alsa.c"
for p in "$PROJECT"/emulator/patches/*.patch; do
	if git -C "$SRC" apply --check --reverse "$p" >/dev/null 2>&1; then
		continue # already applied
	fi
	git -C "$SRC" apply "$p"
done

if [ ! -x "$SRC/configure" ] || [ "$SRC/configure.ac" -nt "$SRC/configure" ]; then
	(cd "$SRC" && autoreconf -fi)
fi

mkdir -p "$OUT"
cd "$OUT"
if [ ! -f Makefile ] || [ "$SRC/configure" -nt Makefile ]; then
	"$SRC/configure" \
		--with-video=kms --with-sound=alsa \
		--enable-netsio \
		--without-readline --without-x \
		--with-png=no --with-mp3=no --with-zlib=yes \
		--disable-monitorbreak --disable-monitortrace \
		CFLAGS="-O2 -pipe -I$PROJECT/common"
fi
make -j"$(nproc)"
ls -la src/atari800
