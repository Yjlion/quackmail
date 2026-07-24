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
| `quackmail` (umbrella) | `qm_version`, `qm_status`, `qm_user_add/remove`, `cit_room_add`, `cit_floor_add`, `qm_mime_*`, `qm_parse_date`, and the site-policy admin functions (`qm_domain_*`, `qm_alias_*`, `qm_acl_*`, `qm_rbl_*`, `qm_dkim_*`, `qm_ratelimit_*`, `qm_config_*`) | schema init, users, room/floor admin, MIME helpers, policy administration |
| `quackmail_citadel` | `cit_start/_stop/_status` | ✅ native Citadel protocol (TCP 504; dev default 5040) |
| `quackmail_smtp_in` | `qm_smtp_in_start/_stop/_status`, `qm_lmtp_*` | ✅ inbound MX with SPF/DKIM/DMARC/DNSBL, hosted domains, aliases and allow/block rules — plus an LMTP local-injection listener (24; dev 2033) |
| `quackmail_smtp_out` | `qm_smtp_submission_start/_stop/_status`, `qm_smtp_smtps_*`, `qm_smtp_relay_*` | ✅ authenticated submission (587/465; dev 2587/2465) with DKIM signing and per-user rate limiting, plus the outbound queue drainer |
| `quackmail_pop3` | `qm_pop3_start/_stop/_status`, `qm_pop3s_*` | ✅ POP3 gateway (STLS + implicit TLS) → serves each user's Mail room |
| `quackmail_imap` | `qm_imap_start/_stop/_status` | ✅ minimal IMAP4rev1 gateway → mailboxes = rooms |
| `quackmail_managesieve` | `qm_managesieve_start/_stop/_status` | ✅ ManageSieve (RFC 5804, port 4190) — install the Sieve filters the delivery path applies |
| `quackmail_nntp` | `qm_nntp_start/_stop/_status`, `qm_nntps_*` | ✅ NNTP reader **and poster** (119/563; dev 1119/1563) — rooms are newsgroups |
| `quackmail_xmpp` | `qm_xmpp_start/_stop/_status`, `qm_xmpps_*` | ✅ XMPP c2s (5222/5223; dev 15222/15223) — instant messages bridged to Citadel's |
| `quackmail_telnet` | `qm_telnet_start/_stop/_status`, `qm_telnets_*` | ✅ BBS shell over telnet (23; dev 2300) and telnets (992; dev 2992) — the Citadel text-client experience, server-side |

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

### Site policy

Mail policy is a second set of tables, created alongside the rest and read by
both SMTP front-ends. All of it is empty by default: a fresh install accepts
mail only for `c_fqdn`, queries no blocklist, and signs nothing.

| Table | Purpose |
|---|---|
| `quackmail_domains` | Domains we accept mail for beyond `c_fqdn`: `kind` is `local` (deliver here) or `relay`, plus the default `dkim_selector`. |
| `quackmail_aliases` | `alias` → `destination`. Several rows for one alias fan out to several users; `@example.com` is that domain's catch-all. |
| `quackmail_acl` | Allow/block rules over `scope` ∈ {`ip`, `sender`, `domain`, `rcpt`, `helo`}. Patterns are globs; `ip` also takes CIDR. **Allow always beats block**, so a narrow allow carves an exception out of a broad block. |
| `quackmail_rbl_zones` | DNSBL zones to query, in order. Empty by default — blocklist checking is opt-in. |
| `quackmail_dkim_keys` | Outbound signing keys. The private half is stored here, so the database file's permissions are the security boundary for it. |
| `quackmail_rate_limits` | Per-user send quotas; the row with an empty username is the default (100 per 300 s, 500 per 24 h). |
| `quackmail_send_log` | The sliding window the limiter counts over. |
| `quackmail_inbound_log` | What the inbound checks decided, per recipient: SPF/DKIM/DMARC/DNSBL verdicts and the disposition. |

## Mail authentication

### Inbound

`quackmail_smtp_in` runs each check at the protocol stage where it belongs: the
IP rules at connect, the HELO rule at EHLO, SPF at `MAIL FROM`, the recipient
rules and the DNSBL at `RCPT TO` (deferred so an allow-listed recipient such as
postmaster stays reachable from a listed address), and DKIM + DMARC over the
message bytes exactly as received. Every accepted message is then stamped with
`Received:`, `Authentication-Results:` and `Received-SPF:` — prepended *after*
verification, so they cannot disturb a signature.

The shipped posture is to report everything and reject little:

| Config key | Default | Effect |
|---|---|---|
| `qm_dmarc_enforce` | `1` | honour the sender's own published policy: `p=reject` → 550, `p=quarantine` → filed into `qm_quarantine_room` |
| `qm_rbl_reject` | `1` | refuse a client listed by a configured DNSBL (none are configured by default) |
| `qm_spf_reject` | `0` | SPF hard-fail alone does not reject — forwarded mail fails SPF routinely |
| `qm_dkim_reject` | `0` | a broken signature alone does not reject; DMARC decides what it means |
| `qm_quarantine_room` | `Junk` | where quarantined mail is filed |

