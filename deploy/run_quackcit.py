#!/usr/bin/env python3
"""Launcher: load the QuackCit extensions, start every listener, then idle.

Runs the whole server in one long-lived process against a persistent DuckDB
file. Everything is configured through the environment, so `quackcit.sh` (which
sources `quackcit.conf`) can drive it without editing this file:

    QUACKCIT_EXT_DIR   default: <repo>/build/release/extension
    QUACKCIT_DB        default: <repo>/quackcit.duckdb
    QUACKCIT_HOST      default: 127.0.0.1
    QUACKCIT_TLS_CERT  optional PEM certificate for STARTTLS/implicit TLS
    QUACKCIT_TLS_KEY   optional PEM private key
    QUACKCIT_ADMIN_SOCK  default: <db>.admin.sock ("" disables the channel)

Per service, where NAME is the key in SERVICES below:

    QUACKCIT_PORT_<NAME>     override the port
    QUACKCIT_ENABLE_<NAME>   0/1 to disable or enable that listener

Requires the matching DuckDB Python package (pip install duckdb==<pinned>), and
that `make` has produced the loadable extensions.

    python3 deploy/run_quackcit.py     # foreground
    deploy/quackcit.sh start           # backgrounded, with a PID file
"""
import json
import os
import signal
import socket
import sys
import threading
import time

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXT_DIR = os.environ.get("QUACKCIT_EXT_DIR", os.path.join(REPO, "build", "release", "extension"))
DB = os.environ.get("QUACKCIT_DB", os.path.join(REPO, "quackcit.duckdb"))
HOST = os.environ.get("QUACKCIT_HOST", "127.0.0.1")
TLS_CERT = os.environ.get("QUACKCIT_TLS_CERT", "")
TLS_KEY = os.environ.get("QUACKCIT_TLS_KEY", "")

# Every listener the server can bring up: the extension providing it, the
# control function prefix, its default dev port, and its TLS mode. Ports are
# dev/non-privileged; the module (not the port) defines the service.
#
# `host` overrides the bind address for services that must not be exposed.
SERVICES = [
    # key            extension                 prefix                 port   tls
    ("CITADEL",      "quackmail_citadel",      "cit",                 5040,  "starttls"),
    ("SMTP_IN",      "quackmail_smtp_in",      "qm_smtp_in",          2525,  "starttls"),
    # LMTP performs no sender authentication and no spam filtering by design:
    # anything that can reach this socket can inject mail. Loopback only.
    ("LMTP",         "quackmail_smtp_in",      "qm_lmtp",             2033,  "none",
     {"host": "127.0.0.1"}),
    ("SUBMISSION",   "quackmail_smtp_out",     "qm_smtp_submission",  2587,  "starttls"),
    ("SMTPS",        "quackmail_smtp_out",     "qm_smtp_smtps",       2465,  "implicit"),
    ("POP3",         "quackmail_pop3",         "qm_pop3",             1110,  "starttls"),
    ("POP3S",        "quackmail_pop3",         "qm_pop3s",            1995,  "implicit"),
    ("IMAP",         "quackmail_imap",         "qm_imap",             1143,  "starttls"),
    ("MANAGESIEVE",  "quackmail_managesieve",  "qm_managesieve",      4190,  "starttls"),
    ("TELNET",       "quackmail_telnet",       "qm_telnet",           2300,  "none"),
    ("TELNETS",      "quackmail_telnet",       "qm_telnets",          2992,  "implicit"),
    ("NNTP",         "quackmail_nntp",         "qm_nntp",             1119,  "starttls"),
    ("NNTPS",        "quackmail_nntp",         "qm_nntps",            1563,  "implicit"),
    ("XMPP",         "quackmail_xmpp",         "qm_xmpp",             15222, "starttls"),
    ("XMPPS",        "quackmail_xmpp",         "qm_xmpps",            15223, "implicit"),
]

# Users seeded on first run. Mirrors the reference Citadel box (admin/admin at
# aide level, leo/leo as a regular user) so protocol output can be diffed 1:1.
SEED_USERS = [
    ("admin", "admin", 6),
    ("leo", "leo", 4),
]


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def env_flag(name, default=True):
    v = os.environ.get(name)
    if v is None:
        return default
    return v.strip().lower() in ("1", "true", "yes", "on")


def enabled_services():
    """The services to start, with their resolved port and bind address."""
    out = []
    for entry in SERVICES:
        key, extension, prefix, default_port, tls = entry[:5]
        opts = entry[5] if len(entry) > 5 else {}
        if not env_flag(f"QUACKCIT_ENABLE_{key}", True):
            continue
        port = int(os.environ.get(f"QUACKCIT_PORT_{key}", default_port))
        host = os.environ.get(f"QUACKCIT_HOST_{key}", opts.get("host", HOST))
        out.append((key, extension, prefix, host, port, tls))
    return out


def start_call(prefix, host, port, tls):
    """Build the CALL that starts one listener, with TLS wired if configured."""
    args = [f"'{host}'", str(port)]
    have_tls = TLS_CERT and TLS_KEY
    if have_tls:
        args.append(f"tls_cert => '{TLS_CERT}'")
        args.append(f"tls_key => '{TLS_KEY}'")
    if tls == "starttls" and have_tls:
        args.append("starttls => true")
    elif tls == "starttls":
        # Without a certificate STARTTLS cannot be offered; the listener still
        # runs in the clear, which is what a dev box wants.
        pass
    elif tls == "implicit":
        if not have_tls:
            return None  # an implicit-TLS listener without a cert cannot work
        args.append("implicit_tls => true")
    return f"CALL {prefix}_start({', '.join(args)})"


