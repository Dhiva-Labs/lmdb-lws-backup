#!/usr/bin/env bash
# Runs inside each distribution container (in-container.sh) against the
# installed package: backs up a small LMDB environment with the packaged
# binary, verifies and lists the encrypted backup, restores it and checks
# the restored data.
set -euo pipefail
lmdb-lws-backup --version
work="$(mktemp -d)"
mkdir -p "$work/db"
# One key ("key" -> "value") in the main database, in mdb_dump's format.
printf 'VERSION=3\nformat=bytevalue\ntype=btree\nHEADER=END\n 6b6579\n 76616c7565\nDATA=END\n' \
  | mdb_load "$work/db"
cat > "$work/config.toml" <<CONF
[source]
db_path = "$work/db"
profile = "generic"

[backup]
destination_dir = "$work/backups"
filename_prefix = "smoke"
retention_days = 30

[encryption]
key_env_var = "LWS_BACKUP_KEY"

[logging]
log_path = "$work/backup.log"
level = "info"
CONF
LWS_BACKUP_KEY="$(head -c 32 /dev/urandom | base64)"
export LWS_BACKUP_KEY
lmdb-lws-backup --config "$work/config.toml" --once
ls -l "$work/backups"
backup="$(ls "$work"/backups/smoke-*.lmdbbak.enc)"
lmdb-lws-backup --config "$work/config.toml" --verify "$backup"
lmdb-lws-backup --config "$work/config.toml" --list
lmdb-lws-backup --config "$work/config.toml" --restore "$backup" --to "$work/restored"
mdb_dump "$work/restored" | tee "$work/restored.dump"
grep -qx ' 76616c7565' "$work/restored.dump"
echo "backup, verify, list and restore: OK"
