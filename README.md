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
| `quackmail` (umbrella) | `qm_version`, `qm_status`, `qm_user_add`, `qm_user_remove` | ✅ |
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

## Build

Requires a C++17 compiler, CMake, Ninja/Make, and `libssl-dev`.

```bash
git clone --recurse-submodules <repo>       # duckdb + extension-ci-tools are submodules (pinned v1.5.4)
cd quackmail
GEN=ninja make                              # builds DuckDB + all extensions
```

Artifacts land in `build/release/extension/<name>/<name>.duckdb_extension`, and a
DuckDB CLI with every extension statically linked is at `build/release/duckdb`.

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
make test                                   # sqllogictest: load + start/stop/status
pip install duckdb==1.5.4
python3 test/integration/test_smtp_in.py    # end-to-end: smtplib STARTTLS+AUTH -> SELECT
```

## Roadmap

Fill in the stubbed extensions (smtp_out relay, IMAP, ManageSieve, POP3), add
bcrypt/argon2 hashing, the full Sieve feature set, and DKIM/SPF/DMARC for outbound.