class AdminChannel:
    """A local control socket for the admin CLI.

    DuckDB allows exactly one read-write process per database file, so while
    this launcher holds the DB no other process can open it to make changes.
    The admin CLI therefore sends SQL here instead. The socket is created mode
    0600 next to the database: it accepts arbitrary SQL, so it is effectively a
    root-equivalent control channel and must not live on a shared mount.
    """

    def __init__(self, con, path):
        self.con = con
        self.path = path
        self.lock = threading.Lock()
        self.sock = None
        self.thread = None
        self.stop = False

    def start(self):
        if not self.path:
            return
        try:
            if os.path.exists(self.path):
                os.unlink(self.path)
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.bind(self.path)
            os.chmod(self.path, 0o600)
            self.sock.listen(4)
            self.sock.settimeout(0.5)
        except OSError as e:
            print(f"admin channel unavailable ({e})", flush=True)
            self.sock = None
            return
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()
        print(f"admin channel: {self.path}", flush=True)

    def _serve(self):
        while not self.stop:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with conn:
                try:
                    self._handle(conn)
                except Exception as e:  # one bad client must not kill the channel
                    try:
                        conn.sendall((json.dumps({"ok": False, "error": str(e)}) + "\n").encode())
                    except OSError:
                        pass

    def _handle(self, conn):
        conn.settimeout(30)
        buf = b""
        while b"\n" not in buf:
            chunk = conn.recv(65536)
            if not chunk:
                return
            buf += chunk
        request = json.loads(buf.split(b"\n", 1)[0].decode())
        sql = request.get("sql", "")
        params = request.get("params", [])

        # The DuckDB connection is not thread-safe against the listener threads
        # running their own connections, but this one is ours alone; serialise
        # concurrent admin clients against each other.
        with self.lock:
            try:
                cur = self.con.execute(sql, params) if params else self.con.execute(sql)
                columns = [d[0] for d in cur.description] if cur.description else []
                rows = cur.fetchall() if columns else []
                payload = {
                    "ok": True,
                    "columns": columns,
                    "rows": [[None if v is None else str(v) for v in row] for row in rows],
                }
            except Exception as e:
                payload = {"ok": False, "error": str(e)}
        conn.sendall((json.dumps(payload) + "\n").encode())

    def close(self):
        self.stop = True
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        if self.path and os.path.exists(self.path):
            try:
                os.unlink(self.path)
            except OSError:
                pass


def main():
    services = enabled_services()
    if not services:
        print("no services enabled; nothing to do", file=sys.stderr)
        return 1

    con = duckdb.connect(DB, config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    for _key, extension, _prefix, _host, _port, _tls in services:
        con.execute(f"LOAD '{ext(extension)}'")

    # Seed the reference users the first time so the server is immediately usable
    # and matches the oracle box for parity diffing.
    if con.execute("SELECT count(*) FROM quackmail_users").fetchone()[0] == 0:
        for name, pw, axlevel in SEED_USERS:
            con.execute("CALL qm_user_add(?, ?)", [name, pw])
            # Log the user in once so their default rooms get provisioned, and set
            # the aide access level where needed.
            con.execute(
                "INSERT INTO citadel_users (username, usernum, axlevel) VALUES "
                "(?, nextval('citadel_user_seq'), ?) "
                "ON CONFLICT (username) DO UPDATE SET axlevel = excluded.axlevel",
                [name, axlevel],
            )
        print("seeded users:", ", ".join(u[0] for u in SEED_USERS), flush=True)

    started = []
    for key, _extension, prefix, host, port, tls in services:
        call = start_call(prefix, host, port, tls)
        if call is None:
            print(f"skipped: {key} needs QUACKCIT_TLS_CERT/KEY for implicit TLS", flush=True)
            continue
        try:
            print(f"started: {key}", con.execute(call).fetchone(), flush=True)
            started.append((prefix, key, host, port))
        except Exception as e:
            print(f"FAILED to start {key} on {host}:{port}: {e}", flush=True)

    admin = AdminChannel(con, os.environ.get("QUACKCIT_ADMIN_SOCK", DB + ".admin.sock"))
    admin.start()

    print(
        "QuackCit up: " + ", ".join(f"{k.lower()} {h}:{p}" for _pfx, k, h, p in started) +
        f"; db={DB}",
        flush=True,
    )
    # Site policy starts empty, which means: only c_fqdn is accepted as a local
    # domain, no DNSBL is queried, and no outbound mail is DKIM-signed.
    print("policy tables are empty by default — see deploy/quackcitadm.sh", flush=True)

    stop = {"v": False}
    signal.signal(signal.SIGTERM, lambda *_: stop.__setitem__("v", True))
    signal.signal(signal.SIGINT, lambda *_: stop.__setitem__("v", True))
    while not stop["v"]:
        time.sleep(1)

    admin.close()
    for prefix, key, _host, _port in started:
        try:
            con.execute(f"CALL {prefix}_stop()")
        except Exception:
            pass
    print("QuackCit stopped", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
