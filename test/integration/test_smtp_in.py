#!/usr/bin/env python3
"""End-to-end test for the quackmail_smtp_in extension.

Runs entirely in one process: loads the QuackMail extensions into an in-memory
DuckDB, starts the inbound SMTP listener (with STARTTLS + AUTH), sends a message
with smtplib, and asserts it lands in quackmail_messages.

Requires: pip install duckdb==1.5.4  (matching the submodule-pinned DuckDB).
Run after `make` so the loadable extensions exist under build/release/extension.
"""
import os
import smtplib
import ssl
import sys
import time
from email.mime.text import MIMEText

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
PORT = 2526
HOST = "127.0.0.1"


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    con.execute(f"LOAD '{ext('quackmail_smtp_in')}'")

    # Create a local user for AUTH.
    ok = con.execute("SELECT ok FROM qm_user_add('alice', 'secret')").fetchone()[0]
    assert ok, "qm_user_add failed"

    # Start the inbound SMTP server with STARTTLS enabled (self-signed cert).
    note = con.execute(
        f"SELECT note FROM qm_smtp_in_start('{HOST}', {PORT}, starttls=true)"
    ).fetchone()[0]
    assert note == "started", f"server did not start: {note}"
    time.sleep(0.3)

    try:
        # Plaintext AUTH must be refused before STARTTLS.
        pre = smtplib.SMTP(HOST, PORT, timeout=10)
        pre.ehlo()
        try:
            pre.login("alice", "secret")
            raise AssertionError("AUTH should be refused before STARTTLS")
        except smtplib.SMTPException:
            pass
        pre.quit()

        # Now the real flow: EHLO -> STARTTLS -> AUTH -> send.
        s = smtplib.SMTP(HOST, PORT, timeout=10)
        s.ehlo()
        s.starttls(context=ssl._create_unverified_context())
        s.ehlo()
        s.login("alice", "secret")

        msg = MIMEText("This is the body.\n")
        msg["Subject"] = "Hello QuackMail"
        msg["From"] = "bob@example.com"
        msg["To"] = "alice@quackmail.test"
        msg["Message-ID"] = "<test-123@example.com>"
        s.sendmail("bob@example.com", ["alice@quackmail.test"], msg.as_string())
        s.quit()
    finally:
        con.execute("CALL qm_smtp_in_stop()").fetchall()

    rows = con.execute(
        "SELECT from_addr, subject, message_id FROM quackmail_messages"
    ).fetchall()
    assert len(rows) == 1, f"expected 1 message, got {rows}"
    assert rows[0][0] == "bob@example.com", rows[0]
    assert rows[0][1] == "Hello QuackMail", rows[0]

    rcpts = con.execute("SELECT rcpt FROM quackmail_recipients").fetchall()
    assert rcpts == [("alice@quackmail.test",)], rcpts

    hdr = con.execute(
        "SELECT count(*) FROM quackmail_headers WHERE name = 'Subject'"
    ).fetchone()[0]
    assert hdr == 1, "expected the Subject header to be stored"

    print("PASS: message received over STARTTLS+AUTH and stored in DuckDB")


if __name__ == "__main__":
    main()
