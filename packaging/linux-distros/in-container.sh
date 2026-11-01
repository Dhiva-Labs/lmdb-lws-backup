#!/usr/bin/env bash
# Runs inside a distribution container started by verify.sh.
# MODE: rpm (Fedora) | rpm-suse (openSUSE) | pacman (Arch)
# FILE: the package to install, under /pkgs.
set -euo pipefail
# shellcheck source=/dev/null
. /linux-distros/distro.conf

# Tools the checks use, plus SMOKE_DEPS from distro.conf (packages the smoke
# test needs; the same name on every distribution).
case "$MODE" in
  rpm)
    dnf -y -q install file findutils which procps-ng "${SMOKE_DEPS[@]}"
    ;;
  rpm-suse)
    zypper -n -q --gpg-auto-import-keys refresh
    zypper -n -q install file findutils which procps "${SMOKE_DEPS[@]}"
    ;;
  pacman)
    pacman -Syu --noconfirm --needed -q file findutils which procps-ng "${SMOKE_DEPS[@]}"
    ;;
  *) echo "unknown MODE $MODE" >&2; exit 2 ;;
esac

case "$MODE" in
  rpm)      dnf -y install "$FILE"; files() { rpm -ql "$NAME"; } ;;
  rpm-suse) zypper -n install --allow-unsigned-rpm "$FILE"; files() { rpm -ql "$NAME"; } ;;
  pacman)   pacman -U --noconfirm "$FILE"; files() { pacman -Qlq "$NAME"; } ;;
  *) echo "unknown MODE $MODE" >&2; exit 2 ;;
esac

# Every ELF file the package installed must find all of its libraries. The
# package's own library directories go on LD_LIBRARY_PATH, as the app's
# RPATH/$ORIGIN would supply them at run time, so only libraries missing
# from the distribution are reported.
mapfile -t elfs < <(files | while IFS= read -r f; do
  [ -f "$f" ] && [ ! -L "$f" ] || continue
  [ "$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ] && echo "$f"
done)
libdirs="$(for f in "${elfs[@]}"; do case "$f" in *.so|*.so.*) dirname "$f" ;; esac; done | sort -u | paste -sd:)"
missing=0
for f in "${elfs[@]}"; do
  if LD_LIBRARY_PATH="$libdirs" ldd "$f" 2>&1 | grep -q 'not found'; then
    echo "missing libraries for $f:"
    LD_LIBRARY_PATH="$libdirs" ldd "$f" | grep 'not found'
    missing=1
  fi
done
echo "${#elfs[@]} ELF files checked with ldd"
[ "$missing" = 0 ] || exit 1

# Smoke test: SMOKE_CMD from distro.conf must exit 0.
export HOME=/tmp/smoke-home
mkdir -p "$HOME"
bash -euo pipefail -c "$SMOKE_CMD"
echo "$MODE: OK"
