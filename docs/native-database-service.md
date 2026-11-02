# Running the DVMDB HTTP service

`aowis-server-dvmdb` serves every project of one storage root over plain HTTP
on a configurable address and port. It is a thin adapter over
`aowis::dvmdb::ProjectService`; the routes and payloads are in the
[OpenAPI contract](dvmdb-http-api.yaml) and the [REST guide](rest-api-design.md).
For backups, recovery and maintenance see [operations](operations.md).

**There is no TLS, authentication or authorization.** Keep the listener on
localhost or a trusted network.

## Quick start

```bash
./compile_linux.sh                 # build Release and start the server in the foreground
curl -fsS http://127.0.0.1:8755/dvmdb/v1/ready      # {"status":"ready"}
curl -fsS http://127.0.0.1:8755/dvmdb/v1/projects   # {"projects":[]} on a new root
```

`./compile_linux.sh --build-only` builds without starting anything. The script
starts the server only if configuration, the full build and the server target
all succeeded, so a failed build never falls back to an older binary.
`SIGINT`/`SIGTERM` closes the listener and lets running requests finish.

## Storage root

The root is chosen in this order:

1. `--data-root DIR`
2. `[storage] root=` in the INI file
3. `./aowis-dvmdb-data`, if it exists in the working directory
4. `$XDG_DATA_HOME/aowis-dvmdb/` (normally `~/.local/share/aowis-dvmdb/`)

The server prints `DVMDB storage root: <path>` at startup. Roots are never
moved or merged automatically; if both 3 and 4 exist, 3 wins and the other is
reported. An empty project list usually means the wrong root, not lost data.
Only one process can own a root (`catalogue.lock`); a second server, or an
embedded library instance on the same root, fails to start with a conflict.

## Configuration

Options on the command line override the INI file (`-c FILE`, default
`aowis-server-dvmdb.ini` in the working directory).

| INI key | Option | Default | Range / meaning |
|---|---|---|---|
| `[server] bind` | `-b`, `--bind` | `127.0.0.1` | numeric IPv4 or IPv6 address |
| `[server] port` | `-p`, `--port` | `8755` | 1–65535 |
| `[server] worker_threads` | `--worker-threads` | `8` | 1–256 HTTP workers; up to 16 queued connections per worker, further ones are closed |
| `[server] max_pending_per_project` | `--max-pending-per-project` | half the workers | 1–workers; operations of one project that may run or wait; beyond that 503 `busy` |
| `[storage] root` | `-d`, `--data-root` | see above | storage root |
| `[storage] max_internal_scratch_mib` | `--max-internal-scratch-mib` | `1024` | 1–16384; process-wide cap on import/export/verification scratch; beyond that 503 `scratch_busy` |
| `[sync] max_transfer_mib` | `--sync-max-mib` | `32` | 1–256; largest bundle upload or download |
| `[sync] max_objects` | `--sync-max-objects` | `100000` | 1–1,000,000 objects per bundle |
| `[sync] max_object_mib` | `--sync-max-object-mib` | `8` | 1–64, at most `max_transfer_mib` |
| `[sync] max_inflight_mib` | `--sync-max-inflight-mib` | max(128, 2 × transfer) | transfer..4096; sum of reserved transfer staging over all projects; beyond that 503 `staging_busy` |
| `[logging] file` | `--log-file` | stderr | append diagnostics to a file |

Invalid values stop the server with exit code 2.

### Request limits

| Request | Limit |
|---|---|
| JSON bodies for projects, branches, role, drafts, leases, release, discard, initialize-network | 16 KiB |
| Merge requests, local Commit and revision display-message updates | 256 KiB |
| Draft saves/imports, network saves, entity edits, model commits | 8 MiB |
| Bundle upload | `max_transfer_mib` |
| Changes per batch, events per Save, merge choices | 128 |
| History, revision graph, draft list page | 1–1000 items |
| Entity ID list | 10,000; diff 1,000 differences |

JSON bodies must declare `Content-Length` (411 otherwise) and are rejected with
413 before the body is read when they are too large. Read timeout 30 s, write
timeout 60 s, keep-alive 10 s.

## Deployment

`deploy/` contains:

* `aowis-server-dvmdb.ini` — production settings (`/var/lib/aowis-dvmdb`);
  install it as `/etc/aowis/aowis-server-dvmdb.ini`.
* `aowis-server-dvmdb.service` — a hardened systemd unit (dedicated user,
  `StateDirectory=aowis-dvmdb`, read-only system, private tmp, `SIGTERM` with
  60 s stop timeout, journald logging).
* `nginx-aowis-server-dvmdb.conf` — an optional TLS-terminating reverse proxy
  for `/dvmdb/`. Keep `client_max_body_size` equal to `max_transfer_mib`. TLS
  protects the connection only; it does not add access control.

## Health

| Route | Meaning |
|---|---|
| `GET /dvmdb/v1/health` | the process answers (`{"status":"ok"}`) |
| `GET /dvmdb/v1/ready` | the catalogue can be read (`{"status":"ready"}`); 503 when the service refuses work, e.g. after an uncertain publication that requires a restart |
| `GET /dvmdb/v1/repository` | the root's repository UUIDv7 |

When the service opens a project and finds a torn pack tail, it logs how many
bytes were set aside in `recovered-pack-tails/`.

## Errors

Every error response is `{"error": {"code", "message"}}`:

| Status | `code` | Cause |
|---|---|---|
| 400 | `invalid_request` | malformed or unknown fields, invalid UUIDs or names, validation failure |
| 404 | `not_found` | unknown project, branch, revision, draft, network or route |
| 409 | `conflict` | stale expected HEAD, generation, lease or token; ID reused for something else; divergent import |
| 409 | (merge body) | a merge with unresolved conflicts returns the merge result with status 409 |
| 411 | `length_required` | JSON body without `Content-Length` |
| 413 | `limit_exceeded`, `transfer_too_large` | body, batch, result or draft limit exceeded |
| 414 / 431 | `uri_too_long`, `header_too_large` | request line or headers too large |
| 503 | `busy` | the project's operation queue is full (`Retry-After: 1`) |
| 503 | `staging_busy` | all bundle-transfer staging is reserved (`Retry-After: 1`) |
| 503 | `scratch_busy` | the internal scratch cap is reached (`Retry-After: 1`) |
| 503 | `unavailable` | a repository must be reopened, or the service needs a restart |
| 507 | `insufficient_storage` | the filesystem reported ENOSPC or EDQUOT |
| 500 | `internal_error` | corruption, I/O or internal failure |

4xx messages describe the problem. 5xx messages are generic; the details
(paths, errno, corruption) go only to the service log. A 5xx or a lost
response after a write may mean the write happened: read the branch HEAD or the
draft generation before retrying, and use caller-chosen IDs where a route
offers them.
