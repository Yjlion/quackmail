# Contributing

QuackCit is a Citadel server that runs inside DuckDB. Every protocol front-end
is a loadable DuckDB extension; the message store is a set of SQL tables. The
single rule that shapes everything else:

> **Extensions never share C++ state — they coordinate through DuckDB tables.**

If you need cross-session state (IMAP `IDLE`, presence, instant messages), add a
table, not a global. See [Architecture](architecture.md) for why.

`CLAUDE.md` at the repository root is the same guidance written for an agent,
and it is kept in step with this page.

## Layout

| Path | What |
|---|---|
| `core/` | shared plumbing, compiled **once** into a static library and linked into every extension |
| `citadel/` | the native Citadel protocol |
| `imap/ pop3/ smtp_in/ smtp_out/ managesieve/` | mail front-ends |
| `http/` | the web interface, CalDAV/CardDAV and JMAP |
| `nntp/ xmpp/ telnet/` | news, instant messaging, the BBS shell |
| `quackmail/` | umbrella extension: schema init, users, room and floor admin, MIME helpers |
| `spool/` | the timer-driven half: mailing-list distribution, remote pulls. No listener |
| `test/sql/` | sqllogictest (`make test`) |
| `test/integration/` | Python end-to-end tests, one per protocol |
| `test/parity/` | captured output from a real Citadel server, used as fixtures |
| `deploy/` | POSIX shell, no interpreter to install |
| `tools/` | code generators and `screenshots.py` |

## Building

Extensions are **Linux-only** (POSIX sockets, system OpenSSL).

```bash
GEN=ninja make release     # build/release/extension/<name>/<name>.duckdb_extension
make test                  # sqllogictest
python3 test/integration/test_<protocol>.py
```

RAM, not cores, is the constraint on the DuckDB build:

```bash
GEN=ninja CMAKE_BUILD_PARALLEL_LEVEL=2 make release
```

The first build takes 20–40 minutes. Incremental rebuilds are fast **except**
that anything under `core/` is compiled into every extension, so touching
`core/src/*.cpp` or a header they share rebuilds all of them. Batch core changes
and pay that cost once.

The integration tests import the `duckdb` Python module, and a few want extras.
Keep them in a virtualenv:

```bash
python3 -m venv ~/venv && ~/venv/bin/pip install 'duckdb==1.5.4' jmapc
```

## Adding a listener

1. Declare a global `ServerController`.
2. Write `void Handle(DatabaseInstance &db, net::ClientStream &stream)`. If the
   handler needs STARTTLS, give it a `ServerController &` parameter and add a
   thin wrapper (see `HandleImap` / `HandlePop3`).
3. `RegisterServerControls(loader, "<prefix>", <default_port>, g_ctrl, Handle)`
   registers `<prefix>_start`, `_stop` and `_status` with the `tls_cert`,
   `tls_key`, `implicit_tls` and `starttls` named parameters.

Implicit TLS is handled centrally in `ServerController::AcceptLoop`. STARTTLS is
done in the handler: `ctrl.StartTlsEnabled()` → `stream.StartTls(ctrl.TlsCtx(), err)`.
Two ports for one protocol means two controllers and two thin wrappers over one
implementation (`smtp_out`, `pop3`).

## Adding a module

Besides `<mod>/src/*` there are **four** files to touch — the last is easy to miss:

1. `<mod>/CMakeLists.txt` — copy `pop3/CMakeLists.txt`, change the name.
2. `extension_config.cmake` — add a `duckdb_extension_load(...)` entry.
3. `core/quackmail_core.cmake` — add any new `core/src/*.cpp` to the single
   compile list (core is compiled exactly once, to avoid duplicate symbols).
4. `.github/workflows/release.yml` — add the extension to the hardcoded
   `for ext in ...` packaging loop, or it will not ship in releases.

Then add it to `quackcit_services` in `deploy/quackcit_common.sh`,
`test/sql/quackmail.test`, and the table in [Protocol coverage](protocols.md).

## Adding a background worker

Not everything is a listener. For anything on a timer, declare a `PeriodicWorker`
(`core/include/quackmail/worker.hpp`), write a tick taking a `Connection &`, and
register `<prefix>_start/_stop/_status` — `spool/` has the registration helper.
Add a row to `quackcit_workers` in `deploy/quackcit_common.sh`, which is what
actually starts it on a real install.

**Always give a worker a one-shot `_run` table function beside its controls.**
That is what makes it testable: an integration test calls the run function and
asserts, instead of sleeping past a poll interval.

