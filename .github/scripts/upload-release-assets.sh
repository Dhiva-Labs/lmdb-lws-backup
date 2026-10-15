#!/usr/bin/env bash
# Completes a GitHub release with the binaries CI built.
#
#  - A binary whose name is already on the release is left alone (a
#    hand-uploaded file wins); nothing is ever overwritten with --clobber.
#  - SHA256SUMS is made to list every binary of this release. An existing
#    SHA256SUMS keeps its lines verbatim; only missing lines are appended
#    (the file is then replaced by that superset). Hashes of binaries that
#    were not uploaded by this run are taken from the files on the release.
#
# usage: upload-release-assets.sh <tag> <dir with lmdb-lws-backup-*-linux-*>
# env:   GH_TOKEN (and GH_REPO or a git checkout) for the gh CLI
set -euo pipefail

tag=$1
dir=$2

release_assets() { gh release view "$tag" --json assets --jq '.assets[].name'; }

shopt -s nullglob
files=("$dir"/lmdb-lws-backup-*-linux-*)
if [ "${#files[@]}" -eq 0 ]; then
  echo "::error::no lmdb-lws-backup-*-linux-* files in $dir"
  exit 1
fi

present=$(release_assets)
for f in "${files[@]}"; do
  name=$(basename "$f")
  if grep -Fxq -- "$name" <<<"$present"; then
    echo "skip $name: already on release $tag"
  else
    echo "upload $name"
    gh release upload "$tag" "$f"
  fi
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/bin"
sums="$work/SHA256SUMS"

had_sums=0
if grep -Fxq SHA256SUMS <<<"$(release_assets)"; then
  had_sums=1
  gh release download "$tag" --pattern SHA256SUMS --dir "$work"
else
  : > "$sums"
fi
# Keep an existing last line intact even when it has no trailing newline.
if [ -s "$sums" ] && [ -n "$(tail -c 1 "$sums")" ]; then echo >> "$sums"; fi

appended=0
for f in "${files[@]}"; do
  name=$(basename "$f")
  if awk -v n="$name" '$2 == n { found = 1 } END { exit !found }' "$sums"; then
    echo "SHA256SUMS already lists $name"
    continue
  fi
  gh release download "$tag" --pattern "$name" --dir "$work/bin"
  (cd "$work/bin" && sha256sum "$name") >> "$sums"
  appended=1
done

if [ "$appended" -eq 1 ]; then
  if [ "$had_sums" -eq 1 ]; then
    gh release delete-asset "$tag" SHA256SUMS --yes
  fi
  gh release upload "$tag" "$sums"
fi

echo "--- SHA256SUMS on $tag"
cat "$sums"
