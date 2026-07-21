# QuackMail

An email server that runs **inside DuckDB**. Each component of the mail server is
a separate DuckDB extension you load into a running DuckDB; your mailbox is a SQL
table.

```sql
LOAD quackmail_smtp_in;
CALL qm_smtp_in_start('0.0.0.0', 2525, starttls => true);   -- background SMTP listener
-- ... mail arrives ...
SELECT from_addr, subject, received_at FROM quackmail_messages;
CALL qm_smtp_in_stop();
```

## Design

The server *is* DuckDB with extensions loaded — not a separate daemon that happens
to use DuckDB for storage. When you call a `*_start` function, the extension spawns
a background listener thread in the DuckDB process; incoming mail is parsed and
inserted into shared DuckDB tables through an internal connection.

Each protocol is its own loadable `.duckdb_extension` (its own shared object). They
do **not** share C++ state at runtime — they coordinate through the **shared DuckDB
database**: the tables are the bus. Common C++ plumbing (schema, socket/TLS server
scaffolding, MIME parsing, SASL auth, a Sieve evaluator) lives in `core/` and is
linked into every extension.

| Extension | Functions | Status |
|---|---|---|
| `quackmail` (umbrella) | `qm_version`, `qm_status`, `qm_user_add`, `qm_user_remove`, `qm_mime_*`, `qm_parse_date` | ✅ |
| `quackmail_smtp_in` | `qm_smtp_in_start/_stop/_status` | ✅ inbound SMTP + STARTTLS/implicit TLS + SASL AUTH |
| `quackmail_smtp_out` | `qm_smtp_out_start/_stop/_status` | 🚧 stub (queue drainer) |
| `quackmail_imap` | `qm_imap_start/_stop/_status` | 🚧 stub |
| `quackmail_managesieve` | `qm_managesieve_start/_stop/_status` | 🚧 stub |
| `quackmail_pop3` | `qm_pop3_start/_stop/_status` | 🚧 stub |

`*_start(host, port)` also accepts named params: `tls_cert`, `tls_key`,
`implicit_tls`, `starttls`. With `starttls => true` and no cert paths, a self-signed
certificate is generated in memory (for development). All control functions return a
status row: `(action, running, host, port, connections, note)`.

### Schema (created idempotently on load/start)

`quackmail_messages`, `quackmail_recipients`, `quackmail_headers`,
`quackmail_mailboxes`, `quackmail_message_flags`, `quackmail_outbound`,
`quackmail_sieve_scripts`, `quackmail_users`.

### TLS & AUTH

TLS uses the **system OpenSSL** (`libssl-dev`). DuckDB's bundled mbedTLS is
crypto-only (no TLS handshake), so it can't serve here. SASL `PLAIN`/`LOGIN` is only
offered after TLS; credentials are verified against `quackmail_users` (salted
SHA-256, constant-time compare). smtp_in requires AUTH for relay/submission but
still accepts unauthenticated inbound mail for local recipients.

### MIME & message parsing (RFC 2045–2049, 822/2822/5322)

Common message-format plumbing lives in `core/` (`quackmail::mime`) and is exposed as SQL table
functions on the umbrella extension, so you can parse the `raw` BLOB of any stored message directly:

```sql
LOAD quackmail;
-- Decompose a message into its MIME parts (IMAP-style section numbers).
SELECT section, content_type, filename, size_bytes
FROM   qm_mime_parts((SELECT raw::VARCHAR FROM quackmail_messages WHERE id = 1));

SELECT * FROM qm_mime_headers(msg);                       -- (name, value)
SELECT decoded FROM qm_mime_decode_header('=?UTF-8?B?SGVsbG8=?=');   -- RFC 2047 -> "Hello"
SELECT decoded FROM qm_mime_decode('base64', 'SGk=');     -- transfer decode -> "Hi"
SELECT name, address FROM qm_mime_addresses('Jane <jane@x.com>, Bob <bob@y.com>');
SELECT epoch, iso FROM qm_parse_date('Mon, 02 Jan 2006 15:04:05 -0700');
```

