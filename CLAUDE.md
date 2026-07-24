# CLAUDE.md — working in this repo

QuackCit is a **Citadel BBS/groupware server that runs inside DuckDB**. Every
protocol front-end is a loadable DuckDB extension; the message store is a set of
SQL tables (users → floors → rooms → messages). Mail protocols (SMTP/IMAP/POP3)
are gateways over the same Citadel room store.

**Extensions never share C++ state — they coordinate through DuckDB tables.**
"The tables are the bus." If you need cross-session state (IDLE, presence,
instant messages), add a table, not a global.

See [MEMORY.md](MEMORY.md) for project history and decisions, [TODO.md](TODO.md)
for what's next.

## Layout

| Path | What |
|---|---|
| `core/` | shared plumbing compiled **once** into a static lib and linked into every extension |
| `citadel/` | native Citadel protocol (the centerpiece) |
| `imap/ pop3/ smtp_in/ smtp_out/ managesieve/` | mail front-ends |
| `quackmail/` | umbrella extension: schema init, users, room/floor admin, MIME helpers |
| `test/sql/` | sqllogictest (`make test`) |
| `test/integration/` | python end-to-end tests, one per protocol |
| `test/parity/` | captured output from the real Citadel server used as fixtures |
| `deploy/` | `run_quackcit.py` (env-driven launcher), `quackcit.sh` + `quackcit.conf` (run it), `quackcitadm.sh` + `quackcit_admin.py` (administer it) |

## Build and test

Extensions are **Linux-only** (POSIX sockets, system OpenSSL). Build on
`debian.lan`, not on the Windows checkout.

```bash
GEN=ninja make release     # artifacts: build/release/extension/<name>/<name>.duckdb_extension
make test                  # sqllogictest
python3 test/integration/test_<protocol>.py
```

On `debian.lan` the toolchain is user-local in `~/venv` and RAM is tight:

```bash
cd ~/quackmail && git fetch origin && git reset --hard origin/<branch>
PATH=~/venv/bin:$PATH GEN=ninja CMAKE_BUILD_PARALLEL_LEVEL=2 make release
```

The first DuckDB build takes ~20 minutes; incremental rebuilds are fast.

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

Then add it to `deploy/run_quackcit.py`, `test/sql/quackmail.test`, and the
README table.

## Reuse before you write

- `core/include/quackmail/citadel_store.hpp` — rooms, floors, messages,
  last-read, users: `ListRooms`, `ResolveRoom`, `GetRoomStats`, `RoomMessages`,
  `LoadMessage`, `InsertMessage`, `SetLastRead`, `GetOrCreateMailRoom`,
  `EnsureUserRooms`, `IsLocalUser`.
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
  keep) and `Check` for ManageSieve validation.
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
  `qm_dkim_verify`) has to be a scalar function.
- The admin CLI cannot open the database while the server is running — DuckDB
  permits one read-write process per file. `deploy/quackcitadm.sh` goes through
  the launcher's Unix socket instead; see `deploy/quackcit_admin.py`.
- `pkill -f run_quackcit` over SSH kills your own shell (the pattern matches the
  command line). Kill by PID.

## The parity oracle

`debian.lan` runs a **real Citadel Groupware server** used as the parity oracle,
and carries the **full Citadel source at `/root/citadel`** (read it with sudo —
`citadel/server/modules/<proto>/` for protocol servers, `textclient/` for the
BBS client). Source is the spec; live probes are the acceptance test.

Ports: oracle owns 504, 25/465/587, 110/995, 143/993, 119/563, 5222, 80/443.
QuackCit runs beside it on dev ports (citadel 5040, smtp-in 2525, submission
2587, smtps 2465, pop3 1110, pop3s 1995, imap 1143). Details, including SSH
access and the `LD_PRELOAD` trick for the official client, are in
[MEMORY.md](MEMORY.md).

The box is a disposable test machine: restarting Citadel and creating test
rooms/users on it is fine and expected when exercising admin features.
