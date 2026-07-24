#!/usr/bin/env python3
"""End-to-end test for the inbound MX gateway over the shared Citadel store.

Loads the extensions into an in-memory DuckDB, starts the inbound SMTP listener
(port 25 in production; a high port here), and checks the MX contract:
  * mail for a known local user is accepted and delivered into their Mail room,
  * an unknown local user is rejected (550 5.1.1),
  * a foreign domain is rejected as relay (550 5.7.1),
then retrieves the delivered message back over POP3 (shared store). The MX offers
no AUTH — authenticated submission lives in smtp_out (see test_submission.py).

The second half covers site policy: an additional hosted domain, alias and
catch-all expansion, sender/IP block rules, and the trace headers the inbound
checks stamp on every accepted message.

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

    # Every accepted message is stamped with what the inbound checks concluded.
    stored = con.execute("SELECT decode(raw) FROM citadel_messages").fetchone()[0]
    assert "Received: from" in stored, "no Received header was added"
    assert "Authentication-Results:" in stored, "no Authentication-Results header"
    assert "dkim=none" in stored, f"unsigned mail should report dkim=none: {stored[:400]}"
    # The message body survived the prepended headers intact.
    assert "This is the body." in stored

    # The audit trail records one row per accepted recipient.
    logged = con.execute(
        "SELECT rcpt, disposition FROM quackmail_inbound_log ORDER BY at"
    ).fetchall()
    assert ("alice@quackmail.test", "accept") in logged, logged

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

    check_policy(con)

    print("PASS: inbound MX validates recipients, delivers local mail, POP3 retrieves it,")
    print("      and honours domains, aliases and block rules")


def check_policy(con):
    """Hosted domains, aliases, catch-alls and allow/block rules."""
    assert con.execute("SELECT ok FROM qm_user_add('carol', 'pw')").fetchone()[0]
    assert con.execute("SELECT ok FROM qm_domain_add('extra.test', 'local')").fetchone()[0]
    # An address alias fanning out to two users, plus a domain catch-all.
    assert con.execute("SELECT ok FROM qm_alias_add('team@extra.test', 'alice')").fetchone()[0]
    assert con.execute("SELECT ok FROM qm_alias_add('team@extra.test', 'carol')").fetchone()[0]
    assert con.execute("SELECT ok FROM qm_alias_add('@extra.test', 'carol')").fetchone()[0]
    # An alias may point off-site; that recipient is forwarded, not rejected.
    assert con.execute(
        "SELECT ok FROM qm_alias_add('fwd@extra.test', 'elsewhere@example.net')"
    ).fetchone()[0]
    # A blocked sender pattern.
    assert con.execute(
        "SELECT ok FROM qm_acl_add('sender', '*@blocked.example', 'block', 'test rule')"
    ).fetchone()[0]

    assert con.execute(
        f"SELECT note FROM qm_smtp_in_start('{HOST}', {SMTP_PORT + 1})"
    ).fetchone()[0] == "started"
    time.sleep(0.3)
    try:
        s = smtplib.SMTP(HOST, SMTP_PORT + 1, timeout=10)

        def send(sender, rcpt, subject):
            m = MIMEText("policy body\n")
            m["Subject"] = subject
            m["From"] = sender
            m["To"] = rcpt
            s.sendmail(sender, [rcpt], m.as_string())

        # The newly hosted domain is accepted; the alias fans out to two users.
        send("outside@example.com", "team@extra.test", "To the team")

        # An address with no alias falls through to the domain catch-all.
        send("outside@example.com", "whoever@extra.test", "To the catch-all")

        # An alias pointing off-site is accepted and queued for forwarding.
        send("outside@example.com", "fwd@extra.test", "To be forwarded")

        # A blocked sender is refused at MAIL FROM, before any recipient.
        try:
            send("spammer@blocked.example", "alice@quackmail.test", "Should not arrive")
            raise AssertionError("blocked sender should have been refused")
        except smtplib.SMTPSenderRefused as e:
            assert e.smtp_code == 550, f"blocked sender reply: {e.smtp_code} {e.smtp_error}"
            assert b"test rule" in e.smtp_error, e.smtp_error

        # A domain we do not host is still relay-denied.
        try:
            send("outside@example.com", "someone@unhosted.test", "Should not relay")
            raise AssertionError("unhosted domain should be relay-denied")
        except smtplib.SMTPRecipientsRefused as e:
            code, text = list(e.recipients.values())[0]
            assert code == 550 and b"elay" in text, e.recipients

        s.quit()
    finally:
        con.execute("CALL qm_smtp_in_stop()").fetchall()

    # The alias reached both mailboxes; the catch-all reached carol's only.
    fanout = con.execute(
        "SELECT count(DISTINCT r.mailbox_owner) FROM citadel_messages m "
        "JOIN citadel_room_msgs rm ON rm.msgnum = m.msgnum "
        "JOIN citadel_rooms r ON r.room_num = rm.room_num "
        "WHERE m.subject = 'To the team' AND r.mailbox_owner > 0"
    ).fetchone()[0]
    assert fanout == 2, f"alias should reach 2 mailboxes, got {fanout}"

    catchall = con.execute(
        "SELECT count(*) FROM citadel_messages WHERE subject = 'To the catch-all'"
    ).fetchone()[0]
    assert catchall == 1, f"catch-all should have delivered once, got {catchall}"

    blocked = con.execute(
        "SELECT count(*) FROM citadel_messages WHERE subject = 'Should not arrive'"
    ).fetchone()[0]
    assert blocked == 0, "a blocked sender's message was stored"

    # The off-site alias was queued for relay rather than stored or refused.
    forwarded = con.execute(
        "SELECT count(*) FROM quackmail_outbound WHERE rcpt = 'elsewhere@example.net'"
    ).fetchone()[0]
    assert forwarded == 1, f"off-site alias should be queued once, got {forwarded}"
    assert con.execute(
        "SELECT count(*) FROM citadel_messages WHERE subject = 'To be forwarded'"
    ).fetchone()[0] == 0, "a forward-only alias should store nothing locally"


if __name__ == "__main__":
    main()
