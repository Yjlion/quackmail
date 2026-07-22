#!/usr/bin/env python3
"""End-to-end test for the mail gateways over the shared Citadel store.

Runs entirely in one process: loads the extensions into an in-memory DuckDB,
starts the inbound SMTP listener (STARTTLS + AUTH), sends a message with
smtplib, asserts it landed in the recipient's Citadel Mail room, then retrieves
it back through the POP3 gateway — proving SMTP-in and POP3 share one store.

Requires: pip install duckdb==1.5.4  (matching the submodule-pinned DuckDB).
Run after `make` so the loadable extensions exist under build/release/extension.
"""
import os
import poplib
import smtplib
import ssl
import time
from email.mime.text import MIMEText

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
SMTP_PORT = 2526
POP_PORT = 1111


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    con.execute(f"LOAD '{ext('quackmail_smtp_in')}'")
    con.execute(f"LOAD '{ext('quackmail_pop3')}'")

    ok = con.execute("SELECT ok FROM qm_user_add('alice', 'secret')").fetchone()[0]
    assert ok, "qm_user_add failed"

    note = con.execute(
        f"SELECT note FROM qm_smtp_in_start('{HOST}', {SMTP_PORT}, starttls=true)"
    ).fetchone()[0]
    assert note == "started", f"server did not start: {note}"
    time.sleep(0.3)

    try:
        # Plaintext AUTH must be refused before STARTTLS.
        pre = smtplib.SMTP(HOST, SMTP_PORT, timeout=10)
        pre.ehlo()
        try:
            pre.login("alice", "secret")
            raise AssertionError("AUTH should be refused before STARTTLS")
        except smtplib.SMTPException:
            pass
        pre.quit()

        # Real flow: EHLO -> STARTTLS -> AUTH -> send.
        s = smtplib.SMTP(HOST, SMTP_PORT, timeout=10)
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

    # It should have been delivered as an RFC822 (format 4) message and pointed
    # into alice's personal Mail room.
    rows = con.execute(
        "SELECT author, recipient, subject, format_type FROM citadel_messages"
    ).fetchall()
    assert len(rows) == 1, f"expected 1 message, got {rows}"
    assert rows[0][0] == "bob@example.com", rows[0]
    assert rows[0][2] == "Hello QuackMail", rows[0]
    assert rows[0][3] == 4, rows[0]

    in_mail = con.execute(
        "SELECT count(*) FROM citadel_room_msgs rm "
        "JOIN citadel_rooms r ON r.room_num = rm.room_num "
        "WHERE r.mailbox_owner > 0 AND r.display_name = 'Mail'"
    ).fetchone()[0]
    assert in_mail == 1, "message should be pointed into a personal Mail room"

    # Now retrieve it back through the POP3 gateway.
    note = con.execute(
        f"SELECT note FROM qm_pop3_start('{HOST}', {POP_PORT})"
    ).fetchone()[0]
    assert note == "started", f"pop3 did not start: {note}"
    time.sleep(0.3)
    try:
        p = poplib.POP3(HOST, POP_PORT, timeout=10)
        p.user("alice")
        p.pass_("secret")
        count, _size = p.stat()
        assert count == 1, f"POP3 STAT expected 1 message, got {count}"
        _resp, lines, _octets = p.retr(1)
        body = b"\r\n".join(lines)
        assert b"Hello QuackMail" in body, "Subject missing from POP3 RETR"
        assert b"This is the body." in body, "Body missing from POP3 RETR"
        p.quit()
    finally:
        con.execute("CALL qm_pop3_stop()").fetchall()

    print("PASS: SMTP delivery -> Citadel Mail room -> POP3 retrieval")


if __name__ == "__main__":
    main()
