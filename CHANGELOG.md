# Changelog

## 0.2.1 - 2026-10-16

- Added: Linux arm64 (aarch64) binary, `lmdb-lws-backup-<version>-linux-aarch64`,
  next to the x86_64 one, for arm64 monero-lws servers (Raspberry Pi 4/5, Ampere
  and Graviton instances). CI builds libsodium, builds the tool, runs the unit and
  end-to-end integration tests, and checks the binary's architecture and linking
  natively on both x86_64 and arm64 (Ubuntu 24.04).
- Added: when a release is published, CI attaches the binaries that are missing
  from it and a `SHA256SUMS` covering all of them. A file already on the release
  is never replaced.
- Docs: the README states that the prebuilt binaries need glibc 2.38 or newer.
- No changes to backup, verify or restore behaviour, the file formats or the
  configuration.

Earlier releases are listed on the
[releases page](https://github.com/Dhiva-Labs/lmdb-lws-backup/releases).
