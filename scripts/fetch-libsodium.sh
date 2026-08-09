#!/bin/sh
# Fetches and builds a static libsodium into third_party/local/.
# Run from anywhere; only needed when libsodium-dev is not installed system-wide.
set -eu

VERSION=1.0.20
SHA256_PIN_FILE="$(dirname "$0")/libsodium-${VERSION}.sha256"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
PREFIX="$TP/local"
TARBALL="$TP/libsodium-${VERSION}.tar.gz"
SRCDIR="$TP/libsodium-${VERSION}"

if [ -f "$PREFIX/lib/libsodium.a" ]; then
    echo "libsodium already built at $PREFIX"
    exit 0
fi

if [ ! -f "$TARBALL" ]; then
    curl -fL -o "$TARBALL" \
        "https://github.com/jedisct1/libsodium/releases/download/${VERSION}-RELEASE/libsodium-${VERSION}.tar.gz"
fi

if [ -f "$SHA256_PIN_FILE" ]; then
    expected="$(cat "$SHA256_PIN_FILE")"
    actual="$(sha256sum "$TARBALL" | cut -d' ' -f1)"
    if [ "$expected" != "$actual" ]; then
        echo "ERROR: libsodium tarball sha256 mismatch" >&2
        echo "  expected: $expected" >&2
        echo "  actual:   $actual" >&2
        exit 1
    fi
else
    echo "WARNING: no sha256 pin file, skipping verification" >&2
fi

rm -rf "$SRCDIR"
tar -xzf "$TARBALL" -C "$TP"
cd "$SRCDIR"
./configure --prefix="$PREFIX" --disable-shared --enable-static --with-pic --quiet
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "libsodium ${VERSION} installed to $PREFIX"
