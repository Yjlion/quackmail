# CLAUDE.md — working in this repo

QuackCit is a **Citadel BBS/groupware server that runs inside DuckDB**. Every
protocol front-end is a loadable DuckDB extension; the message store is a set of
SQL tables (users → floors → rooms → messages). Mail protocols (SMTP/IMAP/POP3)
are gateways over the same Citadel room store.

**Extensions never share C++ state — they coordinate through DuckDB tables.**
"The tables are the bus." If you need cross-session state (IDLE, presence,
instant messages), add a table, not a global.

See [MEMORY.md](MEMORY.md) for project history and decisions, [TODO.md](TODO.md)
for what's next, and [docs/](docs/README.md) for the user-facing documentation
(`docs/contributing.md` is this file written for a person — keep the two in
step).

## Layout

| Path | What |
|---|---|
| `core/` | shared plumbing compiled **once** into a static lib and linked into every extension |
| `citadel/` | native Citadel protocol (the centerpiece) |
| `imap/ pop3/ smtp_in/ smtp_out/ managesieve/` | mail front-ends |
| `quackmail/` | umbrella extension: schema init, users, room/floor admin, MIME helpers |
| `spool/` | the timer-driven half: mailing-list distribution and remote message pulls. No listener |
| `test/sql/` | sqllogictest (`make test`) |
| `test/integration/` | python end-to-end tests, one per protocol |
| `test/parity/` | captured output from the real Citadel server used as fixtures |
| `deploy/` | POSIX shell only, no interpreter: `quackcit.sh` + `quackcit.conf` (run it), `quackcitadm.sh` (administer it), `quackcit_common.sh` (shared: layout detection, the listener table, SQL quoting, the control channel), `quackcit.service` |

## Build and test

Extensions are **Linux-only** (POSIX sockets, system OpenSSL). Build on a Linux
checkout, not on the Windows one.

```bash
GEN=ninja make release     # artifacts: build/release/extension/<name>/<name>.duckdb_extension
make test                  # sqllogictest
python3 test/integration/test_<protocol>.py
```

The integration tests import the `duckdb` Python module; a few want extras
(`jmapc` for `test_jmap_client.py`, which skips itself without it). Keep them in
a virtualenv — `~/venv` is the convention:

```bash
python3 -m venv ~/venv && ~/venv/bin/pip install 'duckdb==1.5.4' jmapc
```

RAM, not cores, is the constraint on the DuckDB build. Cap it:

```bash
GEN=ninja CMAKE_BUILD_PARALLEL_LEVEL=2 make release
```

The first DuckDB build takes ~20–40 minutes; incremental rebuilds are fast,
**except** that anything under `core/` is compiled into all twelve extensions,
so touching `core/src/*.cpp` or a header they share is a twelve-way rebuild.

## Adding a listener

1. Declare a global `ServerController`.
2. Write `void Handle(DatabaseInstance &db, net::ClientStream &stream)`. If the
   handler needs STARTTLS, give it a `ServerController &` parameter and add a
   thin wrapper (see `HandleImap` / `HandlePop3`).
3. `RegisterServerControls(loader, "<prefix>", <default_port>, g_ctrl, Handle)`
   registers `<prefix>_start/_stop/_status` with the `tls_cert`, `tls_key`,
   `implicit_tls` and `starttls` named parameters.

Implicit TLS is handled centrally in `ServerController::AcceptLoop`. STARTTLS is
done in the handler: `ctrl.StartTlsEnabled()` → `stream.StartTls(ctrl.TlsCtx(), err)`.
Two ports for one protocol = two controllers + two thin wrappers over one impl
(`smtp_out`, `pop3`).

## Adding a module

Besides `<mod>/src/*` you must touch **four** files — the last one is easy to miss:

1. `<mod>/CMakeLists.txt` — copy `pop3/CMakeLists.txt` verbatim, change the name.
2. `extension_config.cmake` — add a `duckdb_extension_load(...)` entry.
3. `core/quackmail_core.cmake` — add any new `core/src/*.cpp` to the single
   compile list (core is compiled exactly once to avoid duplicate symbols).
4. `.github/workflows/release.yml` — add the extension to the hardcoded
   `for ext in ...` packaging loop, or it won't ship in releases.

Then add it to the `quackcit_services` table in `deploy/quackcit_common.sh`,
`test/sql/quackmail.test`, and the table in `docs/protocols.md`.

## Adding a background worker

