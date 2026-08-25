# Installing and building

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
extensions and attaches a `quackmail-<tag>-linux_amd64.tar.gz` bundle to a
GitHub Release. The bundle is self-contained — the `.duckdb_extension` files, a
statically linked `duckdb` CLI, and the `deploy/` scripts that run them as a
server. Nothing else is needed: no Python, no separate DuckDB.

```bash
sudo tar -xzf quackmail-<tag>-linux_amd64.tar.gz -C /opt
sudo mv /opt/quackmail-<tag>-linux_amd64 /opt/quackmail

sudo /opt/quackmail/quackcit.sh start
sudo /opt/quackmail/quackcitadm.sh user add alice s3cret
```

The scripts notice they are in an unpacked bundle rather than a checkout and
put the database in `/var/lib/quackcit`, logs in `/var/log/quackcit` and the
control FIFO in `/run/quackcit`; point `QUACKCIT_STATE_DIR`, `QUACKCIT_LOG_DIR`
and `QUACKCIT_RUN_DIR` somewhere writable to run without root.
`quackcit.service` in the bundle is a systemd unit template.

To drive DuckDB by hand instead:

```bash
cd /opt/quackmail
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
