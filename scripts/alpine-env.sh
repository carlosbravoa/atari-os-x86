#!/bin/sh
# alpine-env.sh - unprivileged Alpine build environment for atari-os-x86
#
# Everything the appliance ships is built inside an Alpine root filesystem
# that lives under build/alpine/. No sudo, no Docker: a user namespace
# (unshare -r) makes us root inside it and bwrap/chroot runs the tools.
#
#   scripts/alpine-env.sh bootstrap        download minirootfs, install build deps
#   scripts/alpine-env.sh run <cmd...>     run a command inside, project at /work
#   scripts/alpine-env.sh shell            interactive shell inside
#
# Only the Alpine branch and the package list below are policy; the rest is
# plumbing. Downloads are verified against the release manifest's sha256.

set -eu

ALPINE_BRANCH="${ALPINE_BRANCH:-v3.24}"
ALPINE_ARCH="${ALPINE_ARCH:-x86_64}"
MIRROR="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"

PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$PROJECT/build"
ROOT="$BUILD/alpine"
DL="$BUILD/downloads"

# Packages for building our own code and assembling the image.
BUILD_PKGS="alpine-base build-base autoconf automake libtool pkgconf git
            linux-headers libdrm-dev alsa-lib-dev zlib-dev
            cpio squashfs-tools mtools xorriso dosfstools zstd curl"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

in_userns() {
	# Re-exec ourselves as root inside a fresh user+mount namespace.
	exec unshare -r -m -p -f --propagation private "$0" "--inside" "$@"
}

bootstrap() {
	mkdir -p "$DL"
	log "Fetching Alpine $ALPINE_BRANCH release manifest"
	manifest="$DL/latest-releases.yaml"
	curl -fsSL "$MIRROR/$ALPINE_BRANCH/releases/$ALPINE_ARCH/latest-releases.yaml" -o "$manifest"
	# The minirootfs entry: file name and sha256.
	file=$(awk '/flavor: alpine-minirootfs/{f=1} f&&/file:/{print $2; exit}' "$manifest")
	sha=$(awk '/flavor: alpine-minirootfs/{f=1} f&&/sha256:/{print $2; exit}' "$manifest")
	[ -n "$file" ] && [ -n "$sha" ] || die "could not parse release manifest"

	if [ ! -f "$DL/$file" ]; then
		log "Downloading $file"
		curl -fsSL "$MIRROR/$ALPINE_BRANCH/releases/$ALPINE_ARCH/$file" -o "$DL/$file.part"
		mv "$DL/$file.part" "$DL/$file"
	fi
	echo "$sha  $DL/$file" | sha256sum -c --quiet - || die "checksum mismatch for $file"

	if [ -e "$ROOT" ]; then
		die "$ROOT already exists; remove it to re-bootstrap"
	fi
	mkdir -p "$ROOT"
	log "Extracting into $ROOT"
	# Single-uid user namespace: every file becomes ours (root inside).
	tar -xzf "$DL/$file" -C "$ROOT" --no-same-owner
	cp /etc/resolv.conf "$ROOT/etc/resolv.conf"
	printf '%s/%s/main\n%s/%s/community\n' "$MIRROR" "$ALPINE_BRANCH" "$MIRROR" "$ALPINE_BRANCH" \
		> "$ROOT/etc/apk/repositories"
	mkdir -p "$ROOT/work"

	log "Installing build packages"
	# shellcheck disable=SC2086
	run_inside apk add --no-cache $BUILD_PKGS
	log "Bootstrap done"
}

run_inside() {
	[ -d "$ROOT" ] || die "no build environment; run: $0 bootstrap"
	# We are uid 0 in a private user+mount namespace: build the chroot's
	# mount table by hand (bwrap would do this, but Ubuntu's AppArmor
	# profile for it blocks apk's memfd trigger execution).
	if [ ! -e "$ROOT/.mounted" ]; then
		mount --bind "$ROOT" "$ROOT"
		mount -t proc proc "$ROOT/proc"
		mount --rbind /dev "$ROOT/dev"
		mount --rbind /sys "$ROOT/sys"
		mount -t tmpfs tmpfs "$ROOT/tmp"
		mkdir -p "$ROOT/work"
		mount --bind "$PROJECT" "$ROOT/work"
		cp /etc/resolv.conf "$ROOT/etc/resolv.conf"
		touch "$ROOT/.mounted" # lives on the tmpfs-free root, removed below
	fi
	exec_or_run chroot "$ROOT" /usr/bin/env -i \
		HOME=/root TERM="${TERM:-dumb}" \
		PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
		/bin/sh -c 'rm -f /.mounted; cd /work && exec "$@"' sh "$@"
}

exec_or_run() {
	if [ "${EXEC_LAST:-0}" = 1 ]; then exec "$@"; else "$@"; fi
}

[ "${1:-}" = "--inside" ] || { [ $# -gt 0 ] || die "usage: $0 bootstrap|run <cmd>|shell"; in_userns "$@"; }
shift

case "${1:-}" in
	bootstrap) bootstrap ;;
	run) shift; EXEC_LAST=1 run_inside "$@" ;;
	shell) EXEC_LAST=1 run_inside /bin/sh -l ;;
	*) die "usage: $0 bootstrap|run <cmd>|shell" ;;
esac
