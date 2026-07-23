# QuackCit — Work Handoff

A snapshot of the in-flight "more Citadel features / fuller IMAP / SMTP rework"
initiative: what's done, what remains, and everything needed to pick it up.

## Big picture

QuackCit is a **Citadel BBS/groupware server that runs inside DuckDB**. Each
protocol front-end is a loadable DuckDB extension; the message store is a set of
SQL tables (users → floors → rooms → messages). Mail protocols (SMTP/IMAP/POP3)
are gateways over the same Citadel room store. Extensions coordinate **through
DuckDB tables, not shared C++ state** ("the tables are the bus").

Layout: `core/` (shared plumbing, one static lib linked into every extension),
`citadel/ imap/ pop3/ smtp_in/ smtp_out/ managesieve/ quackmail/` (per-protocol
extensions), `test/` (sqllogictest + python integration), `deploy/`.

## The 3-phase initiative

Approved plan: deliver as three phased PRs.

| Phase | Scope | Status |
|---|---|---|
| **1. SMTP rework** | inbound MX + authenticated submission + relay drainer | ✅ **done** — PR #6 (`smtp-rework` → `main`) |
| **2. IMAP expansion** | STARTTLS+AUTHENTICATE, core RFC3501, folders + public rooms, extensions | ⬜ todo |
| **3. Citadel features** | presence/IM, message ops, admin/config, client niceties | ⬜ todo |

Scope guardrails (decided with the user): **ports stay non-privileged dev ports**
(the module defines the service, the port is just config); **nothing is exposed
to the internet** (all listeners bind `127.0.0.1`, dev self-signed TLS only, relay
tested against a local sink — no real cert, no live outbound-to-internet).

## Branch / PR state

- `main` contains the native-client interop fixes (`MSGP`, `GOTO` magic-alias
  resolution) — PR #5, merged.
- **PR #6** (`smtp-rework` → `main`): Phase 1 SMTP rework — open.

## Phase 1 — SMTP rework (done, see PR #6)

- **`smtp_in` = inbound MX** (2525): validates recipients at RCTP via
  `citadel::IsLocalUser` — unknown local user → `550 5.1.1`, foreign domain →
  `550 5.7.1` relay-denied; no AUTH; local delivery via `deliver::LocalDeliver`.
- **`smtp_out` = authenticated submission**: STARTTLS on 2587
  (`qm_smtp_submission_*`) + implicit-TLS on 2465 (`qm_smtp_smtps_*`), differing
  only in TLS mode; SASL AUTH required before MAIL; local rcpts delivered, remote
  rcpts enqueued to `quackmail_outbound`.
- **Relay drainer** (`qm_smtp_relay_start/_stop/_status`, named params
  `poll_secs`/`smarthost_host`/`smarthost_port`/`smarthost_user`/`smarthost_pass`):
  background thread draining the queue via smarthost or direct-to-MX with
  retry/backoff.
- New shared core: `deliver::LocalDeliver`, `sasl::ServerAuth`, `smtp::Deliver`
  (outbound client) + `tls::ClientContext`/`ClientStream::ConnectTls`,
  `dns::LookupMX` (links `resolv`), `citadel::IsLocalUser`, and outbound-queue
  helpers in `mail_store`.
- Tests: `test/integration/test_smtp_in.py` (rewritten), `test_submission.py`,
  `test_relay.py` — all pass alongside `test_citadel.py`.

## Deployment (server: mail.0011110.xyz)

- SSH: `ssh -i ~/.ssh/quackmail_dev admin@mail.0011110.xyz` (sudo NOPASSWD).
- Install: `/opt/quackcit/current/` (flat `.duckdb_extension` bundle + `duckdb`
  CLI); launcher `/opt/quackcit/run_quackcit.py`; **persistent** systemd unit
  `quackcit.service`. Runtime uses the venv python `~/venv/bin/python` (duckdb
  1.5.4). DB at `/opt/quackcit/quackcit.duckdb`. Demo user `alice`/`secret`.
- Live ports (all `127.0.0.1`): citadel **5040**, smtp-in **2525**, submission
  **2587** (STARTTLS), smtps **2465** (implicit TLS), pop3 **1110**, imap
  **1143**; relay drainer → local sink **2600** (`~/smtp_sink.py`, started via
  nohup — manual, not systemd).
- Rollback: prior copies saved as `*.pre-p1bak` / `*.bak` in `/opt/quackcit/current/`.

## Build & deploy loop (extensions are **Linux-only** — build on the server)

```bash
# on the server
cd ~/quackmail
git fetch origin && git reset --hard origin/<branch>     # source of truth
GEN=ninja make release                                    # incremental; ~600MB RAM + 8GB swap
# artifacts: build/release/extension/<name>/<name>.duckdb_extension
cp build/release/extension/<name>/<name>.duckdb_extension /opt/quackcit/current/<name>.duckdb_extension
# update /opt/quackcit/run_quackcit.py if adding listeners
sudo systemctl restart quackcit.service
```

Verify tools available on the box: `swaks` (SMTP), `nc` (native Citadel proto),
python `imaplib`/`poplib`/`smtplib`, the built text client
`/usr/local/bin/citadel` + expect harnesses in `~/cittest/drive*.exp`. Run
integration tests with `~/venv/bin/python test/integration/test_*.py` (they use
their own ports/DB, safe alongside the live service). `make test` runs
sqllogictest.

## Architecture cheat-sheet (for Phases 2 & 3)

