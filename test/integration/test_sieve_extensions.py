#!/usr/bin/env python3
"""End-to-end test for the Sieve extensions past RFC 5228's core.

test/sql/sieve.test pins what the *evaluator* decides — which actions a script
produces for a message — with no server anywhere near it. That is the right
place for the rules, and it cannot see any of this:

  * imap4flags has to reach `citadel_msg_flags`, the rows IMAP's STORE writes,
    or a rule that files something as already-read is a no-op with a passing
    unit test behind it.
  * vacation's whole point is the *second* message. The evaluator emits the
    action every time; what makes it an auto-reply rather than a reply storm is
    a table lookup, and a table lookup needs two deliveries to be worth anything.
  * variables have to survive being stored and reloaded through
    `quackmail_sieve_scripts` and produce a real room.

Requires: pip install duckdb==1.5.4. Run after `make`.
"""
import email
import os
import smtplib
import time
from email.mime.text import MIMEText

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
SMTP_PORT = 3531


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def install(con, user, script):
    con.execute("DELETE FROM quackmail_sieve_scripts WHERE username = ?", [user])
    con.execute(
        "INSERT INTO quackmail_sieve_scripts (username, name, active, script) "
        "VALUES (?, 'filters', true, ?)",
        [user, script],
    )
    assert con.execute("SELECT qm_sieve_valid(?)", [script]).fetchone()[0], \
        f"the test's own script does not parse:\n{script}"


def deliver(sender, rcpt, subject, extra_headers=()):
    """One message in one SMTP transaction, with the listener already up."""
    m = MIMEText("body\n")
    m["Subject"] = subject
    m["From"] = sender
    m["To"] = rcpt
    for name, value in extra_headers:
        m[name] = value
    s = smtplib.SMTP(HOST, SMTP_PORT, timeout=10)
    try:
        s.sendmail(sender, [rcpt], m.as_string())
    finally:
        s.quit()


def queued(con):
    """Everything on the outbound relay queue, newest last."""
    rows = con.execute(
        "SELECT from_addr, rcpt, raw FROM quackmail_outbound ORDER BY id"
    ).fetchall()
    out = []
    for from_addr, rcpt, raw in rows:
        text = bytes(raw).decode("utf-8", "replace")
        out.append((from_addr, rcpt, email.message_from_string(text)))
    return out


def folder_of(con, subject):
    row = con.execute(
        "SELECT r.display_name FROM citadel_messages m "
        "JOIN citadel_room_msgs rm ON rm.msgnum = m.msgnum "
        "JOIN citadel_rooms r ON r.room_num = rm.room_num "
        "WHERE m.subject = ? AND r.mailbox_owner > 0",
        [subject],
    ).fetchone()
    return row[0] if row else None


