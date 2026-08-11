#!/usr/bin/env python3
"""End-to-end test for iTIP/iMIP scheduling: invitations by mail, replies applied.

test/sql/itip.test pins the message *shapes* — a CANCEL bumps SEQUENCE, a REPLY
carries only the replying attendee, a stale REPLY changes nothing. Those are
pure functions and belong there. This file asserts the two things a pure
function cannot: that a PUT to CalDAV actually puts an invitation in the post,
and that an invitation or reply arriving over SMTP actually lands on somebody's
calendar.

The assertions worth naming, because they are the ones that would regress
silently:

  * **Only the organizer sends.** An attendee saving their own copy of a
    shared event must not mail everybody else. A server that got this wrong
    would turn every calendar into a mailing list, and nothing in the DAV
    response would say so.
  * The invitation is charged against the sender's quota. `submission::Send`
    is quota-agnostic by contract, so a scheduling path that skipped
    `policy::CheckRate` would be a third door onto the mail path with the
    limit switched off — exactly what TODO.md warns about for JMAP.
  * A DELETE cancels rather than going quiet. The attendee's copy has to be
    told, or the meeting stays on their calendar forever.
  * An inbound REPLY updates the PARTSTAT on the organizer's stored copy, and
    the organizer reads that change back over CalDAV — the same object, not a
    second one.

Requires: pip install duckdb==1.5.4
Run after `make release`.
"""
import base64
import os
import re
import smtplib
import sys
import urllib.error
import urllib.request

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 18103
SMTP_PORT = 3527

# A .test domain, so nothing here consults real DNS: example.com's org domain
# publishes DMARC p=reject and the MX would refuse the reply on the way in.
FQDN = "sched.test"
ORGANIZER = "ada"
ORGANIZER_PW = "secret"
ATTENDEE = "bob"
ATTENDEE_PW = "secret2"

BASE = f"http://{HOST}:{PORT}"

EVENT_UID = "meeting-9001@sched.test"


def event(seq=0, summary="Design review", extra=""):
    return (
        "BEGIN:VCALENDAR\r\n"
        "VERSION:2.0\r\n"
        "PRODID:-//Test//EN\r\n"
        "BEGIN:VEVENT\r\n"
        f"UID:{EVENT_UID}\r\n"
        "DTSTAMP:20260701T120000Z\r\n"
        "DTSTART:20260815T140000Z\r\n"
        "DTEND:20260815T150000Z\r\n"
        f"SEQUENCE:{seq}\r\n"
        f"SUMMARY:{summary}\r\n"
        f"ORGANIZER:mailto:{ORGANIZER}@{FQDN}\r\n"
        f"ATTENDEE;PARTSTAT=NEEDS-ACTION;RSVP=TRUE:mailto:{ATTENDEE}@{FQDN}\r\n"
        f"{extra}"
        "END:VEVENT\r\n"
        "END:VCALENDAR\r\n"
    )


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def dav_name(euid):
    out = []
    for i, ch in enumerate(euid.encode()):
        c = chr(ch)
        safe = c.isascii() and (c.isalnum() or c in "._-")
        if safe and not (c == "." and i == 0):
            out.append(c)
        else:
            out.append("~%02X" % ch)
    return "".join(out)


class Dav:
    def __init__(self, user, password):
        self.auth = base64.b64encode(f"{user}:{password}".encode()).decode()
        self.op = urllib.request.build_opener()

    def go(self, method, path, body=None, headers=None):
        req = urllib.request.Request(BASE + path, data=body, method=method)
        req.add_header("Authorization", "Basic " + self.auth)
        for k, v in (headers or {}).items():
            req.add_header(k, v)
        try:
            r = self.op.open(req, timeout=20)
            return r.status, dict(r.headers), r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), e.read().decode("utf-8", "replace")


def mailbox(con, user):
    """Every message body in a user's Mail room, newest last."""
    rows = con.execute(
        """
        SELECT m.raw FROM citadel_messages m
        JOIN citadel_room_msgs rm ON rm.msgnum = m.msgnum
        JOIN citadel_rooms r ON r.room_num = rm.room_num
        JOIN citadel_users u ON u.usernum = r.mailbox_owner
        WHERE u.username = ? AND r.display_name = 'Mail'
        ORDER BY m.msgnum
        """,
        [user],
    ).fetchall()
    # raw is a BLOB, and every assertion below is about text.
    return [r[0].decode("utf-8", "replace") if isinstance(r[0], (bytes, bytearray))
            else r[0] for r in rows]