Implemented: `Content-Type`/`Content-Disposition` parsing, `multipart/*` decomposition and
`message/rfc822` nesting (RFC 2046), base64 + quoted-printable transfer decoding (RFC 2045), RFC 2047
encoded-word decoding, RFC 5322 address-list and date parsing. Charsets: UTF-8 and US-ASCII pass
through, ISO-8859-1 is transcoded to UTF-8; other charsets return the transfer-decoded bytes as-is
(full transcoding is a later item). RFC 2048 is procedural (IANA registration); RFC 2049 conformance
defaults are honored. Inbound delivery (`smtp_in`) now decodes RFC 2047 subjects on store.

## Build

Requires a C++17 compiler, CMake, Ninja/Make, and `libssl-dev`.

```bash
git clone --recurse-submodules <repo>       # duckdb + extension-ci-tools are submodules (pinned v1.5.4)
cd quackmail
GEN=ninja make                              # builds DuckDB + all extensions
```

Artifacts land in `build/release/extension/<name>/<name>.duckdb_extension`, and a
DuckDB CLI with every extension statically linked is at `build/release/duckdb`.

### Install from a release

Pushing a `v*` tag runs `.github/workflows/release.yml`, which builds the extensions and attaches a
`quackmail-<tag>-linux_amd64.tar.gz` bundle (the six `.duckdb_extension` files plus a statically
linked `duckdb` CLI) to a GitHub Release. To install:

```bash
tar -xzf quackmail-<tag>-linux_amd64.tar.gz
cd quackmail-<tag>-linux_amd64
./duckdb -unsigned          # unsigned extensions require -unsigned (or SET allow_unsigned_extensions=true)
```
```sql
LOAD './quackmail.duckdb_extension';
LOAD './quackmail_smtp_in.duckdb_extension';
```

## Try it

```bash
./build/release/duckdb
```
```sql
CALL qm_user_add('alice', 'secret');
CALL qm_smtp_in_start('127.0.0.1', 2525, starttls => true);
```
From another shell:
```bash
swaks --to alice@quackmail.test --server 127.0.0.1:2525 --tls --auth-user alice --auth-password secret
```
Back in DuckDB:
```sql
SELECT from_addr, subject FROM quackmail_messages;
```

## Tests

```bash
make test                                   # sqllogictest: load/start/stop/status + MIME parsing
pip install duckdb==1.5.4
python3 test/integration/test_smtp_in.py    # end-to-end: smtplib STARTTLS+AUTH -> SELECT
```

## Roadmap

The email-RFC work is phased. **Phase 1 (done): MIME & Internet Message Format** — RFC 2045–2049 and
822/2822/5322 parsing in `core/` (see above). Remaining phases, in order:

1. **IMAP4rev1 (RFC 3501)** real server on `quackmail_mailboxes`/`quackmail_message_flags`, reusing
   `mime::FlattenParts`/`MimeEntity` for `FETCH BODYSTRUCTURE`/`BODY[...]`; then the extension batch
   (LITERAL+, IDLE, UNSELECT, ENABLE, NAMESPACE, UIDPLUS, MOVE, CONDSTORE/QRESYNC, …).
2. **SCRAM-SHA-256 SASL (RFC 5802/7677)** plus a reusable `core/` SASL module (adds SCRAM credential
   storage alongside the current salted-SHA-256 scheme).
3. **SMTP/LMTP extensions**: LMTP (RFC 2033), submission (6409), enhanced codes (2034), pipelining
   (2920), CHUNKING/BDAT (3030), 8BITMIME (6152), and the `smtp_out` relay/queue drainer.
4. **Hardening**: bcrypt/argon2 hashing, the full Sieve feature set, charset transcoding beyond
   UTF-8/Latin-1 (RFC 2231 continuations), and DKIM/SPF/DMARC for outbound.