Not everything is a listener. For anything on a timer, declare a
`PeriodicWorker` (`core/include/quackmail/worker.hpp`), write a tick taking a
`Connection &`, and register `<prefix>_start/_stop/_status` — `spool/` has the
registration helper. Then add a row to `quackcit_workers` in
`deploy/quackcit_common.sh`, which is what actually starts it on a real install.

Always give a worker a one-shot `_run` table function beside its controls. That
is what makes it testable: an integration test calls the run function and
asserts, instead of sleeping past a poll interval.

## Reuse before you write

- `core/include/quackmail/citadel_store.hpp` — rooms, floors, messages,
  last-read, users: `ListRooms`, `ResolveRoom`, `GetRoomStats`, `RoomMessages`,
  `LoadMessage`, `InsertMessage`, `SetLastRead`, `GetOrCreateMailRoom`,
  `FindUserRoom`, `EnsureUserRooms`, `IsLocalUser`. Permissions:
  `EffectiveRights`/`SetRights`/`ListRights` (RFC 4314) and `CanPost` — every
  front-end that accepts a message must ask `CanPost`, never re-derive it.
  `PostAideMessage` is the server's log-to-the-BBS channel.
- `psl.hpp` — `PublicSuffix`/`RegistrableDomain` over the bundled Public Suffix
  List. `core/src/psl_data.cpp` is generated by `tools/gen_psl.py` and committed;
  the build must never fetch it.
- `citadel_msg.hpp` — `FormatMsg0` (native listing), `BodyText`,
  `RenderRfc822`/`MessageId` (the RFC822 view POP3/NNTP/IMAP serve).
- `mime.hpp` — `Parse`, `ParseEntity`, `FlattenParts`, `ParseContentType`.
- `auth.hpp` (`Verify`, `AddUser`), `sasl.hpp` (`ServerAuth` — the callback owns
  the wire framing), `delivery.hpp` (`LocalDeliver`), `smtp_client.hpp`, `dns.hpp`.
- `mailpolicy.hpp` — site policy for both SMTP front-ends: `IsLocalDomain`,
  `ExpandAlias`, `CheckAcl`, `RblZones`, `DkimKeyFor`/`DkimKeyLookup`,
  `CheckRate`/`RecordSend`, `GetEnforcement`, `LogInbound`.
- `spf.hpp`, `dkim.hpp` (sign, verify, keygen), `dmarc.hpp`, `rbl.hpp` — mail
  authentication. `dkim::Verify` takes an injectable `KeyLookup` so it can read
  locally stored keys instead of DNS; that is what makes it testable offline.
- `sieve.hpp` — RFC 5228 `Evaluate` (returns a *list* of actions with implicit
  keep) and `Check` for ManageSieve validation, plus `imap4flags` (flags ride on
  the KEEP/FILEINTO they were set for), `variables` and `vacation`. `Evaluate`
  takes no `Connection` and must stay that way: it decides only what *this
  message* deserves, so every RFC 5230 suppression rule is testable from
  `test/sql/sieve.test` through `qm_sieve_eval`. The per-correspondent window
  and the actual send are `delivery.cpp`'s, because only it has a connection.
- `listserv.hpp` — mailing lists over rooms: `ResolveAddress` (post vs. command
  address), `Subscribe`/`Unsubscribe`/`Confirm`, `RenderForList` (the List-*
  rewriting, pure enough to assert from SQL), `SpoolOnce`/`SpoolRoom`.
- `worker.hpp` — `PeriodicWorker`, the clock counterpart to `ServerController`.
  Anything on a timer uses it rather than growing its own thread and sleep loop.
  Register controls the same way a listener does; see `spool/`.
- `mail_client.hpp` (POP3/IMAP *clients*), `http_client.hpp`, `feed.hpp`
  (RSS/Atom over the existing `xmlstream` tokenizer), `fetch.hpp` (the model).
  `net::Connect` is the shared dialer, with a connect timeout — never write
  another `getaddrinfo`/`connect` pair.
- Cross-session state lives in `citadel_sessions` and `citadel_express`
  (already backing `RWHO` / `SEXP` / `GEXP`).

## Gotchas

- The extension build treats structs with default member initializers as
  non-aggregates: `Foo{a, b}` brace-init needs an explicit constructor.
- `citadel::GetOrAssignUserNum` returns 0 for unknown users (it does not create
  them) — use it to check whether a user exists.