def flags_of(con, subject, user):
    return sorted(
        f[0]
        for f in con.execute(
            "SELECT f.flag FROM citadel_msg_flags f "
            "JOIN citadel_messages m ON m.msgnum = f.msgnum "
            "WHERE m.subject = ? AND f.username = ?",
            [subject, user],
        ).fetchall()
    )


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    con.execute(f"LOAD '{ext('quackmail_smtp_in')}'")
    assert con.execute("SELECT ok FROM qm_user_add('alice', 'secret')").fetchone()[0]

    note = con.execute(f"SELECT note FROM qm_smtp_in_start('{HOST}', {SMTP_PORT})").fetchone()[0]
    assert note == "started", f"smtp_in did not start: {note}"
    time.sleep(0.3)

    try:
        # ---- imap4flags reaches the rows IMAP reads -----------------------
        install(con, "alice", r"""require ["imap4flags", "fileinto"];
if header :contains "Subject" "receipt" {
    fileinto :flags ["\\Seen", "$Receipt"] "Receipts";
}
""")
        deliver("shop@example.invalid", "alice@quackmail.test", "your receipt")
        assert folder_of(con, "your receipt") == "Receipts", \
            f"fileinto did not route: {folder_of(con, 'your receipt')}"
        assert flags_of(con, "your receipt", "alice") == ["$Receipt", "\\Seen"], \
            f"flags did not reach citadel_msg_flags: {flags_of(con, 'your receipt', 'alice')}"

        # A message the rule did not match carries no flags at all — the
        # internal set is per-message, not per-script.
        deliver("friend@example.invalid", "alice@quackmail.test", "lunch")
        assert flags_of(con, "lunch", "alice") == [], \
            f"an unmatched message picked up flags: {flags_of(con, 'lunch', 'alice')}"

        # ---- variables name a folder from the message ---------------------
        install(con, "alice", r"""require ["variables", "fileinto"];
if header :matches "Subject" "[*] *" {
    fileinto "Lists/${1}";
}
""")
        deliver("list@example.invalid", "alice@quackmail.test", "[duckdb] a question")
        assert folder_of(con, "[duckdb] a question") == "Lists/duckdb", \
            f"a captured ${{1}} did not become a folder: {folder_of(con, '[duckdb] a question')}"

        # ---- vacation ------------------------------------------------------
        con.execute("DELETE FROM quackmail_outbound")
        install(con, "alice", """require ["vacation"];
vacation :days 7 :subject "Away" "I am on holiday until Monday.";
""")

        deliver("colleague@example.invalid", "alice@quackmail.test", "the report")
        replies = queued(con)
        assert len(replies) == 1, f"expected exactly one auto-reply, got {len(replies)}"
        from_addr, rcpt, msg = replies[0]

        # RFC 5230 §4.5: an empty envelope sender, so a bounce of the reply
        # does not land on the person who is away.
        assert from_addr == "", f"the auto-reply had envelope sender {from_addr!r}"
        assert rcpt == "colleague@example.invalid", rcpt
        # §4.6: this is the header that stops two servers both on holiday from
        # writing to each other forever. The other side's own suppression reads
        # it and says nothing back.
        assert msg["Auto-Submitted"] == "auto-replied", \
            f"Auto-Submitted was {msg['Auto-Submitted']!r}"
        assert msg["Subject"] == "Away", f"subject was {msg['Subject']!r}"
        assert msg["To"] == "colleague@example.invalid", msg["To"]
        assert "alice@" in (msg["From"] or ""), f"From was {msg['From']!r}"
        assert "holiday" in msg.get_payload(), msg.get_payload()

        # The message itself was still delivered: an auto-reply is not a
        # delivery decision, so the implicit keep stands.
        assert folder_of(con, "the report") == "Mail", \
            f"vacation swallowed the message: {folder_of(con, 'the report')}"

        # **The second message is the whole point.** The evaluator emits the
        # action again; the window is what makes this an auto-reply rather than
        # a reply to every message they ever send.
        deliver("colleague@example.invalid", "alice@quackmail.test", "the other report")
        assert len(queued(con)) == 1, \
            f"a second message inside the window was answered again: {len(queued(con))}"

        # A different correspondent has not been told yet, so they are.
        deliver("someone-else@example.invalid", "alice@quackmail.test", "hello")
        assert len(queued(con)) == 2, \
            f"a new correspondent was not answered: {len(queued(con))}"

        # Rewording the message starts a fresh window: the default handle is
        # derived from the text, so an edit is a new announcement (§4.3).
        install(con, "alice", """require ["vacation"];
vacation :days 7 :subject "Away" "Back on Wednesday now, plans changed.";
""")
        deliver("colleague@example.invalid", "alice@quackmail.test", "third try")
        assert len(queued(con)) == 3, \
            f"a reworded message did not start a new window: {len(queued(con))}"

        # ...but an explicit :handle pins the window across a reword, which is
        # how somebody fixes a typo without mailing everybody a second time.
        con.execute("DELETE FROM quackmail_outbound")
        con.execute("DELETE FROM quackmail_vacation_sent")
        install(con, "alice", """require ["vacation"];
vacation :days 7 :handle "holiday" "First wording.";
""")
        deliver("pinned@example.invalid", "alice@quackmail.test", "one")
        assert len(queued(con)) == 1, "the first message under a handle was not answered"
        install(con, "alice", """require ["vacation"];
vacation :days 7 :handle "holiday" "Second wording, same announcement.";
""")
        deliver("pinned@example.invalid", "alice@quackmail.test", "two")
        assert len(queued(con)) == 1, \
            f"an explicit :handle did not hold the window: {len(queued(con))}"

        # A mailing list gets no answer, ever. This is the rule that is
        # invisible until it has gone to four hundred people.
        con.execute("DELETE FROM quackmail_outbound")
        con.execute("DELETE FROM quackmail_vacation_sent")
        install(con, "alice", """require ["vacation"];
vacation :days 7 "Away";
""")
        deliver("list@example.invalid", "alice@quackmail.test", "digest",
                extra_headers=[("List-Id", "<l.example.invalid>")])
        assert queued(con) == [], f"a mailing list was auto-replied to: {queued(con)}"

        # ...and neither does something that was itself auto-generated.
        deliver("robot@example.invalid", "alice@quackmail.test", "notice",
                extra_headers=[("Auto-Submitted", "auto-generated")])
        assert queued(con) == [], f"an auto-submitted message was answered: {queued(con)}"

        # The site can switch auto-replies off outright: they are a backscatter
        # vector, and an admin needs a way to stop one without editing a user's
        # script.
        con.execute("DELETE FROM quackmail_vacation_sent")
        con.execute("SELECT ok FROM qm_config_set('qm_sieve_vacation', '0')").fetchall()
        deliver("colleague@example.invalid", "alice@quackmail.test", "with it off")
        assert queued(con) == [], f"qm_sieve_vacation=0 still replied: {queued(con)}"
        con.execute("SELECT ok FROM qm_config_set('qm_sieve_vacation', '1')").fetchall()

    finally:
        con.execute("CALL qm_smtp_in_stop()").fetchall()

    print("PASS: Sieve imap4flags reach IMAP's flags, variables name folders, "
          "and vacation answers a correspondent once")


if __name__ == "__main__":
    main()
