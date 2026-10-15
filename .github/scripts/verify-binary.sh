#!/usr/bin/env bash
# Asserts the built binary (and the vendored libsodium it links) really is
# the expected CPU architecture, and that it is "mostly static": the C++ and
# gcc runtimes, libsodium and LMDB are linked in, so only glibc remains as a
# shared dependency.
#
# usage: verify-binary.sh <binary> <libsodium.a> <file(1) arch> <readelf machine>
#   e.g. verify-binary.sh build/lmdb-lws-backup third_party/local/lib/libsodium.a \
#          "ARM aarch64" AArch64
set -euo pipefail

bin=$1
sodium=$2
file_arch=$3
elf_machine=$4

description=$(file "$bin")
echo "$description"
if ! grep -qF "$file_arch" <<<"$description"; then
  echo "::error::$bin is not '$file_arch'"
  exit 1
fi

# Every object in the static libsodium must be for this CPU (catches a stale
# or foreign cached build, and any x86-only translation unit on arm64).
machines=$(readelf -h "$sodium" | sed -n 's/^ *Machine: *//p' | sort -u)
echo "libsodium.a object machines: $machines"
if [ "$machines" != "$elf_machine" ]; then
  echo "::error::$sodium holds objects for '$machines', expected only '$elf_machine'"
  exit 1
fi

# Shared libraries: glibc only. libstdc++, libgcc_s, libsodium and liblmdb
# must not appear (they are linked statically).
echo "--- ldd"
ldd "$bin"
unexpected=$(ldd "$bin" | awk '{print $1}' \
  | grep -vE '^(linux-vdso\.so\.1|linux-gate\.so\.1|/lib.*/ld-linux.*|ld-linux.*|libc\.so\.6|libm\.so\.6|libpthread\.so\.0|libdl\.so\.2|librt\.so\.1)$' || true)
if [ -n "$unexpected" ]; then
  echo "::error::unexpected shared library dependencies:"
  echo "$unexpected"
  exit 1
fi

echo "--- glibc symbol floor"
objdump -T "$bin" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n 1

echo "--- --version"
"$bin" --version
