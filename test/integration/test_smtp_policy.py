#!/usr/bin/env python3
"""End-to-end test for outbound DKIM signing and per-user rate limiting.

  * A message submitted through smtp_out is signed with the domain's key, and
    the signature verifies — proving the bytes we sign are the bytes we store,
    which is the thing that silently breaks in DKIM implementations.
  * A user over their quota gets 451 (transient, so clients retry) rather than
    a permanent rejection, and the message is not delivered.
  * Quota is charged per envelope recipient, not per message.

Verification runs against the locally stored public key, so no DNS is needed.

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
SUB_PORT = 3588


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def message(to, subject):
    m = MIMEText("policy test body\n")
    m["Subject"] = subject
    m["From"] = "alice@quackmail.test"
    m["To"] = to
    return m.as_string()


def connect():
    s = smtplib.SMTP(HOST, SUB_PORT, timeout=10)
    s.ehlo()
    s.starttls(context=ssl._create_unverified_context())
    s.ehlo()
    s.login("alice", "secret")
    return s


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    con.execute(f"LOAD '{ext('quackmail_smtp_out')}'")

    assert con.execute("SELECT ok FROM qm_user_add('alice', 'secret')").fetchone()[0]

    ok, dns_name, record = con.execute(
        "SELECT ok, dns_name, dns_record FROM qm_dkim_keygen('quackmail.test', 'mail', '2048')"
    ).fetchone()
    assert ok, f"key generation failed: {record}"
    assert dns_name == "mail._domainkey.quackmail.test", dns_name
    assert record.startswith("v=DKIM1; k=rsa; p="), record

    note = con.execute(
        f"SELECT note FROM qm_smtp_submission_start('{HOST}', {SUB_PORT}, starttls=true)"
    ).fetchone()[0]
    assert note == "started", f"submission did not start: {note}"
    time.sleep(0.3)

    try:
        # ---- DKIM signing ------------------------------------------------
        s = connect()
        s.sendmail("alice@quackmail.test", ["alice@quackmail.test"],
                   message("alice@quackmail.test", "Signed local"))
        s.sendmail("alice@quackmail.test", ["bob@example.com"],
                   message("bob@example.com", "Signed remote"))
        s.quit()

        stored = con.execute(
            "SELECT decode(raw) FROM citadel_messages WHERE subject = 'Signed local'"
        ).fetchone()[0]
        assert "DKIM-Signature:" in stored, "the stored copy carries no signature"
        assert "d=quackmail.test" in stored, "signature is for the wrong domain"
        assert "Received: from" in stored, "submission left no trace header"

        verdict = con.execute(
            "SELECT qm_dkim_verify(decode(raw)) FROM citadel_messages WHERE subject = 'Signed local'"
        ).fetchone()[0]
        assert verdict == "pass", f"our own signature does not verify: {verdict}"

        # The queued copy is signed too — one signature covers both paths.
        queued = con.execute(
            "SELECT decode(raw) FROM quackmail_outbound WHERE rcpt = 'bob@example.com'"
        ).fetchone()[0]
        assert "DKIM-Signature:" in queued, "the queued copy carries no signature"
        assert con.execute("SELECT qm_dkim_verify(?)", [queued]).fetchone()[0] == "pass"

        # ---- rate limiting -----------------------------------------------
        # Start from a clean window, then allow only 2 recipients per minute.
        con.execute("DELETE FROM quackmail_send_log")
        assert con.execute(
            "SELECT ok FROM qm_ratelimit_set('alice', 2, 60, 100)"
        ).fetchone()[0]

        s = connect()
        s.sendmail("alice@quackmail.test", ["one@example.com"],
                   message("one@example.com", "Quota 1"))
        s.sendmail("alice@quackmail.test", ["two@example.com"],
                   message("two@example.com", "Quota 2"))

        used, allowed = con.execute(
            "SELECT burst_used, allowed FROM qm_rate_status('alice')"
        ).fetchone()
        assert used == 2, f"expected 2 charged, got {used}"
        assert not allowed, "alice should be at her limit"

        # The third is refused with a transient code, naming a retry delay.
        try:
            s.sendmail("alice@quackmail.test", ["three@example.com"],
                       message("three@example.com", "Quota 3"))
            raise AssertionError("over-quota send should have been refused")
        except smtplib.SMTPDataError as e:
            assert e.smtp_code == 451, f"expected 451, got {e.smtp_code}: {e.smtp_error}"
            assert b"retry in" in e.smtp_error, e.smtp_error
        s.quit()

        assert con.execute(
            "SELECT count(*) FROM quackmail_outbound WHERE rcpt = 'three@example.com'"
        ).fetchone()[0] == 0, "a refused message must not be queued"

        # ---- the unit is the recipient, not the message --------------------
        con.execute("DELETE FROM quackmail_send_log")
        assert con.execute("SELECT ok FROM qm_ratelimit_set('alice', 5, 60, 100)").fetchone()[0]

        s = connect()
        rcpts = [f"r{i}@example.com" for i in range(3)]
        s.sendmail("alice@quackmail.test", rcpts, message(", ".join(rcpts), "Three at once"))
        s.quit()

        used = con.execute("SELECT burst_used FROM qm_rate_status('alice')").fetchone()[0]
        assert used == 3, f"one message to 3 recipients should cost 3, got {used}"
    finally:
        con.execute("CALL qm_smtp_submission_stop()").fetchall()

    print("PASS: outbound mail is DKIM-signed and verifies; rate limits hold per recipient")


if __name__ == "__main__":
    main()
