#!/usr/bin/env bash
# Renders the AUR PKGBUILD for one release and proves it builds.
#
# usage: aur.sh <x86_64 binary> <aarch64 binary> <outdir>
#
# Fills PKGBUILD.in with the version, the release download URL and the
# sha256 of both release binaries and of the repository files the PKGBUILD
# fetches from the tag, then, in an archlinux:latest container, generates
# .SRCINFO, runs makepkg on the x86_64 sources (all of them are placed next
# to the PKGBUILD, so makepkg verifies the checksums without downloading),
# installs the result and runs it. Writes <name>-bin-<version>-aur.tar.gz
# holding PKGBUILD and .SRCINFO, ready to push to the AUR.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
[ $# -eq 3 ] || { echo "usage: $0 <x86_64 binary> <aarch64 binary> <outdir>" >&2; exit 2; }
bin_x64="$(readlink -f "$1")"; bin_arm64="$(readlink -f "$2")"
mkdir -p "$3"; out="$(cd "$3" && pwd)"
# shellcheck source=/dev/null
. "$here/../linux-distros/distro.conf"
: "${VERSION:?set VERSION}"
tag="${TAG_PREFIX-v}$VERSION"
sha() { sha256sum "$1" | cut -d' ' -f1; }

# Local name in the PKGBUILD -> path in the repository (fetched from the tag).
declare -A files=(
  [README.md]=README.md
  [CHANGELOG.md]=CHANGELOG.md
  [config.example.toml]=config.example.toml
  [$NAME.service]=systemd/$NAME.service
  [$NAME-daemon.service]=systemd/$NAME-daemon.service
  [$NAME.timer]=systemd/$NAME.timer
  [LMDB-LICENSE]=third_party/liblmdb/LICENSE
  [LMDB-COPYRIGHT]=third_party/liblmdb/COPYRIGHT
  [tomlplusplus-LICENSE]=third_party/tomlplusplus/LICENSE
  [libsodium-LICENSE]=third_party/libsodium/LICENSE
)
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
sources=""; sums=""
while IFS= read -r local_name; do
  sources+="\"$local_name::\$_raw/${files[$local_name]}\" "
  sums+="'$(sha "$repo/${files[$local_name]}")' "
  cp "$repo/${files[$local_name]}" "$work/$local_name"
done < <(printf '%s\n' "${!files[@]}" | sort)

deps=""; for d in "${ARCH_DEPENDS[@]}"; do deps+="'$d' "; done
sed -e "s|@NAME@|$NAME|g" -e "s|@VERSION@|$VERSION|g" \
    -e "s|@SUMMARY@|${SUMMARY//\'/}|g" -e "s|@HOMEPAGE@|$HOMEPAGE|g" \
    -e "s|@LICENSE@|$LICENSE|g" -e "s|@ARCH_DEPENDS@|${deps% }|g" \
    -e "s|@GITHUB_REPO@|$GITHUB_REPO|g" -e "s|@TAG@|$tag|g" \
    -e "s|@RELEASE_URL@|https://github.com/$GITHUB_REPO/releases/download/$tag|g" \
    -e "s|@SOURCES@|${sources% }|g" -e "s|@SHA256SUMS@|${sums% }|g" \
    -e "s|@SHA_X64@|$(sha "$bin_x64")|g" -e "s|@SHA_ARM64@|$(sha "$bin_arm64")|g" \
    "$here/PKGBUILD.in" > "$work/PKGBUILD"
if grep -n '@[A-Z0-9_]*@' "$work/PKGBUILD"; then echo "unfilled placeholder" >&2; exit 1; fi
cp "$bin_x64" "$work/$NAME-$VERSION-x86_64"

# The container hands /build to its builder user; the trap gives it back to
# the runner's user, or the tar below and the cleanup cannot read it.
docker run --rm -v "$work:/build" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  archlinux:latest bash -euo pipefail -c '
  trap "chown -R $HOST_UID:$HOST_GID /build" EXIT
  pacman -Syu --noconfirm --needed -q base-devel namcap
  useradd -m builder
  chown -R builder /build
  cd /build
  su builder -c "makepkg --printsrcinfo > .SRCINFO"
  su builder -c "makepkg --noconfirm --nodeps -f"
  namcap PKGBUILD
  pacman -U --noconfirm ./*-x86_64.pkg.tar.zst
  namcap ./*-x86_64.pkg.tar.zst
  pacman -Qlq '"$NAME"'-bin
  '"$NAME"' --version
  grep -h ExecStart /usr/lib/systemd/system/'"$NAME"'*.service
  cat .SRCINFO
'
tar -C "$work" -czf "$out/$NAME-bin-$VERSION-aur.tar.gz" PKGBUILD .SRCINFO
ls -l "$out/$NAME-bin-$VERSION-aur.tar.gz"