Change any of them with `quackcitadm.sh config set qm_spf_reject 1`.

*Known limitation:* no Public Suffix List is bundled, so DMARC's organizational
domain is approximated as the last two labels plus a table of common
multi-label suffixes (`co.uk`, `com.au`, …). Under an unlisted multi-label
suffix, relaxed alignment comes out stricter than a PSL-backed implementation —
never looser, so it cannot turn a Fail into a Pass.

### Outbound

Submitted mail is signed with the key for its `From:` header domain (falling
back to the envelope sender's domain, then to the organizational domain, so one
key covers subdomains). Signing happens once at submission, so the locally
delivered copy and every queued copy carry the same signature.

```bash
deploy/quackcitadm.sh dkim keygen example.com mail
```

That generates a 2048-bit key, stores it, and prints the TXT record to publish
at `mail._domainkey.example.com`. Confirm it with
`dig +short TXT mail._domainkey.example.com`, then check a signed message with
`deploy/quackcitadm.sh dkim verify /path/to/message.eml`.

Rate limiting charges **one unit per envelope recipient**, so a message to 50
addresses spends 50. Over-quota submissions get `451` — transient, so a
well-behaved client retries rather than bouncing the mail.

```bash
deploy/quackcitadm.sh ratelimit set '' 100 300 500   # the default policy
deploy/quackcitadm.sh ratelimit set bulkmailer 500 300 5000
deploy/quackcitadm.sh ratelimit status alice
```

### LMTP

`qm_lmtp_start` opens an RFC 2033 listener for **trusted local injection**. It
greets with `LHLO`, emits one reply per recipient after `DATA`, and **performs
no sender authentication and no spam filtering at all** — no SPF, DKIM, DMARC,
DNSBL or access rules. Domain routing, alias expansion and the recipient's
Sieve filter still apply, because those are addressing rather than filtering.

Anything that can reach this socket can inject mail as anyone. It binds to
loopback regardless of `QUACKCIT_HOST` unless `QUACKCIT_HOST_LMTP` overrides it.
Do not expose it.

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

## The BBS shell (telnet)

A real Citadel install has no telnet listener: the BBS experience comes from the
`citadel` text client speaking the native protocol. `quackmail_telnet` *is* that
client, running server-side, so a plain `telnet` gets the BBS — registration and
login, the `<Room>>` prompt, and the menu from `citadel.rc`:

```
telnet localhost 2300
```
```
QuackCit BBS - The Cloud

Enter your name (or 'new' to register): alice
Password:

Lobby>  1 new of 1 messages
Room cmds:    <K>nown rooms, <G>oto next room, <.G>oto a specific room, ...
```

Commands: `<G>`oto, `<K>`nown rooms, `<U>`ngoto, `<M>`ail, read `<N>`ew/`<O>`ld/
`<F>`orward/`<R>`everse/`<L>`ast five, `<E>`nter a message, `<W>`ho is online,
`<P>`age a user, `<X>` expert mode, `<?>` help, `<T>`erminate, and `.` commands
(`.Goto <room>`). Sessions register in `citadel_sessions`, so telnet users and
native Citadel clients see each other in the who-list and can page one another.

## News (NNTP)

Every room a user can see is a newsgroup, using Citadel's own name mapping
(`Lobby` → `ctdl.lobby`, `Global Address Book` → `ctdl.global+20address+20book`,
`0000000002.Mail` unchanged), and the room's message pointers are the article
numbers. `LIST ACTIVE/NEWSGROUPS/OVERVIEW.FMT` (with wildmat patterns), `GROUP`,
`LISTGROUP`, `ARTICLE`/`HEAD`/`BODY`/`STAT`, `NEXT`/`LAST`, `OVER`/`XOVER`,
`NEWGROUPS`, `DATE`, `AUTHINFO`, and `STARTTLS` are implemented.

Unlike a stock Citadel server, which answers `POST` with
`500 I'm afraid I can't do that.`, QuackCit **accepts posted articles**: they are
stored as ordinary Citadel messages, so an article posted over NNTP is readable
from a Citadel client, the BBS shell, IMAP and POP3. (It also resolves
`<message-id>` fetches and reports real `:bytes`/`:lines` in `OVER`, both of
which Citadel punts on.)

## Instant messaging (XMPP)

`quackmail_xmpp` speaks client-to-server XMPP: STARTTLS, SASL `PLAIN` (and the
legacy `jabber:iq:auth`), resource binding, sessions, `jabber:iq:roster`,
presence, `vcard-temp`, `urn:xmpp:ping` and service discovery.