- DuckDB prepared statements: check `HasError()` before `Execute`, and use
  `Cast<MaterializedQueryResult>()` for row access.
- DuckDB **table function arguments must be constant-foldable**, so table
  functions cannot be nested. Anything that needs to compose (`qm_dkim_sign` →
  `qm_dkim_verify`) has to be a scalar function. Numeric arguments should be
  declared `BIGINT`, not `VARCHAR` — DuckDB will not implicitly convert an
  `INTEGER` literal to `VARCHAR`, so a VARCHAR signature forces callers to
  quote every number.
- `store::EnsureSchema` runs from a table function's **init**, not from `LOAD`.
  The copy `LoadInternal` calls only partly takes effect — on a brand-new
  database `quackmail_users`, `citadel_users`, `citadel_rooms` and friends do
  *not* exist after `LOAD quackmail` alone, though `citadel_config` and
  `quackmail_sieve_scripts` do. Plain SQL against those tables is a catalog
  error until some `qm_*` function has run once, so any offline batch has to
  lead with one (`SELECT count(*) FROM qm_status()` warms everything).
- The umbrella's table functions open **their own `Connection`**, so rows
  written by sqllogictest's harness connection are invisible to them. Assert
  configuration in `test/sql/`, but assert *enforcement* (rate limits, delivery)
  in `test/integration/`. Protocol handlers are unaffected: each session does
  all its reads and writes on the one connection it opens.
- The admin CLI cannot open the database while the server is running — DuckDB
  permits one read-write process per file. `deploy/quackcitadm.sh` writes SQL
  into the server's control FIFO instead and reads the file the CLI redirects
  the result to; see the control-channel section of `deploy/quackcit_common.sh`.
  Errors are the one thing `.output` cannot capture, so the server's stderr has
  its own file and the bytes appended across a request are that request's error.
  This works only because no C++ in the tree writes to stderr — keep it that way.
- Deploy scripts are POSIX `sh` and run under `dash`; check with `dash -n`.
  Watch for `die` inside `$(...)`: it exits only the subshell, so validators
  like `sql_int` must be used in a plain assignment (`n=$(sql_int "$1")`) where
  `set -e` sees the failure.
- Kill the server by the PID in `quackcit.pid`. `pkill -f quackcit` over SSH
  kills your own shell (the pattern matches the command line).

## The parity oracle

A **real Citadel Groupware server** is the oracle. Source is the spec; live
probes are the acceptance test.

Get one anywhere with docker — this is the default:

```bash
docker run -d --name citadel -p 10504:504 -p 10025:25 -p 10110:110 \
  -p 10143:143 -p 10119:119 -p 10522:5222 -p 10080:80 \
  -v citadel-data:/citadel-data citadeldotorg/citadel
docker exec citadel /usr/local/citadel/sendcommand -h/citadel-data 'AGUP admin'
```

Real port + 10000, so nothing collides with QuackCit's dev ports (citadel 5040,
smtp 2525, submission 2587, submissions 2465, pop3 1110, pop3s 1995, imap 1143,
imaps 1993, xmpp 15222) or with the integration tests. `admin`/`citadel` exists
already; `sendcommand` needs `-h/citadel-data` or it looks in the wrong place.

If the `ai` user is not in the `docker` group, every command above needs
`sudo` (`sudo docker exec citadel ...`).

The container has **no source tree**, and neither does this machine. Read the
source over HTTP from **`https://code.citadel.org/citadel.git`** (cgit), which
serves the whole tree, history and patches:

```bash
curl -sS -b 'cgit_access=verified' \
  https://code.citadel.org/citadel.git/plain/libcitadel/lib/libcitadel.h
```

The `-b` cookie is not optional — the site gates on a JavaScript check that sets
it, and without it every path returns the challenge page instead of the file.
`citadel/server/modules/<proto>/` holds the protocol servers, `textclient/` the
BBS client, `webcit/` and `webcit-ng/` the web clients, and
`libcitadel/lib/libcitadel.h` the shared constants and enums.

`debian.lan`, named throughout [MEMORY.md](MEMORY.md) as a second oracle
carrying a checkout at `/root/citadel`, **no longer resolves** — do not spend a
session trying to reach it. The `LD_PRELOAD` trick for pointing the official
client at QuackCit is still recorded there and still works.

The box is a disposable test machine: restarting Citadel and creating test
rooms/users on it is fine and expected when exercising admin features.
