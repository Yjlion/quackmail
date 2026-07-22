# QuackCit

A **[Citadel](https://www.citadel.org/) groupware/BBS server that runs *inside*
DuckDB**. Each protocol front-end is a separate DuckDB extension you load into a
running DuckDB; the whole message store — users, floors, rooms, and messages —
is a set of SQL tables.

Like real Citadel, QuackCit speaks its own native client/server protocol
(`protocol.txt`, TCP 504) *and* the standard mail protocols (SMTP, IMAP, POP3)
over the same store. A message that arrives by SMTP shows up in the recipient's
Mail **room**, readable both from a Citadel client and over POP3/IMAP.

```sql
LOAD quackmail;            -- umbrella: schema, users, admin, MIME helpers
LOAD quackmail_citadel;    -- the native Citadel protocol
CALL qm_user_add('alice', 'secret');
CALL cit_start('0.0.0.0', 5040);        -- background Citadel listener
-- ... clients connect, post to rooms ...
SELECT display_name, highest_msg FROM citadel_rooms;
SELECT author, subject FROM citadel_messages;
CALL cit_stop();
```

## Design

The server *is* DuckDB with extensions loaded — not a separate daemon that
happens to use DuckDB for storage. When you call a `*_start` function, the
extension spawns a background listener thread in the DuckDB process; each
connection runs a protocol session against an internal DuckDB connection.

Each protocol is its own loadable `.duckdb_extension` (its own shared object).
They do **not** share C++ state at runtime — they coordinate through the
**shared DuckDB database**: the tables are the bus. Common C++ plumbing (schema,
socket/TLS server scaffolding, the Citadel room/message store, MIME parsing,
SASL auth, a Sieve evaluator) lives in `core/` and is linked into every
extension.

| Extension | Functions | Role |
|---|---|---|
| `quackmail` (umbrella) | `qm_version`, `qm_status`, `qm_user_add/remove`, `cit_room_add`, `cit_floor_add`, `qm_mime_*`, `qm_parse_date` | schema init, users, room/floor admin, MIME helpers |
| `quackmail_citadel` | `cit_start/_stop/_status` | ✅ native Citadel protocol (TCP 504; dev default 5040) |
| `quackmail_smtp_in` | `qm_smtp_in_start/_stop/_status` | ✅ inbound SMTP gateway → delivers into Mail rooms (STARTTLS/implicit TLS + SASL AUTH + Sieve) |
| `quackmail_pop3` | `qm_pop3_start/_stop/_status` | ✅ POP3 gateway → serves each user's Mail room |
| `quackmail_imap` | `qm_imap_start/_stop/_status` | ✅ minimal IMAP4rev1 gateway → mailboxes = rooms |
| `quackmail_managesieve` | `qm_managesieve_start/_stop/_status` | 🚧 stub |
| `quackmail_smtp_out` | `qm_smtp_out_start/_stop/_status` | 🚧 stub (outbound queue drainer) |

`*_start(host, port)` also accepts named params: `tls_cert`, `tls_key`,
`implicit_tls`, `starttls`. With `starttls => true` and no cert paths, a
self-signed certificate is generated in memory (for development). All control
functions return a status row: `(action, running, host, port, connections, note)`.

> **Port 504** is the IANA-assigned Citadel port, but it is privileged on Linux,
> so `cit_start` defaults to **5040** for development (just as `qm_smtp_in`
> defaults to 2525 for 25). Pass the port explicitly to use 504.

## Data model

The store is Citadel-shaped: **users → floors → rooms → messages**, with
per-user last-read pointers and a reference-count message model (one message can
be pointed into several rooms). Created idempotently on load by
`EnsureSchema` / `EnsureCitadelSchema` (`core/src/citadel_store.cpp`).

| Table | Purpose |
|---|---|
| `citadel_users` | Citadel user metadata: `usernum`, `axlevel`, post/call counters (credentials live in `quackmail_users`). |
| `citadel_floors` | Floors — named containers grouping rooms. Seeded with floor 0, "Main Floor". |
| `citadel_rooms` | Rooms: `room_num`, `name`/`display_name`, `floor_num`, `qr_flags` (public/private/passworded/mailbox), `mailbox_owner`, `highest_msg`. Seeded with Lobby (0) and Aide (1); user rooms start at 100; personal mailbox rooms are `<usernum>.<name>`. |
| `citadel_messages` | Messages: `msgnum`, `author`, `recipient`, `msgtime`, `subject`, `format_type` (0 = Citadel, 4 = RFC822/MIME), `raw` canonical bytes. |
| `citadel_room_msgs` | `(room_num, msgnum)` message pointers — the ref-count model. |
| `citadel_room_state` | Per-user `last_read` pointer + room flags. |
| `citadel_msg_flags` | IMAP flags (`\Seen`, `\Deleted`, …) per `(msgnum, username)`. |
| `citadel_config` | `INFO`/config key-value (nodename, fqdn, humannode, …). |

Also present: `quackmail_users` (credentials), `quackmail_sieve_scripts`,
`quackmail_outbound` (relay queue).

## Native Citadel protocol

`quackmail_citadel` implements a useful subset of the Citadel client/server
protocol (stateful, 3-digit result codes, pipe-delimited params, `000`-terminated
listings):

- **Session**: greeting, `NOOP`, `ECHO`, `IDEN`, `QUIT`, `LOUT`, `INFO`.
- **Auth**: `USER`/`PASS`, `NEWU` (create + log in), `SETP` (set password).
- **Floors**: `LFLR` (list), `CFLR` (create, aide).
- **Rooms**: `LKRA`/`LKRN`/`LKRO` (list all/new/old), `GOTO`, `CRE8`, `KILL`,
  `GETR`/`SETR`, `RINF`, `SLRP` (set last-read).
- **Messages**: `MSGS` (`all`/`new`/`old`/`last`/`first`/`gt`/`lt`), `MSG0`
  (field listing), `MSG2` (raw), `ENT0` (post).

Config-heavy admin verbs (`CONF`, `DOWN`, `SCDN`, `TERM`, `EXPI`), the Citadel
network mesh, and instant messaging are deferred (see Roadmap).

## Mail gateways

The standard-protocol extensions are front-ends onto the same room store:

- **`quackmail_smtp_in`** parses each inbound message, runs the recipient's Sieve
  script, and delivers into their Mail room (or a `fileinto` room) as a
  `format_type = 4` message. AUTH/relay still require STARTTLS + SASL.
- **`quackmail_pop3`** authenticates a user and serves their Mail room over
  `USER/PASS/STAT/LIST/UIDL/RETR/DELE/RSET/QUIT`.
- **`quackmail_imap`** (minimal) maps mailboxes to rooms (`INBOX` → the user's
  Mail room) and supports `LOGIN/LIST/SELECT/FETCH/STORE/EXPUNGE`, reusing the
  `core/` MIME parser for `BODY[...]`/`ENVELOPE`.

### MIME & message parsing (RFC 2045–2049, 822/2822/5322)

The `core/` MIME plumbing (`quackmail::mime`) is exposed as SQL table functions
on the umbrella extension, so you can parse the `raw` of any stored message:

```sql
SELECT section, content_type, filename, size_bytes
FROM   qm_mime_parts((SELECT raw::VARCHAR FROM citadel_messages WHERE msgnum = 1));

SELECT * FROM qm_mime_headers(msg);                       -- (name, value)
SELECT decoded FROM qm_mime_decode_header('=?UTF-8?B?SGVsbG8=?=');   -- -> "Hello"
SELECT name, address FROM qm_mime_addresses('Jane <jane@x.com>, Bob <bob@y.com>');
SELECT epoch, iso FROM qm_parse_date('Mon, 02 Jan 2006 15:04:05 -0700');
```

## TLS & AUTH

TLS uses the **system OpenSSL** (`libssl-dev`); DuckDB's bundled mbedTLS is
crypto-only. SASL `PLAIN`/`LOGIN` (SMTP) is offered only after TLS; credentials
are verified against `quackmail_users` (salted SHA-256, constant-time compare).
The native Citadel `USER`/`PASS` verify against the same table.

## Build

Requires a C++17 compiler, CMake, Ninja/Make, and `libssl-dev`. The socket layer
is POSIX; the extensions build and run on Linux (CI targets `linux_amd64`).

```bash
git clone --recurse-submodules <repo>       # duckdb + extension-ci-tools submodules (pinned v1.5.4)
cd quackmail
GEN=ninja make                              # builds DuckDB + all extensions
```

Artifacts land in `build/release/extension/<name>/<name>.duckdb_extension`, and a
DuckDB CLI with every extension statically linked is at `build/release/duckdb`.

### Install from a release

Pushing a `v*` tag runs `.github/workflows/release.yml`, which builds the
extensions and attaches a `quackmail-<tag>-linux_amd64.tar.gz` bundle (the
`.duckdb_extension` files plus a statically linked `duckdb` CLI) to a GitHub
Release. To install:

```bash
tar -xzf quackmail-<tag>-linux_amd64.tar.gz
cd quackmail-<tag>-linux_amd64
./duckdb -unsigned          # unsigned extensions require -unsigned
```
```sql
LOAD './quackmail.duckdb_extension';
LOAD './quackmail_citadel.duckdb_extension';
```

## Try it

```bash
./build/release/duckdb
```
```sql
LOAD quackmail;
LOAD quackmail_citadel;
CALL qm_user_add('alice', 'secret');
CALL cit_start('127.0.0.1', 5040);
```
From another shell, drive the native protocol by hand:
```bash
nc 127.0.0.1 5040
200 quackcit|QuackCit BBS|quackmail.test|QuackCit 0.1.0
USER alice
300 Password required for alice.
PASS secret
200 alice|4|0|0|0|1|0
GOTO Lobby
200 Lobby|0|0|1|1|0|0|0|0||0|0|0|0
ENT0 1||0|0|Hello
400 Enter message; terminate with '000' on a line by itself.
Hi from the Lobby.
000
200 1
MSGS all
100 listing follows
1
000
QUIT
```
Deliver mail and read it back over POP3 (unified store):
```sql
LOAD quackmail_smtp_in; LOAD quackmail_pop3;
CALL qm_smtp_in_start('127.0.0.1', 2525, starttls => true);
CALL qm_pop3_start('127.0.0.1', 1110);
```

## Tests

```bash
make test                                   # sqllogictest: load/start/stop/status, admin fns, MIME
pip install duckdb==1.5.4
python3 test/integration/test_citadel.py    # native protocol: NEWU->GOTO->ENT0->MSGS->MSG0
python3 test/integration/test_smtp_in.py    # SMTP STARTTLS+AUTH delivery -> Mail room -> POP3 RETR
```

## Roadmap

- **IMAP depth**: `BODYSTRUCTURE`, `IDLE`, `UIDPLUS`, `CONDSTORE`/`QRESYNC`,
  `NAMESPACE`, and the extension batch (RFC 3501+).
- **Citadel breadth**: `CONF`/config verbs, message expiry/purge (`EXPI`),
  instant messaging (`SEXP`/`GEXP`), address books / vCard rooms, and the
  Citadel network mesh (inter-node message replication).
- **SMTP/LMTP**: LMTP (2033), submission (6409), enhanced codes (2034),
  pipelining (2920), CHUNKING/BDAT (3030), 8BITMIME (6152), and the `smtp_out`
  relay/queue drainer.
- **Hardening**: SCRAM-SHA-256 SASL (5802/7677), bcrypt/argon2 hashing, the full
  Sieve feature set, charset transcoding beyond UTF-8/Latin-1, and DKIM/SPF/DMARC.