def calendar_of(con, user):
    """The stored text/calendar bodies in a user's Calendar room."""
    rows = con.execute(
        """
        SELECT m.raw FROM citadel_messages m
        JOIN citadel_room_msgs rm ON rm.msgnum = m.msgnum
        JOIN citadel_rooms r ON r.room_num = rm.room_num
        JOIN citadel_users u ON u.usernum = r.mailbox_owner
        WHERE u.username = ? AND r.display_name = 'Calendar'
        ORDER BY m.msgnum
        """,
        [user],
    ).fetchall()
    # raw is a BLOB, and every assertion below is about text.
    return [r[0].decode("utf-8", "replace") if isinstance(r[0], (bytes, bytearray))
            else r[0] for r in rows]


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_http", "quackmail_smtp_in"):
        con.execute(f"LOAD '{ext(name)}'")
    con.execute("SELECT count(*) FROM qm_status()").fetchall()

    con.execute(f"CALL qm_user_add('{ORGANIZER}', '{ORGANIZER_PW}')")
    con.execute(f"CALL qm_user_add('{ATTENDEE}', '{ATTENDEE_PW}')")
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    con.execute(f"CALL qm_config_set('c_fqdn', '{FQDN}')")
    con.execute(f"SELECT ok FROM qm_domain_add('{FQDN}', 'local')").fetchall()
    con.execute(f"CALL qm_http_start('{HOST}', {PORT})")
    con.execute(f"SELECT note FROM qm_smtp_in_start('{HOST}', {SMTP_PORT})").fetchall()

    try:
        ada = Dav(ORGANIZER, ORGANIZER_PW)
        bob = Dav(ATTENDEE, ATTENDEE_PW)

        # The personal rooms are created on first use, so ask for the home set
        # before looking any of them up — the same thing a client does when it
        # first connects.
        for who, cl in ((ORGANIZER, ada), (ATTENDEE, bob)):
            status, _, _ = cl.go(
                "PROPFIND", f"/dav/calendars/{who}/",
                b'<?xml version="1.0"?><D:propfind xmlns:D="DAV:">'
                b"<D:prop><D:resourcetype/></D:prop></D:propfind>",
                {"Depth": "1", "Content-Type": "application/xml"})
            assert status == 207, f"{who}'s calendar home returned {status}"

        # The organizer's Calendar room, and the resource inside it.
        room = con.execute(
            """SELECT r.room_num FROM citadel_rooms r JOIN citadel_users u
               ON u.usernum = r.mailbox_owner
               WHERE u.username = ? AND r.display_name = 'Calendar'""",
            [ORGANIZER],
        ).fetchone()
        assert room, "the organizer has no Calendar room"
        cal = f"/dav/calendars/{ORGANIZER}/{room[0]}/"
        href = cal + dav_name(EVENT_UID) + ".ics"

        # ---- PUT sends an invitation ----------------------------------------
        status, _, _ = ada.go("PUT", href, event().encode(),
                              {"Content-Type": "text/calendar"})
        assert status == 201, f"creating the event returned {status}"

        inbox = mailbox(con, ATTENDEE)
        assert len(inbox) == 1, f"the attendee got {len(inbox)} messages, expected an invitation"
        invite = inbox[0]
        assert "method=REQUEST" in invite, \
            "no method= on the Content-Type, so clients treat it as a plain attachment"
        assert "METHOD:REQUEST" in invite, invite[:400]
        assert EVENT_UID in invite
        assert "Subject: Invitation: Design review" in invite, \
            f"the subject does not say what arrived: {invite[:400]}"
        # A client that cannot read the calendar part still has to see something.
        assert "You have been invited" in invite, invite[:400]

        # The quota was charged. submission::Send does not do it, so a scheduling
        # path that forgot would be a door onto the mail path with the limit off.
        sent = con.execute(
            "SELECT count(*) FROM quackmail_send_log WHERE username = ?", [ORGANIZER]
        ).fetchone()[0]
        assert sent >= 1, "the invitation was not charged against the organizer's send quota"

        # ---- an attendee saving their own copy sends nothing ------------------
        #
        # The single most important negative here. bob is on this event but does
        # not organize it; if storing his copy mailed everyone, every shared
        # calendar would become a mailing list.
        bob_room = con.execute(
            """SELECT r.room_num FROM citadel_rooms r JOIN citadel_users u
               ON u.usernum = r.mailbox_owner
               WHERE u.username = ? AND r.display_name = 'Calendar'""",
            [ATTENDEE],
        ).fetchone()[0]
        before = len(mailbox(con, ORGANIZER))
        status, _, _ = bob.go(
            "PUT", f"/dav/calendars/{ATTENDEE}/{bob_room}/" + dav_name(EVENT_UID) + ".ics",
            event(summary="Design review").encode(), {"Content-Type": "text/calendar"})
        assert status in (201, 204), f"the attendee could not store their own copy ({status})"
        assert len(mailbox(con, ORGANIZER)) == before, \
            "an attendee storing their own copy mailed the organizer — only the organizer sends"

        # ---- an inbound REPLY updates the organizer's copy --------------------
        reply_ics = con.execute(
            "SELECT qm_itip_reply(?, ?, 'ACCEPTED')", [event(), f"{ATTENDEE}@{FQDN}"]
        ).fetchone()[0]
        assert reply_ics and "METHOD:REPLY" in reply_ics

        msg = (
            f"From: {ATTENDEE}@{FQDN}\r\n"
            f"To: {ORGANIZER}@{FQDN}\r\n"
            "Subject: Accepted: Design review\r\n"
            "MIME-Version: 1.0\r\n"
            'Content-Type: text/calendar; method=REPLY; charset=utf-8\r\n'
            "\r\n" + reply_ics
        )
        s = smtplib.SMTP(HOST, SMTP_PORT, timeout=10)
        s.sendmail(f"{ATTENDEE}@{FQDN}", [f"{ORGANIZER}@{FQDN}"], msg)
        s.quit()

        stored = calendar_of(con, ORGANIZER)
        assert len(stored) == 1, f"the organizer's calendar holds {len(stored)} objects, expected 1"
        assert "PARTSTAT=ACCEPTED" in stored[0], \
            f"the reply did not update the organizer's copy: {stored[0]}"
        # Updated in place, not filed as a second event, and the organizer reads
        # it back over the protocol they wrote it with.
        status, _, body = ada.go("GET", href)
        assert status == 200 and "PARTSTAT=ACCEPTED" in body, \
            f"CalDAV does not show the answer ({status}): {body[:400]}"

        # ---- an inbound REQUEST lands on the attendee's calendar --------------
        other_uid = "invite-from-outside@outside.invalid"
        outside = (
            "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nMETHOD:REQUEST\r\n"
            "BEGIN:VEVENT\r\n"
            f"UID:{other_uid}\r\n"
            "DTSTAMP:20260701T120000Z\r\nDTSTART:20260901T090000Z\r\n"
            "DTEND:20260901T100000Z\r\nSUMMARY:From another server\r\n"
            "ORGANIZER:mailto:someone@outside.invalid\r\n"
            f"ATTENDEE;RSVP=TRUE:mailto:{ATTENDEE}@{FQDN}\r\n"
            "END:VEVENT\r\nEND:VCALENDAR\r\n"
        )
        msg = (
            "From: someone@outside.invalid\r\n"
            f"To: {ATTENDEE}@{FQDN}\r\n"
            "Subject: Invitation: From another server\r\n"
            "MIME-Version: 1.0\r\n"
            'Content-Type: multipart/alternative; boundary="bd"\r\n'
            "\r\n"
            "--bd\r\nContent-Type: text/plain\r\n\r\nYou are invited.\r\n"
            "--bd\r\nContent-Type: text/calendar; method=REQUEST\r\n\r\n" + outside +
            "--bd--\r\n"
        )
        s = smtplib.SMTP(HOST, SMTP_PORT, timeout=10)
        s.sendmail("someone@outside.invalid", [f"{ATTENDEE}@{FQDN}"], msg)
        s.quit()

        bob_cal = calendar_of(con, ATTENDEE)
        assert any(other_uid in c for c in bob_cal), \
            f"an invitation by mail never reached the calendar: {[c[:80] for c in bob_cal]}"
        # And it is still in the mailbox: the calendar effect is in addition to
        # delivery, not instead of it.
        assert any(other_uid in m for m in mailbox(con, ATTENDEE)), \
            "the invitation was filed on the calendar and swallowed from the mailbox"

        # ---- DELETE cancels --------------------------------------------------
        before = len(mailbox(con, ATTENDEE))
        status, _, _ = ada.go("DELETE", href)
        assert status == 204, f"deleting the event returned {status}"
        after = mailbox(con, ATTENDEE)
        assert len(after) == before + 1, \
            "deleting the event told nobody, so it stays on the attendee's calendar forever"
        cancel = after[-1]
        assert "METHOD:CANCEL" in cancel and "STATUS:CANCELLED" in cancel, cancel[:400]
        assert "Subject: Cancelled: Design review" in cancel, cancel[:400]
        # The bump is what stops a client treating the cancellation as a stale
        # duplicate of the invitation it already has.
        assert re.search(r"SEQUENCE:1", cancel), f"the cancellation did not bump SEQUENCE: {cancel}"

    finally:
        con.execute("CALL qm_smtp_in_stop()")
        con.execute("CALL qm_http_stop()")

    print("PASS: iTIP/iMIP (invitation on PUT, only the organizer sends, inbound REPLY "
          "and REQUEST applied, DELETE cancels)")


if __name__ == "__main__":
    main()
