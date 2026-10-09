# lmdb-lws-backup

Encrypted daily hot backups for any running LMDB environment, with a
flagship profile for [monero-lws](https://github.com/vtnerd/monero-lws).
Single static-leaning C++ binary + one TOML config file.

LMDB databases routinely hold secrets, and the monero-lws database stores
**plaintext Monero view keys**. This tool treats every backup artifact as
exactly as sensitive as the live database: plaintext never touches
persistent storage at any point in the pipeline.

## How it works

```
live LMDB env ──mdb_env_copyfd2(MDB_CP_COMPACT)──▶ pipe ──▶ XChaCha20-Poly1305
   (read-only snapshot txn, never blocks the writer)          secretstream
                                                                    │
                                          <prefix>-YYYYMMDD.lmdbbak.enc.partial
                                                                    │
                        decrypt into anonymous RAM (memfd), open as LMDB,
                        walk every table; the monero-lws profile also parses
                        the accounts table, and the manifest detail
                        follows the profile (see Profiles)           ── verify
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
  any schema migration. Only the manifest step reads rows: every profile
  counts tables and rows, and the monero-lws profile additionally parses
  `accounts_v1_by_status,id`, whose 144-byte layout is static-asserted
  against upstream `src/db/data.h`.

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

The example config targets monero-lws. For any other LMDB database see
[Backing up other LMDB databases](#backing-up-other-lmdb-databases).

## Profiles

A profile decides what verify understands and what the manifest records.
It never changes the backup itself: under every profile the encrypted file
is the same compacted page-level snapshot. Set it with `[source] profile`
or override it per run with `--profile <auto|monero-lws|generic>`.

| Profile | Verify | Manifest |
|---|---|---|
| `auto` (default) | walks every table of the decrypted snapshot; if `accounts_v1_by_status,id` is present it behaves as `monero-lws`, otherwise as `generic` | whatever the resolved profile records |
| `monero-lws` | as `generic`, plus a strict walk of the accounts table (every row must be exactly 144 bytes); **fails if the table is missing** | account count, scan-height min/max, table count, total rows |
| `generic` | fully schema-blind: walks every table and never interprets a row | table count and total rows only |

- Detection happens on the snapshot, not the live database, and the
  manifest always records the *resolved* profile (`monero-lws` or
  `generic`), never `auto`.
- A monero-lws deployment needs no config change: `auto` finds the accounts
  table and behaves as before. Pin `profile = "monero-lws"` if you want a
  missing accounts table to fail verify (so the backup is not promoted)
  instead of quietly falling back to `generic`.
- `--once`, `--verify`, and `--restore` all resolve the profile the same way,
  from the config or `--profile`; a pinned `monero-lws` therefore also
  applies when you verify or restore an old backup by hand.
- `[source] max_named_dbs` (default `128`, range `1..4096`) is the number of
  named sub-databases the tool will open. LMDB needs that declared up front;
  the default is far above monero-lws's 13 tables. If a database has more,
  the walk stops with an error telling you to raise it.
- `[backup] filename_prefix` (default `lws-backup`) names the files
  `<prefix>-YYYYMMDD.lmdbbak.enc` and `<prefix>-YYYYMMDD.manifest.json`. The
  tool adds the dash before the date, so the prefix has no trailing dash. It
  must be 1 to 100 characters from `A-Za-z0-9._-` and must not start with
  `.` or `-`. Retention, `--list`, and orphan-manifest cleanup only touch
  files carrying the configured prefix, so changing it on an existing
  destination leaves the old files alone, outside retention.

## Backing up other LMDB databases

Nothing about the pipeline is specific to monero-lws. A minimal config for
an arbitrary LMDB environment (a directory holding `data.mdb` and
`lock.mdb`):

```toml
[source]
db_path = "/var/lib/myapp/db"
profile = "generic"              # or leave unset; auto falls back to generic

[backup]
destination_dir = "/var/backups/myapp"
filename_prefix = "myapp"        # myapp-YYYYMMDD.lmdbbak.enc

[encryption]
key_env_var = "LWS_BACKUP_KEY"
```

```bash
LWS_BACKUP_KEY="<base64-key>" lmdb-lws-backup --config myapp.toml --once
lmdb-lws-backup --config myapp.toml --list
```

Everything else (scheduling, retention, verify, restore, alerting) works as
described below. The manifest for such a backup records the table count and
total row count in place of account counts. If the application has many
named sub-databases, raise `max_named_dbs`. Give each database its own
`filename_prefix`, and preferably its own `destination_dir`: the sweep for
leftover `*.partial` files at the start of a cycle covers the whole
destination directory regardless of prefix, so two databases sharing one
directory must never back up at the same time.

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
| `--list` | table of retained backups with age, size, and manifest summary |
| `--restore <file> --to <dir>` | decrypt to a plain LMDB env (`<dir>/data.mdb`) |

`--db-path`, `--destination`, `--retention-days`, and `--profile` override
the config; `--config` defaults to `/etc/lmdb-lws-backup/config.toml`.

Exit codes: `0` success, `1` runtime/verify failure, `2` usage or config
error. Failures also hit the optional `[alerting] on_failure_webhook`.

## Restore procedure

```bash
lmdb-lws-backup --config ... --restore \
    /var/backups/monero-lws/lws-backup-20260810.lmdbbak.enc --to /tmp/recovered
```

This produces a plain **decrypted** LMDB environment (view keys included,
for monero-lws) at `/tmp/recovered/`, sanity-opens it, and prints a summary:
table and row counts, plus the account count and scan heights under the
monero-lws profile. Pointing monero-lws (or your application) at it is
deliberately left as a manual operator step.

## Operational notes

- **The service user needs write access to `lock.mdb`** in the source DB
  directory — LMDB registers read transactions in the lock file even for
  read-only access. Read access to `data.mdb` is not enough. The systemd
  units grant this via `BindPaths`; for cron, put the backup user in the
  monero-lws group and make `lock.mdb` group-writable. The same holds for
  any other application's environment.
- **`db_path` may name the `data.mdb` file; it is treated as its
  directory.** If `db_path` is a file called exactly `data.mdb` and a
  `lock.mdb` sits next to it, the tool opens the parent directory as the
  environment instead (logged at debug level). Opening `data.mdb` directly
  would make LMDB coordinate through a separate `data.mdb-lock` file that
  the live writer, which uses `lock.mdb`, never sees: two lock protocols on
  one database, and reads can be torn. Any other regular file is opened as
  a single-file environment (`MDB_NOSUBDIR`, lock file `<file>-lock`), and a
  path that does not exist is passed through so you get the normal open
  error. If **both** `lock.mdb` and `data.mdb-lock` exist the layout is
  ambiguous — the live writer could be using either lock file — and the tool
  refuses to open rather than guess (a wrong guess would make its reader
  slot invisible to the writer and the snapshot could be torn). Delete the
  stale lock file, i.e. the one no running process holds open, and rerun.
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
- The manifest (`<prefix>-YYYYMMDD.manifest.json`) holds only the profile,
  counts (tables and rows; accounts and scan heights under `monero-lws`),
  sizes, and the ciphertext SHA-256 — never key material. Neither do the
  logs.
- **Upgrading from 0.1.x needs no config change.** The defaults reproduce
  the old behavior: `lws-backup-` filenames and the monero-lws account
  checks (via `auto`). Existing backups and manifests, including those
  without a `profile` field, stay readable by `--verify` and `--list`, and
  retention keeps managing them.
- Backups are `0600`, as are the log, manifests, and key files (the tool
  refuses key files with looser modes).

## Layout

```
src/
  hot_copy.{h,cpp}    RAII LMDB env/txn wrappers, env path resolution,
                      reader_check, copyfd2
  crypto.{h,cpp}      libsodium secretstream framing, key loading, sha256
  manifest.{h,cpp}    generic table walk, lws accounts walk (144-byte
                      mirror), JSON
  retention.{h,cpp}   prefix-aware filename dating, dual-condition pruning,
                      partial cleanup
  backup.{h,cpp}      the --once pipeline, profile resolution, verify, restore
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
