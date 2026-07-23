#!/usr/bin/env python3
"""End-to-end test for authenticated submission (smtp_out).

Starts the STARTTLS submission listener, and checks the submission contract:
  * MAIL before AUTH is refused (530),
  * after STARTTLS + AUTH, a message to a local user is delivered into their
    Mail room, and a message to a remote domain is enqueued to quackmail_outbound
    for the relay drainer.

The implicit-TLS endpoint (qm_smtp_smtps_start) shares the same handler and
differs only in TLS mode, so it is not separately re-tested here.

Requires: pip install duckdb==1.5.4. Run after `make`.
"""
import os
import smtplib
import ssl
import time
from email.mime.text import MIMEText

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
SUB_PORT = 3587


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def msg(to):
    m = MIMEText("submission body\n")
    m["Subject"] = "Submitted"
    m["From"] = "alice@quackmail.test"
    m["To"] = to
    return m.as_string()


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    con.execute(f"LOAD '{ext('quackmail_smtp_out')}'")

    assert con.execute("SELECT ok FROM qm_user_add('alice', 'secret')").fetchone()[0]

    note = con.execute(
        f"SELECT note FROM qm_smtp_submission_start('{HOST}', {SUB_PORT}, starttls=true)"
    ).fetchone()[0]
    assert note == "started", f"submission did not start: {note}"
    time.sleep(0.3)

    try:
        # MAIL before AUTH must be refused.
        s = smtplib.SMTP(HOST, SUB_PORT, timeout=10)
        s.ehlo()
        s.starttls(context=ssl._create_unverified_context())
        s.ehlo()
        code, _ = s.mail("alice@quackmail.test")
        assert code == 530, f"MAIL before AUTH should be 530, got {code}"
        s.rset()

        # Authenticate, then submit to a local and a remote recipient.
        s.login("alice", "secret")
        s.sendmail("alice@quackmail.test", ["alice@quackmail.test"], msg("alice@quackmail.test"))
        s.sendmail("alice@quackmail.test", ["bob@example.com"], msg("bob@example.com"))
        s.quit()
    finally:
        con.execute("CALL qm_smtp_submission_stop()").fetchall()

    # Local recipient: delivered into a personal Mail room.
    local = con.execute(
        "SELECT count(*) FROM citadel_room_msgs rm JOIN citadel_rooms r ON r.room_num = rm.room_num "
        "WHERE r.mailbox_owner > 0 AND r.display_name = 'Mail'"
    ).fetchone()[0]
    assert local == 1, f"local submission should be in the Mail room, got {local}"

    # Remote recipient: queued for relay, not delivered locally.
    queued = con.execute(
        "SELECT count(*) FROM quackmail_outbound WHERE status = 'queued' AND rcpt = 'bob@example.com'"
    ).fetchone()[0]
    assert queued == 1, f"remote submission should be queued, got {queued}"

    print("PASS: submission requires AUTH, delivers local mail, queues remote mail")


if __name__ == "__main__":
    main()