- **Add a listener**: declare a global `ServerController`, write a
  `void Handle(DatabaseInstance&, net::ClientStream&)`, and
  `RegisterServerControls(loader, "<prefix>", <port>, g_ctrl, Handle)`. Implicit
  TLS is handled centrally in `ServerController::AcceptLoop`; STARTTLS is done in
  the handler via `ctrl.StartTlsEnabled()` / `ctrl.TlsCtx()` /
  `stream.StartTls(...)`. To listen on two ports, use two controllers + two thin
  handlers over a shared impl (see `smtp_out`).
- **`net::ClientStream`**: `ReadLine`/`WriteLine`/`ReadDotStuffed`/`Write`,
  `StartTls`/`AcceptTls`/`ConnectTls`, `IsTls`.
- **SASL**: `sasl::ServerAuth(con, mech, initial, challengeFn, &user)` — the
  challenge callback owns the wire framing (SMTP `334 `, IMAP `+ `). Reuse for
  IMAP `AUTHENTICATE`.
- **Store** (`core/include/quackmail/`): `citadel_store.hpp`
  (rooms/floors/messages/last-read/users, `IsLocalUser`), `mail_store.hpp`
  (schema + outbound-queue helpers), `citadel_msg.hpp` (`FormatMsg0`/`BodyText`),
  `mime.hpp` (`Parse`, and `ParseEntity`/`FlattenParts`/`ParseContentType` for
  IMAP `BODYSTRUCTURE`/part fetch), `delivery.hpp` (`LocalDeliver`).
- **Cross-session features** (IMAP IDLE, Citadel `SEXP`/`RWHO`): use DuckDB tables
  (planned `citadel_sessions`, `citadel_express`), consistent with the "tables
  are the bus" design — do not add shared in-process state.
- **Gotcha**: the extension build treats structs with default member initializers
  as non-aggregates, so brace-init like `Foo{a, b}` needs an explicit constructor
  (see `dns::MxHost`).
- **New module**: create `<mod>/CMakeLists.txt` from the standard template, add a
  `duckdb_extension_load(...)` entry in `extension_config.cmake`, and add any new
  `core/src/*.cpp` to the single-compile list in `core/quackmail_core.cmake`.

## Phase 2 — IMAP expansion (`imap/src/quackmail_imap_extension.cpp`, `HandleImap`)

Today it's a minimal subset (LOGIN, CAPABILITY=IMAP4rev1 only, LIST/LSUB,
SELECT/EXAMINE, FETCH, STORE, EXPUNGE, CLOSE; UIDVALIDITY hardcoded to 1; only the
user's own mailbox rooms are listed; implicit-TLS only, no STARTTLS/AUTHENTICATE).
Planned:

- **Security**: `STARTTLS` command, `AUTHENTICATE PLAIN/LOGIN` (via `sasl`),
  CAPABILITY advertising `STARTTLS` / `LOGINDISABLED` (pre-TLS) / `AUTH=...`.
- **Core RFC 3501**: `STATUS`, `SEARCH`/`UID SEARCH` (searchable subset), `APPEND`,
  `COPY` + `MOVE`, `CLOSE` auto-expunge, real per-room **UIDVALIDITY** (new
  `citadel_rooms.uidvalidity` column), correct RECENT/PERMANENTFLAGS.
- **Folders + public rooms**: `CREATE/DELETE/RENAME/SUBSCRIBE/UNSUBSCRIBE` (new
  `citadel_subscriptions` table), and expose public rooms (Lobby/Aide/floors) in
  LIST with floor-path names (extend `ResolveMailbox` and the LIST query).
- **Extensions**: `IDLE`, `NAMESPACE`, `UIDPLUS` (APPENDUID/COPYUID),
  `BODYSTRUCTURE` + `BODY[<part>]` / `BODY[HEADER.FIELDS (...)]` / partial fetch
  (reuse the core MIME tree).
- Add store helpers for message copy/move/delete + flag set/clear/replace (IMAP
  STORE currently uses inline SQL against `citadel_msg_flags`).
- Verify: python `imaplib` + `openssl s_client`; add `test/integration/test_imap.py`.

## Phase 3 — Citadel features (`citadel/src/quackmail_citadel_extension.cpp`, `HandleCitadel`)

- **Presence + IM**: `citadel_sessions(session_id, username, host, since,
  last_seen)` + `RWHO`; `SEXP`/`GEXP` via `citadel_express(id, to_user, from_user,
  text, sent_at)`.
- **Message ops**: `MOVE`, `DELE` (delete a message — distinct from `KILL`, which
  deletes rooms), `EXPI` — needs new `DeleteMessage`/`MoveMessage` store helpers.
- **Admin/config** (aide-gated, axlevel ≥ 6): `CONF`, `TERM`, `DOWN`/`SCDN`.
- **Client niceties**: `CHEK`, distinct `MSG4` MIME rendering (currently aliased
  to MSG0), `GTSN`/`STSN` generalized seen-set.
- Verify: text client + native probes (two concurrent sessions for
  RWHO/SEXP/GEXP); extend `test/integration/test_citadel.py` and
  `test/sql/citadel.test`.

## Known notes

- The official Citadel text client now completes a session against QuackCit
  (MSGP + GOTO fixes on `main`). One cosmetic nit: it prints a single
  `Unrecognized or unsupported command.` for a handshake verb right after the
  banner — non-blocking, login proceeds fine.
- `qm_version()` / the greeting still report `0.1.0` even in the v0.2.1 release —
  the internal version constant was never bumped.
