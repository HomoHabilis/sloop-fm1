#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
# Download JieLi's Linux toolchain (clang 4.0.1 for pi32v2), check its SHA-256 and link it as DEST/toolchain.
#   tools/get_toolchain.sh [DEST]      (default: $JIELI_HOME, else ~/.jieli)
#   JIELI_TOOLCHAIN_URL, JIELI_TOOLCHAIN_SHA256   another tarball (set both)
# The pin: jieli-linux-toolchains-20250805.1.tar.xz (unpacks to jieli-linux-toolchains-20250324.1/), what
# https://pkgman.jieliapp.com/s/linux-toolchain served in October 2026; its Aliyun address first, the pkgman
# one if that fails. The same SHA-256 is pinned independently by flowstate-fm1 (Zakaria Chowdhury), whose
# version of this script this one follows, and by ElectronicCats' jieli-ble-badge-research.
# Nothing is downloaded when DEST/toolchain.sha256 names the same tarball, or when an install made by an
# earlier version of this script has exactly the pinned files (PIN_TREE). Nothing is installed unless the
# download matches the SHA-256.
set -e
PIN_URLS="https://jl-update.oss-cn-shenzhen.aliyuncs.com/jieli-linux-toolchains-20250805.1.tar.xz
https://pkgman.jieliapp.com/s/linux-toolchain"
PIN_SHA=f686586bcfb45e0f0bb27fd2b39c7a7f313cb4f0e88a66a14da621ffa8225958
PIN_TREE=3008c63d9eec73c550021137244f2a348fdecec7abaae8d475ac3474e0103589

case "$1" in -h|--help) sed -n '4,13s/^# \{0,1\}//p' "$0"; exit 0 ;; esac
if [ -n "$JIELI_TOOLCHAIN_URL" ] || [ -n "$JIELI_TOOLCHAIN_SHA256" ]; then
    [ -n "$JIELI_TOOLCHAIN_URL" ] && [ -n "$JIELI_TOOLCHAIN_SHA256" ] ||
        { echo "get_toolchain.sh: set both JIELI_TOOLCHAIN_URL and JIELI_TOOLCHAIN_SHA256" >&2; exit 1; }
    URLS="$JIELI_TOOLCHAIN_URL"
    SHA="$JIELI_TOOLCHAIN_SHA256"
else
    URLS="$PIN_URLS"
    SHA="$PIN_SHA"
fi
DEST="${1:-${JIELI_HOME:-$HOME/.jieli}}"
TC="$DEST/toolchain"
MARK="$DEST/toolchain.sha256"
if command -v sha256sum >/dev/null 2>&1; then SUM=sha256sum; else SUM="shasum -a 256"; fi

# shellcheck disable=SC2086
tree_sha() {    # hash of every file's hash and every link's target under $1 (independent of the extraction)
    (cd "$1" && {
        find . -type f ! -name .DS_Store -exec $SUM {} +
        find . -type l -exec sh -c 'for l; do echo "$l -> $(readlink "$l")"; done' sh {} +
    } | LC_ALL=C sort | $SUM | cut -d' ' -f1)
}

if [ -x "$TC/pi32v2/bin/clang" ]; then
    if [ "$(cut -d' ' -f1 "$MARK" 2>/dev/null)" = "$SHA" ]; then
        echo "JIELI_TOOLCHAIN=$TC (up to date)"
        exit 0
    fi
    if [ "$SHA" = "$PIN_SHA" ] && [ "$(tree_sha "$TC/")" = "$PIN_TREE" ]; then
        echo "$SHA  (files match the pin)" > "$MARK"
        echo "JIELI_TOOLCHAIN=$TC (files match the pinned toolchain)"
        exit 0
    fi
    echo "get_toolchain.sh: $TC is not the pinned toolchain: downloading it" >&2
fi
[ ! -e "$TC" ] || [ -L "$TC" ] || { echo "get_toolchain.sh: $TC is not a link; move it away" >&2; exit 1; }

mkdir -p "$DEST"
TMP="$(mktemp -d "$DEST/.get_toolchain.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
trap 'exit 1' INT TERM
GOT=""
for URL in $URLS; do
    echo "downloading $URL" >&2
    rm -f "$TMP/toolchain.tar.xz"
    if curl -fL --retry 3 --connect-timeout 30 --progress-bar -o "$TMP/toolchain.tar.xz" "$URL"; then
        # shellcheck disable=SC2086
        GOT="$($SUM "$TMP/toolchain.tar.xz" | cut -d' ' -f1)"
        [ "$GOT" = "$SHA" ] && break
        echo "get_toolchain.sh: $URL: SHA-256 $GOT, not the pin" >&2
    fi
done
if [ "$GOT" != "$SHA" ]; then
    { echo "get_toolchain.sh: no download matched the SHA-256, nothing installed"
      echo "  expected $SHA"
      echo "For a newer JieLi toolchain set JIELI_TOOLCHAIN_URL and JIELI_TOOLCHAIN_SHA256."; } >&2
    exit 1
fi
mkdir "$TMP/x"
tar -xJf "$TMP/toolchain.tar.xz" -C "$TMP/x"
set -- "$TMP"/x/*
{ [ $# = 1 ] && [ -x "$1/pi32v2/bin/clang" ]; } || { echo "get_toolchain.sh: unexpected tarball layout" >&2; exit 1; }
NAME="$(basename "$1")"
if [ -e "$DEST/$NAME" ]; then   # an earlier unpinned install: keep it if it has the same files
    [ "$(tree_sha "$DEST/$NAME")" = "$(tree_sha "$1")" ] ||
        { echo "get_toolchain.sh: $DEST/$NAME differs from the tarball; move it away and run again" >&2; exit 1; }
else
    mv "$1" "$DEST/$NAME"
fi
ln -sfn "$NAME" "$TC"
echo "$SHA  $URL" > "$MARK"
echo "JIELI_TOOLCHAIN=$TC ($NAME)"
