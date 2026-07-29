#!/usr/bin/env python3
"""End-to-end test for the mailing list manager.

The sqllogictest covers configuration and the pure header rewriting. This covers
*enforcement* — the part that needs a real SMTP conversation and a real spooler
pass, and which sqllogictest cannot reach because the umbrella's table functions
open their own connection.

What it establishes:
  * a message mailed to the list address is archived in the room and, after one
    spooler pass, queued once per subscriber with the RFC 2369/2919 List-*
    headers and a list bounce envelope,
  * the watermark holds: a second spooler pass sends nothing again,
  * a post made *in the room* by some other route is distributed too — which is
    the whole reason distribution is a spooler over the room rather than a hook
    in the SMTP handler,
  * subscribe by mail mints a pending row and mails a confirmation, and only the
    token in that mail activates it; unsubscribe likewise,
  * a moderated list holds a post instead of storing it, and approval posts it
    into the room for the next spooler pass to distribute,
  * digest mode batches.

Requires: pip install duckdb==1.5.4. Run after `make`.
"""
import os
import re
import smtplib
import time
from email.mime.text import MIMEText

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
SMTP_PORT = 3625


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_smtp_in", "quackmail_spool"):
        con.execute(f"LOAD '{ext(name)}'")

    assert con.execute("SELECT ok FROM qm_user_add('alice', 'secret')").fetchone()[0]

    # Subscribers here have example.com addresses, and example.com really does
    # publish p=reject — so with enforcement on, half of this test would be
    # measuring live DNS rather than the list manager. test_smtp_policy.py is
    # where DMARC belongs.
    assert con.execute("SELECT ok FROM qm_config_set('qm_dmarc_enforce', '0')").fetchone()[0]

    # ---- helpers ---------------------------------------------------------

    def queued(rcpt=None):
        """Rows on the outbound queue, as (rcpt, from_addr, raw)."""
        sql = "SELECT rcpt, from_addr, CAST(raw AS VARCHAR) FROM quackmail_outbound"
        if rcpt:
            sql += f" WHERE rcpt = '{rcpt}'"
        return con.execute(sql + " ORDER BY id").fetchall()

    def clear_queue():
        con.execute("DELETE FROM quackmail_outbound")

    def spool(room=None):
        """One spooler pass; returns (distributed, recipients, digests)."""
        call = "qm_listserv_run()" if room is None else f"qm_listserv_run(room_num => {room})"
        row = con.execute(f"SELECT distributed, recipients, digests, note FROM {call}").fetchone()
        assert not row[3].startswith("error"), row[3]
        return row[0], row[1], row[2]

    def send(to, subject, body="Body.\n", mail_from="poster@example.invalid", expect_ok=True):
        msg = MIMEText(body)
        msg["Subject"] = subject
        msg["From"] = mail_from
        msg["To"] = to
        s = smtplib.SMTP(HOST, SMTP_PORT, timeout=10)
        try:
            s.sendmail(mail_from, [to], msg.as_string())
            assert expect_ok, f"{to} should have been refused"
        except smtplib.SMTPRecipientsRefused as e:
            assert not expect_ok, f"{to} was refused: {e.recipients}"
        finally:
            s.quit()

    def room_of(subject):
        rows = con.execute(
            "SELECT r.display_name FROM citadel_messages m "
            "JOIN citadel_room_msgs rm ON rm.msgnum = m.msgnum "
            "JOIN citadel_rooms r ON r.room_num = rm.room_num "
            f"WHERE m.subject = '{subject}'"
        ).fetchall()
        return sorted(x[0] for x in rows)

    # ---- set the lists up ------------------------------------------------

    assert con.execute("SELECT ok FROM cit_room_add('Announce')").fetchone()[0]
    ok, note = con.execute(
        "SELECT ok, note FROM qm_list_create('Announce', 'announce')"
    ).fetchone()
    assert ok, note
    announce_room = con.execute(
        "SELECT room_num FROM qm_lists() WHERE address = 'announce@quackmail.test'"
    ).fetchone()[0]

    for who in ("alice@example.com", "bob@example.com"):
        ok, note = con.execute(
            f"SELECT ok, note FROM qm_list_sub_add('Announce', '{who}', 'post')"
        ).fetchone()
        assert ok, note
    # Anyone may post, so the SMTP path does not hold ordinary mail.
    assert con.execute(
        "SELECT ok FROM qm_list_set('Announce', 'post_policy', 'anyone')"
    ).fetchone()[0]

    note = con.execute(f"SELECT note FROM qm_smtp_in_start('{HOST}', {SMTP_PORT})").fetchone()[0]
    assert note == "started", f"server did not start: {note}"
    time.sleep(0.3)

    try:
        # ---- 1. a post arrives by mail and is fanned out -----------------

        send("announce@quackmail.test", "First post")
        assert room_of("First post") == ["Announce"], room_of("First post")
        # Nothing is sent until the spooler runs: archiving and distributing are
        # deliberately separate steps.
        assert queued() == [], "nothing should be queued before the spooler runs"

        distributed, recipients, _ = spool()
        assert (distributed, recipients) == (1, 2), (distributed, recipients)

        rows = queued()
        assert sorted(r[0] for r in rows) == ["alice@example.com", "bob@example.com"]
        for rcpt, from_addr, raw in rows:
            # The envelope sender is the list, so a bounce does not land on
            # whoever happened to post.
            assert from_addr == "announce-bounces@quackmail.test", from_addr
            assert "List-Id: " in raw and "<announce.quackmail.test>" in raw, raw[:400]
            assert "List-Post: <mailto:announce@quackmail.test>" in raw
            assert "List-Unsubscribe: <mailto:announce-unsubscribe@quackmail.test>" in raw
            assert "Precedence: list" in raw
            assert "First post" in raw

        # ---- 2. the watermark holds --------------------------------------

        clear_queue()
        distributed, recipients, _ = spool()
        assert (distributed, recipients) == (0, 0), "a second pass must not resend"

        # ---- 3. a post made in the room, not over SMTP -------------------
        #
        # This is the case the whole design exists for: the BBS, NNTP, a Citadel
        # client and webmail all write straight into the room, and every one of
        # them has to reach subscribers.

        # The message number has to come from the same sequence every other
        # front-end draws from — a hand-picked one above the sequence would push
        # the list's watermark past everything the SMTP path goes on to store.
        msgnum = con.execute("SELECT nextval('citadel_msg_seq')").fetchone()[0]
        con.execute(
            "INSERT INTO citadel_messages (msgnum, author, msgtime, subject, format_type, "
            f"origin_room, raw) VALUES ({msgnum}, 'Alice', 1753660800, 'Posted in the room', 0, "
            "'Announce', 'Straight into the room.')"
        )
        con.execute(f"INSERT INTO citadel_room_msgs VALUES ({announce_room}, {msgnum})")
        con.execute(
            f"UPDATE citadel_rooms SET highest_msg = {msgnum} WHERE room_num = {announce_room}"
        )

        distributed, recipients, _ = spool()
        assert (distributed, recipients) == (1, 2), (distributed, recipients)
        assert all("Posted in the room" in r[2] for r in queued())
        clear_queue()

        # ---- 4. subscribe by mail, with confirmation ---------------------

        send("announce-subscribe@quackmail.test", "subscribe me",
             mail_from="carol@example.com")

        state = con.execute(
            "SELECT state FROM citadel_list_subs WHERE address = 'carol@example.com'"
        ).fetchone()
        assert state is not None and state[0] == "pending", state

        # Exactly one thing was sent, to the address that was claimed and nowhere
        # else — that is what stops one person subscribing another.
        confirmations = queued("carol@example.com")
        assert len(confirmations) == 1, f"expected one confirmation, got {len(confirmations)}"
        assert len(queued()) == 1, "the confirmation must be the only mail sent"
        # A robot reply carries a null envelope sender, so bouncing it cannot
        # start a loop.
        assert confirmations[0][1] == "", confirmations[0][1]
        raw = confirmations[0][2]
        m = re.search(r"announce-confirm-([A-Za-z0-9_-]+)@quackmail\.test", raw)
        assert m, raw
        token = m.group(1)
        clear_queue()

        # She is not on the list yet, so a post distributed now reaches the two
        # confirmed subscribers and not her.
        send("announce@quackmail.test", "Not for carol yet")
        spool()
        assert queued("carol@example.com") == [], "an unconfirmed address must not receive posts"
        assert len(queued()) == 2, queued()
        clear_queue()

        # Mailing the confirmation address is what actually subscribes her.
        send(f"announce-confirm-{token}@quackmail.test", "yes",
             mail_from="carol@example.com")
        state = con.execute(
            "SELECT state FROM citadel_list_subs WHERE address = 'carol@example.com'"
        ).fetchone()[0]
        assert state == "active", state

        clear_queue()
        send("announce@quackmail.test", "Carol gets this one")
        spool()
        assert len(queued("carol@example.com")) == 1
        clear_queue()

        # A token cannot be replayed: it was cleared when it was spent.
        send(f"announce-confirm-{token}@quackmail.test", "again",
             mail_from="mallory@example.invalid")
        assert con.execute(
            "SELECT count(*) FROM citadel_list_subs WHERE address = 'mallory@example.invalid'"
        ).fetchone()[0] == 0
        clear_queue()

        # ---- 5. unsubscribe, also confirmed ------------------------------

        send("announce-unsubscribe@quackmail.test", "bye", mail_from="carol@example.com")
        state = con.execute(
            "SELECT state FROM citadel_list_subs WHERE address = 'carol@example.com'"
        ).fetchone()[0]
        assert state == "unsub_pending", state

        raw = con.execute(
            "SELECT CAST(raw AS VARCHAR) FROM quackmail_outbound WHERE rcpt = 'carol@example.com'"
        ).fetchone()[0]
        m = re.search(r"announce-confirm-([A-Za-z0-9_-]+)@quackmail\.test", raw)
        assert m, raw
        clear_queue()

        send(f"announce-confirm-{m.group(1)}@quackmail.test", "yes",
             mail_from="carol@example.com")
        assert con.execute(
            "SELECT count(*) FROM citadel_list_subs WHERE address = 'carol@example.com'"
        ).fetchone()[0] == 0

        # ---- 6. moderation ------------------------------------------------

        clear_queue()
        assert con.execute(
            "SELECT ok FROM qm_list_set('Announce', 'post_policy', 'moderated')"
        ).fetchone()[0]

        # The sender is not told it was held: the SMTP transaction succeeds.
        send("announce@quackmail.test", "Needs approval", mail_from="stranger@example.invalid")
        assert room_of("Needs approval") == [], "a moderated post must not reach the room"

        held = con.execute(
            "SELECT id, mail_from, subject FROM qm_list_held('Announce')"
        ).fetchall()
        assert len(held) == 1, held
        held_id, mail_from, subject = held[0]
        assert mail_from == "stranger@example.invalid" and subject == "Needs approval"

        ok, note = con.execute(f"SELECT ok, note FROM qm_list_approve({held_id})").fetchone()
        assert ok, note
        assert room_of("Needs approval") == ["Announce"], room_of("Needs approval")
        # Approval posts and stops; the spooler is still the only sender.
        assert queued() == [], "approval must not send anything itself"

        distributed, recipients, _ = spool()
        assert (distributed, recipients) == (1, 2), (distributed, recipients)
        clear_queue()

        # Rejecting leaves the room alone.
        send("announce@quackmail.test", "Never approved", mail_from="stranger@example.invalid")
        held_id = con.execute("SELECT id FROM qm_list_held('Announce')").fetchone()[0]
        assert con.execute(f"SELECT ok FROM qm_list_reject({held_id})").fetchone()[0]
        assert room_of("Never approved") == []
        assert con.execute("SELECT count(*) FROM qm_list_held('Announce')").fetchone()[0] == 0

        # A subscriber-only list holds a stranger's post but takes a member's.
        assert con.execute(
            "SELECT ok FROM qm_list_set('Announce', 'post_policy', 'subscribers')"
        ).fetchone()[0]
        send("announce@quackmail.test", "From a stranger", mail_from="nobody@example.invalid")
        assert room_of("From a stranger") == []
        send("announce@quackmail.test", "From a member", mail_from="alice@example.com")
        assert room_of("From a member") == ["Announce"]

        # ---- 7. digests ---------------------------------------------------

        # Drain Announce first, so the counts below are Weekly's alone.
        spool()
        clear_queue()

        assert con.execute("SELECT ok FROM cit_room_add('Weekly')").fetchone()[0]
        ok, note = con.execute(
            "SELECT ok, note FROM qm_list_create('Weekly', 'weekly')"
        ).fetchone()
        assert ok, note
        for key, value in (("mode", "digest"), ("post_policy", "anyone")):
            assert con.execute(
                f"SELECT ok FROM qm_list_set('Weekly', '{key}', '{value}')"
            ).fetchone()[0]
        assert con.execute(
            "SELECT ok FROM qm_list_sub_add('Weekly', 'digest@example.com', 'digest')"
        ).fetchone()[0]

        for n in (1, 2, 3):
            send("weekly@quackmail.test", f"Digest item {n}")
        # No immediate fan-out in digest mode, and one batch covering all three.
        distributed, recipients, digests = spool()
        assert (distributed, digests) == (0, 1), (distributed, recipients, digests)

        rows = queued("digest@example.com")
        assert len(rows) == 1, rows
        raw = rows[0][2]
        assert "Content-Type: multipart/digest" in raw, raw[:400]
        for n in (1, 2, 3):
            assert f"Digest item {n}" in raw
        assert "Messages in this digest:" in raw

        # The interval has not elapsed, so a second pass builds nothing.
        clear_queue()
        send("weekly@quackmail.test", "Digest item 4")
        _, _, digests = spool()
        assert digests == 0, "a digest must not go out before its interval elapses"

        # ---- 8. bounces are swallowed, not answered -----------------------

        clear_queue()
        send("announce-bounces@quackmail.test", "Undelivered", mail_from="")
        assert queued() == [], "answering a bounce is how mail loops start"

    finally:
        con.execute("CALL qm_smtp_in_stop()").fetchall()

    print("test_listserv: ok")


if __name__ == "__main__":
    main()
