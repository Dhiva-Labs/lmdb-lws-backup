# lmdb-lws-backup

Encrypted daily hot backups for a running [monero-lws](https://github.com/vtnerd/monero-lws)
LMDB database. Single static-leaning C++ binary + one TOML config file.

The monero-lws database stores **plaintext Monero view keys**. This tool
treats every backup artifact as exactly as sensitive as the live database:
plaintext never touches persistent storage at any point in the pipeline.

## How it works

```
live monero-lws DB ──mdb_env_copyfd2(MDB_CP_COMPACT)──▶ pipe ──▶ XChaCha20-Poly1305
   (read-only snapshot txn, never blocks the scanner)          secretstream
                                                                    │
                                              lws-backup-YYYYMMDD.lmdbbak.enc.partial
                                                                    │
                        decrypt into anonymous RAM (memfd), open as LMDB,
                        walk the accounts table + every other table  ── verify
                                                                    │
                                     atomic rename to .enc + .manifest.json
                                                                    │
                                          retention: prune to newest 30 days
```

Key properties:

- **Never interferes with the live server.** The source env is opened
  read-only (`MDB_RDONLY`), the copy uses LMDB's MVCC snapshot semantics
  (readers never block the writer and vice versa), the map is never
  resized, and `mdb_reader_check()` runs before every backup to clear
  stale reader slots from crashed runs.
- **Plaintext never rests on disk.** The compacting copy streams through a
  pipe directly into the authenticated encryption stream; the post-backup
  verify decrypts into an unlinked `memfd` (RAM) and opens it via
  `/proc/self/fd`. A crash at any point leaves nothing to shred.
- **A bad backup can never replace or delete a good one.** The encrypted
  stream is only finalized if the LMDB copy succeeded; the `.partial` file
  is only renamed into place after it decrypts, opens, and fully walks
  clean; retention runs only after that, deletes oldest-first, and only
  files that are both past the retention age *and* outside the newest-N
  set — an outage can never cause a mass deletion.
- **Schema-blind.** The backup is a raw page-level snapshot, correct across
  monero-lws table-version migrations. Only the manifest step parses rows,
  only from `accounts_v1_by_status,id`, whose 144-byte layout is
  static-asserted against upstream `src/db/data.h`.

## Build

Requires: gcc 10+/clang, CMake ≥ 3.16. LMDB 0.9.33 and toml++ are vendored.
libsodium is either taken from the system (`libsodium-dev`) or built
statically with the included script:

```bash
./scripts/fetch-libsodium.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build          # unit + integration tests
sudo install -m 755 build/lmdb-lws-backup /usr/local/bin/
```

## Quick start

```bash
# 1. Generate a key (KEEP THIS SAFE — without it every backup is noise):
head -c 32 /dev/urandom | base64

# 2. Configure:
sudo mkdir -p /etc/lmdb-lws-backup
sudo cp config.example.toml /etc/lmdb-lws-backup/config.toml
sudoedit /etc/lmdb-lws-backup/config.toml

# 3. Run one cycle:
LWS_BACKUP_KEY="<base64-key>" lmdb-lws-backup \
    --config /etc/lmdb-lws-backup/config.toml --once
```

## Scheduling (recommended: systemd timer)

```bash
sudo install -m 600 /dev/null /etc/lmdb-lws-backup/key.env
# put one line in it:  LWS_BACKUP_KEY=<base64-key>
sudo cp systemd/lmdb-lws-backup.{service,timer} /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now lmdb-lws-backup.timer
```

Plain cron works identically (`--once` is the whole cycle). For
environments without any scheduler, `--daemon` runs the same cycle from an
internal daily scheduler (`schedule_time`/`timezone` in the config) and
shuts down cleanly on SIGTERM/SIGINT — see
`systemd/lmdb-lws-backup-daemon.service`.

## CLI

| Command | Purpose |
|---|---|
| `--once` | one full backup cycle (copy → encrypt → verify → retention), exit 0/1 |
| `--daemon` | loop forever, one cycle daily at `schedule_time` |
| `--verify <file>` | decrypt + integrity-walk an existing backup, cross-check its manifest |
| `--list` | table of retained backups with age, size, account counts |
| `--restore <file> --to <dir>` | decrypt to a plain LMDB env (`<dir>/data.mdb`) |

`--db-path`, `--destination`, and `--retention-days` override the config;
`--config` defaults to `/etc/lmdb-lws-backup/config.toml`.

Exit codes: `0` success, `1` runtime/verify failure, `2` usage or config
error. Failures also hit the optional `[alerting] on_failure_webhook`.

## Restore procedure

```bash
lmdb-lws-backup --config ... --restore \
    /var/backups/monero-lws/lws-backup-20260810.lmdbbak.enc --to /tmp/recovered
```

This produces a plain **decrypted** LMDB environment (view keys included)
at `/tmp/recovered/`, sanity-opens it, and prints the account summary.
Pointing monero-lws at it (`--db-path`) is deliberately left as a manual
operator step.

## Operational notes

- **The service user needs write access to `lock.mdb`** in the source DB
  directory — LMDB registers read transactions in the lock file even for
  read-only access. Read access to `data.mdb` is not enough. The systemd
  units grant this via `BindPaths`; for cron, put the backup user in the
  monero-lws group and make `lock.mdb` group-writable.
- **Verify needs RAM** roughly equal to the compacted DB size (the decrypted
  copy lives in an unlinked memfd). If that's not viable, `[verify]
  scratch_dir` trades the RAM requirement for transient plaintext on a
  filesystem you choose — use an encrypted volume.
- During a long copy the live DB cannot recycle free pages, so `data.mdb`
  may grow while the backup streams. This is inherent to LMDB snapshots and
  harmless; the space is reused afterwards.
- Re-running `--once` on the same day atomically replaces that day's backup
  (same filename) — the old file is only replaced after the new one
  verifies.
- The manifest (`lws-backup-YYYYMMDD.manifest.json`) holds only counts,
  heights, sizes, and the ciphertext SHA-256 — never key material. Neither
  do the logs.
- Backups are `0600`, as are the log, manifests, and key files (the tool
  refuses key files with looser modes).

## Layout

```
src/
  hot_copy.{h,cpp}    RAII LMDB env/txn wrappers, reader_check, copyfd2
  crypto.{h,cpp}      libsodium secretstream framing, key loading, sha256
  manifest.{h,cpp}    accounts walk (144-byte mirror), schema-blind full walk, JSON
  retention.{h,cpp}   filename dating, dual-condition pruning, partial cleanup
  backup.{h,cpp}      the --once pipeline, verify, restore
  scheduler.{h,cpp}   --daemon loop, timezone handling, signal self-pipe
  config.{h,cpp}      toml++ loading + validation
  cli.{h,cpp}         argument parsing and dispatch
  alert.{h,cpp}       failure webhook via curl
tests/                one executable per module + end-to-end integration test
third_party/          vendored liblmdb 0.9.33, toml++ 3.4.0 (+ local libsodium build)
```

The integration test builds a synthetic monero-lws-shaped database (all 13
current tables, real 144-byte account rows), mutates it from a writer
thread for the whole test, and drives the installed binary through backup,
verify, corruption detection, retention, restore, and daemon shutdown while
asserting the reader table never leaks a slot.
