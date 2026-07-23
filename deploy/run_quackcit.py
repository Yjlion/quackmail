#!/usr/bin/env python3
"""Dev launcher: load the QuackCit extensions, start every listener, then idle.

Runs the whole server in one long-lived process against a persistent DuckDB
file — handy for a dev box. Paths and bind address come from the environment
(with sensible defaults derived from this checkout):

    QUACKCIT_EXT_DIR   default: <repo>/build/release/extension
    QUACKCIT_DB        default: <repo>/quackcit.duckdb
    QUACKCIT_HOST      default: 127.0.0.1

Requires the matching DuckDB Python package (pip install duckdb==<pinned>), and
that `make` has produced the loadable extensions.

    python3 deploy/run_quackcit.py     # foreground
    nohup python3 deploy/run_quackcit.py > quackcit.log 2>&1 &   # background
"""
import os
import signal
import sys
import time

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXT_DIR = os.environ.get("QUACKCIT_EXT_DIR", os.path.join(REPO, "build", "release", "extension"))
DB = os.environ.get("QUACKCIT_DB", os.path.join(REPO, "quackcit.duckdb"))
HOST = os.environ.get("QUACKCIT_HOST", "127.0.0.1")

# (extension, "CALL ...") listeners to bring up. Ports are dev/non-privileged;
# the module (not the port) defines the service. SMTP is split into an inbound
# MX (2525), STARTTLS submission (2587), and implicit-TLS submission (2465).
LISTENERS = [
    ("quackmail_citadel", f"CALL cit_start('{HOST}', 5040)"),
    ("quackmail_smtp_in", f"CALL qm_smtp_in_start('{HOST}', 2525, starttls=>true)"),
    ("quackmail_smtp_out", f"CALL qm_smtp_submission_start('{HOST}', 2587, starttls=>true)"),
    ("quackmail_smtp_out", f"CALL qm_smtp_smtps_start('{HOST}', 2465, implicit_tls=>true)"),
    ("quackmail_pop3", f"CALL qm_pop3_start('{HOST}', 1110)"),
    ("quackmail_imap", f"CALL qm_imap_start('{HOST}', 1143)"),
]


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def main():
    con = duckdb.connect(DB, config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    for name, _ in LISTENERS:
        con.execute(f"LOAD '{ext(name)}'")

    # Seed a demo user the first time so the server is immediately usable.
    if con.execute("SELECT count(*) FROM quackmail_users").fetchone()[0] == 0:
        con.execute("CALL qm_user_add('alice', 'secret')")
        print("seeded demo user alice/secret", flush=True)

    for _name, call in LISTENERS:
        print("started:", con.execute(call).fetchone(), flush=True)

    print(
        f"QuackCit up on {HOST} (citadel 5040, smtp-in 2525, submission 2587, "
        f"smtps 2465, pop3 1110, imap 1143); db={DB}",
        flush=True,
    )

    stop = {"v": False}
    signal.signal(signal.SIGTERM, lambda *_: stop.__setitem__("v", True))
    signal.signal(signal.SIGINT, lambda *_: stop.__setitem__("v", True))
    while not stop["v"]:
        time.sleep(1)

    for name, call in LISTENERS:
        stop_call = call.split("(")[0].replace("_start", "_stop") + "()"
        try:
            con.execute(stop_call)
        except Exception:
            pass
    print("QuackCit stopped", flush=True)


if __name__ == "__main__":
    main()