## Reuse before you write

`core/include/quackmail/` is deliberately large. Before adding anything, look
for it there: `citadel_store.hpp` (rooms, floors, messages, last-read, users,
and the RFC 4314 permission helpers — every front-end that accepts a message
must ask `CanPost`, never re-derive it), `citadel_msg.hpp`, `mime.hpp`,
`auth.hpp`, `sasl.hpp`, `delivery.hpp`, `mailpolicy.hpp`, `spf.hpp`, `dkim.hpp`,
`dmarc.hpp`, `rbl.hpp`, `sieve.hpp`, `quota.hpp`, `listserv.hpp`, `worker.hpp`,
`psl.hpp`, `tz.hpp`, `vcard.hpp`, `ical.hpp`, `vnote.hpp`, `html_sanitize.hpp`,
`http_client.hpp`, `mail_client.hpp`, `feed.hpp`, `net.hpp`.

`quota.hpp` is storage quota rather than send quota, which is why it is not in
`mailpolicy.hpp`: that file is scoped to SMTP site policy, whereas storage is
asked by IMAP, JMAP, DAV, the web and `InsertMessage` itself. Putting it there
would force `citadel_store.cpp` to include `mailpolicy.hpp` and invert the
dependency.

`net::Connect` is the shared dialer, with a connect timeout — never write
another `getaddrinfo`/`connect` pair.

## The parity oracle

A **real Citadel server** is the oracle. Source is the spec; live probes are the
acceptance test.

Get a server with docker:

```bash
docker run -d --name citadel -p 10504:504 -p 10025:25 -p 10110:110 \
  -p 10143:143 -p 10119:119 -p 10522:5222 -p 10080:80 \
  -v citadel-data:/citadel-data citadeldotorg/citadel
docker exec citadel /usr/local/citadel/sendcommand -h/citadel-data 'AGUP admin'
```

Real port plus 10000, so nothing collides with QuackCit's dev ports or with the
integration tests. `admin`/`citadel` exists already, and `sendcommand` needs
`-h/citadel-data` or it looks in the wrong place.

The container ships no source tree. **Read the source from the project's own
cgit**, which serves the whole tree, history and patches:

```bash
curl -sS -b 'cgit_access=verified' \
  https://code.citadel.org/citadel.git/plain/libcitadel/lib/libcitadel.h
```

The `cgit_access=verified` cookie is not optional — without it every path
returns a JavaScript challenge page rather than the file, which looks exactly
like a successful fetch of the wrong content. History matters as much as the
current tree: constants have been *removed* from Citadel over the years, and one
that exists only in an old commit is one you must not put on the wire.

## Tests

## Tests

```bash
make test                                   # sqllogictest: load/start/stop/status, admin fns, MIME,
                                            #   site policy (test/sql/mailpolicy.test) and the
                                            #   Sieve parser (test/sql/sieve.test)
pip install duckdb==1.5.4
python3 test/integration/test_citadel.py     # native protocol: NEWU->GOTO->ENT0->MSGS->MSG0
python3 test/integration/test_smtp_in.py     # MX: recipient validation, domains, aliases, ACLs -> POP3
python3 test/integration/test_smtp_policy.py # outbound DKIM signing + per-user rate limiting
python3 test/integration/test_lmtp.py        # LMTP per-recipient replies, no spam checks
python3 test/integration/test_managesieve.py # ManageSieve round trip, then the filter routes delivery
python3 test/integration/test_sieve_extensions.py # flags reach IMAP, vacation answers a sender once
python3 test/integration/test_itip.py        # scheduling: PUT mails an invitation, a reply updates the event

pip install jmapc                            # optional; the test below skips itself without it
python3 test/integration/test_jmap_client.py # JMAP driven by a third-party client library
```

Most integration tests assert the wire format this server produces, which
cannot catch a format that is self-consistently wrong. `test_caldav.py` passed
for months while real clients could not sync, because resource names were
required to equal the object UID and vdirsyncer names them itself.
`test_jmap_client.py` is the answer to that class of bug for JMAP: every
request goes through `jmapc`'s own models, so a response shape somebody else's
deserializer rejects fails without anyone having thought to assert it. It found
that all three Session URLs were relative paths, which made `/jmap/` unusable
to any real client.

The DKIM tests run entirely offline: `dkim::Verify` takes an injectable key
lookup, and `policy::DkimKeyLookup` resolves locally stored keys before falling
back to DNS, so a sign→verify round trip needs no resolver.
