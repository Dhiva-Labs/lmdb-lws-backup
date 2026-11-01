#!/usr/bin/env bash
# Lays out the installed file tree that pack.sh packages, from the binary the
# build job compiled and tested (there is no .deb to unpack).
#
# usage: make-tree.sh <lmdb-lws-backup binary> <root>
set -euo pipefail
[ $# -eq 2 ] || { echo "usage: $0 <lmdb-lws-backup binary> <root>" >&2; exit 2; }
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
bin="$1"
root="$2"
name=lmdb-lws-backup

install -Dm755 "$bin" "$root/usr/bin/$name"
# The systemd units, installed disabled; the package puts the binary in
# /usr/bin rather than the /usr/local/bin of a manual install.
for unit in "$repo"/systemd/*.service "$repo"/systemd/*.timer; do
  install -Dm644 "$unit" "$root/usr/lib/systemd/system/$(basename "$unit")"
  sed -i "s|/usr/local/bin/$name|/usr/bin/$name|" "$root/usr/lib/systemd/system/$(basename "$unit")"
done
if grep -r /usr/local "$root/usr/lib/systemd/system"; then
  echo "a unit still points at /usr/local" >&2
  exit 1
fi
for f in README.md CHANGELOG.md config.example.toml; do
  install -Dm644 "$repo/$f" "$root/usr/share/doc/$name/$f"
done
# Licenses of the code linked into the binary.
lic="$root/usr/share/licenses/$name"
install -Dm644 "$repo/third_party/liblmdb/LICENSE" "$lic/LMDB-LICENSE"
install -Dm644 "$repo/third_party/liblmdb/COPYRIGHT" "$lic/LMDB-COPYRIGHT"
install -Dm644 "$repo/third_party/tomlplusplus/LICENSE" "$lic/tomlplusplus-LICENSE"
install -Dm644 "$repo/third_party/libsodium/LICENSE" "$lic/libsodium-LICENSE"
find "$root" | sort
