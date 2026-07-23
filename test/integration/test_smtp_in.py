#!/usr/bin/env python3
"""End-to-end test for the inbound MX gateway over the shared Citadel store.

Loads the extensions into an in-memory DuckDB, starts the inbound SMTP listener
(port 25 in production; a high port here), and checks the MX contract:
  * mail for a known local user is accepted and delivered into their Mail room,
  * an unknown local user is rejected (550 5.1.1),
  * a foreign domain is rejected as relay (550 5.7.1),
then retrieves the delivered message back over POP3 (shared store). The MX offers
no AUTH — authenticated submission lives in smtp_out (see test_submission.py).

Requires: pip install duckdb==1.5.4. Run after `make`.
"""
import os
import poplib
import smtplib
import time
from email.mime.text import MIMEText

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
SMTP_PORT = 3525
POP_PORT = 3110


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    con.execute(f"LOAD '{ext('quackmail_smtp_in')}'")
    con.execute(f"LOAD '{ext('quackmail_pop3')}'")

    assert con.execute("SELECT ok FROM qm_user_add('alice', 'secret')").fetchone()[0]

    note = con.execute(f"SELECT note FROM qm_smtp_in_start('{HOST}', {SMTP_PORT})").fetchone()[0]
    assert note == "started", f"server did not start: {note}"
    time.sleep(0.3)

    try:
        msg = MIMEText("This is the body.\n")
        msg["Subject"] = "Hello QuackMail"
        msg["From"] = "bob@example.com"
        msg["To"] = "alice@quackmail.test"
        msg["Message-ID"] = "<test-123@example.com>"

        # Known local user -> accepted.
        s = smtplib.SMTP(HOST, SMTP_PORT, timeout=10)
        s.sendmail("bob@example.com", ["alice@quackmail.test"], msg.as_string())

        # Unknown local user -> 550 5.1.1.
        try:
            s.sendmail("bob@example.com", ["nobody@quackmail.test"], msg.as_string())
            raise AssertionError("unknown local user should be rejected")
        except smtplib.SMTPRecipientsRefused as e:
            code = list(e.recipients.values())[0][0]
            assert code == 550, f"unknown user reply: {e.recipients}"

        # Foreign domain -> 550 5.7.1 relay denied.
        try:
            s.sendmail("bob@example.com", ["stranger@elsewhere.example"], msg.as_string())
            raise AssertionError("foreign domain should be relay-denied")
        except smtplib.SMTPRecipientsRefused as e:
            code, text = list(e.recipients.values())[0]
            assert code == 550 and b"elay" in text, f"relay reply: {e.recipients}"
        s.quit()
    finally:
        con.execute("CALL qm_smtp_in_stop()").fetchall()

    rows = con.execute(
        "SELECT author, subject, format_type FROM citadel_messages"
    ).fetchall()
    assert len(rows) == 1, f"expected exactly 1 delivered message, got {rows}"
    assert rows[0][0] == "bob@example.com" and rows[0][1] == "Hello QuackMail" and rows[0][2] == 4, rows[0]

    # Retrieve it back through POP3 (shared store).
    assert con.execute(f"SELECT note FROM qm_pop3_start('{HOST}', {POP_PORT})").fetchone()[0] == "started"
    time.sleep(0.3)
    try:
        p = poplib.POP3(HOST, POP_PORT, timeout=10)
        p.user("alice")
        p.pass_("secret")
        count, _ = p.stat()
        assert count == 1, f"POP3 STAT expected 1, got {count}"
        body = b"\r\n".join(p.retr(1)[1])
        assert b"Hello QuackMail" in body and b"This is the body." in body
        p.quit()
    finally:
        con.execute("CALL qm_pop3_stop()").fetchall()

    print("PASS: inbound MX validates recipients, delivers local mail, POP3 retrieves it")


if __name__ == "__main__":
    main()
