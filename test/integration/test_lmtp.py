#!/usr/bin/env python3
"""End-to-end test for the LMTP local-injection listener (RFC 2033).

LMTP differs from the MX in three ways, all checked here:
  * it greets with LHLO and refuses EHLO outright,
  * after DATA it emits one reply per recipient rather than a single reply,
  * it applies no sender authentication and no spam filtering — a message the
    MX would reject on DMARC is accepted here.

It still applies addressing: hosted domains, alias expansion and the
recipient's Sieve filter all take effect, which is the whole reason a local
process would inject through LMTP rather than writing to the store directly.

Requires: pip install duckdb==1.5.4. Run after `make`.
"""
import os
import socket
import time

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
LMTP_PORT = 3033


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class Client:
    """A minimal LMTP client: smtplib cannot speak LHLO or read N replies."""

    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.f = self.sock.makefile("rb")
        self.read_reply()

    def read_reply(self):
        """Read one reply, following any 'code-text' continuation lines."""
        lines = []
        while True:
            line = self.f.readline()
            if not line:
                raise AssertionError("connection closed while reading a reply")
            lines.append(line.decode().rstrip("\r\n"))
            if len(lines[-1]) < 4 or lines[-1][3] != "-":
                break
        return lines

    def cmd(self, text):
        self.sock.sendall((text + "\r\n").encode())
        return self.read_reply()

    def data(self, body):
        reply = self.cmd("DATA")
        assert reply[0].startswith("354"), f"DATA not accepted: {reply}"
        payload = body.replace("\n", "\r\n")
        if not payload.endswith("\r\n"):
            payload += "\r\n"
        self.sock.sendall(payload.encode() + b".\r\n")
        return reply

    def read_n(self, n):
        """After DATA, LMTP owes exactly one reply per accepted recipient."""
        return [self.read_reply()[0] for _ in range(n)]

    def close(self):
        try:
            self.cmd("QUIT")
        except Exception:
            pass
        self.sock.close()


def message(to, sender="stranger@nowhere.invalid", subject="LMTP injection"):
    return (
        f"From: {sender}\n"
        f"To: {to}\n"
        f"Subject: {subject}\n"
        f"Message-ID: <lmtp-{subject.replace(' ', '-')}@nowhere.invalid>\n"
        f"Date: Tue, 21 Jul 2026 12:00:00 +0000\n"
        f"\n"
        f"Injected locally.\n"
    )


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    con.execute(f"LOAD '{ext('quackmail_smtp_in')}'")

    for user in ("alice", "bob"):
        assert con.execute(f"SELECT ok FROM qm_user_add('{user}', 'pw')").fetchone()[0]

    # A hosted domain and an alias, to prove routing still applies.
    assert con.execute("SELECT ok FROM qm_domain_add('local.test', 'local')").fetchone()[0]
    assert con.execute("SELECT ok FROM qm_alias_add('team@local.test', 'alice')").fetchone()[0]
    assert con.execute("SELECT ok FROM qm_alias_add('team@local.test', 'bob')").fetchone()[0]

    note = con.execute(f"SELECT note FROM qm_lmtp_start('{HOST}', {LMTP_PORT})").fetchone()[0]
    assert note == "started", f"LMTP did not start: {note}"
    time.sleep(0.3)

    try:
        c = Client(HOST, LMTP_PORT)

        # EHLO is not an LMTP verb; the server must say so rather than proceed.
        reply = c.cmd("EHLO tester")
        assert reply[0].startswith("500"), f"EHLO should be refused on LMTP, got {reply}"

        reply = c.cmd("LHLO tester")
        assert reply[0].startswith("250"), f"LHLO rejected: {reply}"

        # Two recipients: one deliverable, one unknown.
        assert c.cmd("MAIL FROM:<stranger@nowhere.invalid>")[0].startswith("250")
        assert c.cmd("RCPT TO:<alice@quackmail.test>")[0].startswith("250")
        unknown = c.cmd("RCPT TO:<nobody@quackmail.test>")
        assert unknown[0].startswith("550"), f"unknown user should be 550, got {unknown}"

        # One accepted recipient -> exactly one reply after the dot.
        c.data(message("alice@quackmail.test"))
        replies = c.read_n(1)
        assert replies[0].startswith("250"), f"delivery reply: {replies}"
        assert "alice@quackmail.test" in replies[0], f"reply should name the recipient: {replies}"

        # Two accepted recipients -> two replies, in order.
        assert c.cmd("MAIL FROM:<stranger@nowhere.invalid>")[0].startswith("250")
        assert c.cmd("RCPT TO:<alice@quackmail.test>")[0].startswith("250")
        assert c.cmd("RCPT TO:<bob@quackmail.test>")[0].startswith("250")
        c.data(message("alice@quackmail.test, bob@quackmail.test", subject="Two recipients"))
        replies = c.read_n(2)
        assert all(r.startswith("250") for r in replies), f"both should succeed: {replies}"
        assert "alice@quackmail.test" in replies[0] and "bob@quackmail.test" in replies[1], replies

        # The alias expands to two users, but it is one envelope recipient, so
        # LMTP still owes exactly one reply.
        assert c.cmd("MAIL FROM:<stranger@nowhere.invalid>")[0].startswith("250")
        assert c.cmd("RCPT TO:<team@local.test>")[0].startswith("250")
        c.data(message("team@local.test", subject="Via alias"))
        replies = c.read_n(1)
        assert replies[0].startswith("250") and "team@local.test" in replies[0], replies

        c.close()
    finally:
        con.execute("CALL qm_lmtp_stop()").fetchall()

    # No inbound authentication ran: LMTP records no SPF/DMARC verdict.
    verdicts = con.execute(
        "SELECT DISTINCT coalesce(spf, ''), coalesce(dmarc, '') FROM quackmail_inbound_log"
    ).fetchall()
    assert verdicts == [("", "")], f"LMTP must not run sender authentication, got {verdicts}"

    # Three messages stored, not four: the two-recipient delivery is stored once
    # and pointed into both Mail rooms, the same reference-counted model the MX
    # path uses. Per-recipient LMTP *replies* do not mean per-recipient copies.
    stored = con.execute("SELECT count(*) FROM citadel_messages").fetchone()[0]
    assert stored == 3, f"expected 3 stored messages, got {stored}"

    shared = con.execute(
        "SELECT count(*) FROM citadel_room_msgs rm "
        "JOIN citadel_messages m ON m.msgnum = rm.msgnum "
        "WHERE m.subject = 'Two recipients'"
    ).fetchone()[0]
    assert shared == 2, f"one stored message should point into 2 rooms, got {shared}"

    alias_rooms = con.execute(
        "SELECT count(DISTINCT r.mailbox_owner) FROM citadel_room_msgs rm "
        "JOIN citadel_rooms r ON r.room_num = rm.room_num "
        "JOIN citadel_messages m ON m.msgnum = rm.msgnum "
        "WHERE m.subject = 'Via alias' AND r.mailbox_owner > 0"
    ).fetchone()[0]
    assert alias_rooms == 2, f"alias should reach both users, got {alias_rooms}"

    print("PASS: LMTP refuses EHLO, replies per recipient, expands aliases, skips spam checks")


if __name__ == "__main__":
    main()
