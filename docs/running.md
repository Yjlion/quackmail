# Running a server

## Running and administering a server

`deploy/` has two scripts, both POSIX shell with no interpreter to install. Both
read `deploy/quackcit.conf` (override the path with `QUACKCIT_CONF`), where the
database location, bind address, TLS material and every port live. Values
already in the environment win, so one-off overrides work. They work the same
from a git checkout and from an unpacked release, detecting which they are in.

```bash
deploy/quackcit.sh start          # background, PID file, log file
deploy/quackcit.sh status
deploy/quackcit.sh logs 100
deploy/quackcit.sh stop

QUACKCIT_PORT_SMTP=25 deploy/quackcit.sh foreground   # run in this terminal
```

**TLS out of the box.** With no certificate configured, the first `start`
generates a self-signed one under `$QUACKCIT_TLS_DIR` (`/var/lib/quackcit/tls`
in a release, beside the database in a checkout) and starts every listener with
it — including the implicit-TLS ones, SUBMISSIONS/POP3S/IMAPS/TELNETS/NNTPS/XMPPS and the
HTTPS web interface, which are skipped outright when there is no certificate to
give them. It is issued to `hostname -f` with `localhost`, `127.0.0.1` and `::1`
as alternative names, lasts ten years, and is reused by every restart, so a
client that accepted the fingerprint keeps working. Nothing will trust it —
for production point `QUACKCIT_TLS_CERT` and `QUACKCIT_TLS_KEY` at real material
(a Let's Encrypt `fullchain.pem`/`privkey.pem` pair works as-is), which takes
precedence and disables the generated one. `QUACKCIT_TLS_AUTOGEN=0` opts out
entirely.

**One account, with a password you have to go and read.** The first start of an
empty database creates the accounts in `QUACKCIT_SEED_USERS`, which defaults to
a single aide named `admin`. Its password is generated — 24 alphanumerics from
`openssl` or `/dev/urandom` — and written to
`$QUACKCIT_SEED_SECRET_FILE` (`/var/lib/quackcit/initial-credentials` in a
release), mode `0600`. It is never printed to the terminal or into the log;
only the path is. Change it and delete the file. Seed whoever you like instead,
generating some passwords and choosing others:

```sh
: "${QUACKCIT_SEED_USERS:=admin - 6
alice - 4
bob hunter2 4}"
```

`<name> <password> <axlevel>` per line, `-` for "generate one", access level 6
for an aide and 4 for an ordinary user. This runs only while the database has
no accounts at all — it cannot reset a password later, so use
`quackcitadm.sh user add` after that.

**The server names itself after the host.** The first start resolves
`hostname -f` (then `hostname`, then `localhost`) and stores it as `c_fqdn`,
the name used for SMTP banners, `Received:` and `Message-ID` headers, the domain
mail is accepted for, and the certificate subject. It only ever replaces an
empty value or the `quackmail.test` placeholder, so a name you chose survives
every restart. Set `QUACKCIT_FQDN` to decide it yourself, in which case it is
authoritative and applied on every start.

**The log is syslog.** `quackcit.log` is RFC 5424, one record per line:

```
<22>1 2026-07-28T15:41:02Z mail quackcit 12345 server - qm_http	true	0.0.0.0	8080	0	started
```

`<22>` is `mail.info` — set `QUACKCIT_SYSLOG_FACILITY` to any facility name or
number. Everything the server prints goes through it, so a collector tailing the
file gets the same records `quackcit.sh logs` shows you. `quackcit.err` is
deliberately *not* in that format and should not be pointed at a collector: it
is the control channel's error return path, and `quackcitadm.sh` reads a failed
request's error text out of it byte for byte. Neither file is rotated yet.

`quackcitadm.sh` administers the server — users, domains, aliases, access
rules, DKIM keys, send and storage quotas, Sieve scripts, the outbound queue and
server config:

```bash
deploy/quackcitadm.sh user add alice s3cret
deploy/quackcitadm.sh domain add example.com
deploy/quackcitadm.sh alias add sales@example.com alice
deploy/quackcitadm.sh alias add @example.com catchall     # domain catch-all
deploy/quackcitadm.sh acl block ip 192.0.2.0/24 'noisy range'
deploy/quackcitadm.sh acl allow ip 192.0.2.7 'except this host'
deploy/quackcitadm.sh rbl add zen.spamhaus.org
deploy/quackcitadm.sh dkim keygen example.com mail
deploy/quackcitadm.sh ratelimit status alice               # how much may they send
deploy/quackcitadm.sh quota set alice 500MB               # how much may they keep
deploy/quackcitadm.sh queue list
deploy/quackcitadm.sh help
```

It works whether or not the server is running. **DuckDB allows a single
read-write process per database file**, so while the server holds it the script
cannot simply open the file; it sends the statement over the server's control
channel instead, and falls back to opening the database directly when the server
is stopped.

The server *is* the DuckDB CLI, holding the database open with every listener
extension loaded — the listeners are threads inside it. Its standard input is a
FIFO opened read-write, which is both what keeps the process alive (the read
never reaches end-of-file) and what `quackcitadm.sh` administers it through: a
request is SQL wrapped in `.output` redirections, and the reply is the file the
CLI writes. The FIFO accepts arbitrary SQL, so it is created mode 0600 and
should not live on a shared filesystem.