Like Citadel, there is no stored roster: **the roster and presence list are the
people currently logged in** — read from `citadel_sessions`, so XMPP clients,
telnet users and native Citadel clients all see one another. A `<message>` is
written to `citadel_express`, the same queue the native protocol's `SEXP`/`GEXP`
uses, and queued messages are pushed to connected XMPP clients as `<message>`
stanzas. Sending from XMPP and reading with `GEXP` from a Citadel client (or the
BBS shell) works in both directions.

## Mail gateways

The standard-protocol extensions are front-ends onto the same room store:

- **`quackmail_smtp_in`** authenticates the sender (SPF/DKIM/DMARC/DNSBL), routes
  the recipient through the hosted-domain and alias tables, runs their Sieve
  script, and delivers into their Mail room (or a `fileinto` room) as a
  `format_type = 4` message. See [Mail authentication](#mail-authentication).
  It offers no AUTH and never relays — submission lives in `quackmail_smtp_out`.
- **`quackmail_managesieve`** lets users install those Sieve scripts over RFC
  5804 (`PUTSCRIPT`/`SETACTIVE`/`GETSCRIPT`/…), validating each one against the
  same parser the delivery path uses, so a script that installs is a script
  that runs.
- **`quackmail_pop3`** authenticates a user and serves their Mail room over
  `USER/PASS/STLS/CAPA/STAT/LIST/UIDL/RETR/TOP/DELE/RSET/NOOP/LAST/QUIT` —
  the same command set, response wording and UPDATE-state semantics as a stock
  Citadel POP3 server. Two listeners like Citadel's: `qm_pop3` (110; dev 1110,
  STLS) and `qm_pop3s` (995; dev 1995, implicit TLS).
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

## Running and administering a server

`deploy/` has two scripts. Both read `deploy/quackcit.conf` (override the path
with `QUACKCIT_CONF`), where the database location, bind address, TLS material
and every port live. Values already in the environment win, so one-off
overrides work.

```bash
deploy/quackcit.sh start          # background, PID file, log file
deploy/quackcit.sh status
deploy/quackcit.sh logs 100
deploy/quackcit.sh stop

QUACKCIT_PORT_SMTP_IN=25 deploy/quackcit.sh foreground   # run in this terminal
```

`quackcitadm.sh` administers the server — users, domains, aliases, access
rules, DKIM keys, quotas, Sieve scripts, the outbound queue and server config:

```bash
deploy/quackcitadm.sh user add alice s3cret
deploy/quackcitadm.sh domain add example.com
deploy/quackcitadm.sh alias add sales@example.com alice
deploy/quackcitadm.sh alias add @example.com catchall     # domain catch-all
deploy/quackcitadm.sh acl block ip 192.0.2.0/24 'noisy range'
deploy/quackcitadm.sh acl allow ip 192.0.2.7 'except this host'
deploy/quackcitadm.sh rbl add zen.spamhaus.org
deploy/quackcitadm.sh dkim keygen example.com mail
deploy/quackcitadm.sh ratelimit status alice
deploy/quackcitadm.sh queue list
deploy/quackcitadm.sh help
```

It works whether or not the server is running. **DuckDB allows a single
read-write process per database file**, so while the server holds it the script
cannot simply open the file; it sends the statement over the launcher's admin
socket instead, and falls back to opening the database directly when the server
is stopped. That socket accepts arbitrary SQL, so it is created mode 0600 and
should not live on a shared filesystem.

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
```

The DKIM tests run entirely offline: `dkim::Verify` takes an injectable key
lookup, and `policy::DkimKeyLookup` resolves locally stored keys before falling
back to DNS, so a sign→verify round trip needs no resolver.

## Roadmap

- **IMAP depth**: `BODYSTRUCTURE`, `IDLE`, `UIDPLUS`, `CONDSTORE`/`QRESYNC`,
  `NAMESPACE`, and the extension batch (RFC 3501+).
- **Citadel breadth**: `CONF`/config verbs, message expiry/purge (`EXPI`),
  instant messaging (`SEXP`/`GEXP`), address books / vCard rooms, and the
  Citadel network mesh (inter-node message replication).
- **SMTP**: PIPELINING (2920), CHUNKING/BDAT (3030), and DSN (3461).
- **Mail authentication depth**: a bundled Public Suffix List for exact DMARC
  organizational-domain resolution, DMARC aggregate (`rua`) reporting, ARC
  (8617) so forwarded mail survives, and MTA-STS / DANE for outbound transport.
- **Hardening**: SCRAM-SHA-256 SASL (5802/7677), bcrypt/argon2 hashing, and
  charset transcoding beyond UTF-8/Latin-1.
- **Sieve**: the variables (5229), regex, vacation (5230) and imap4flags (5232)
  extensions; the parser covers the RFC 5228 core plus `reject`, `envelope`,
  `body` and `copy` today.
