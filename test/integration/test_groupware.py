#!/usr/bin/env python3
"""End-to-end test for the calendar, tasks, notes and blog room views.

Companion to test_contacts.py, which covers VIEW_ADDRESSBOOK. This drives the
other renderers over HTTP and then reads what they stored back over IMAP.

The assertions worth naming, because they are the ones that would regress
silently:

  * A recurring event appears on every day it recurs, and at the same *local*
    time either side of a daylight-saving change. Everything else about the
    calendar could be right while this is wrong, and nobody would notice until
    November.
  * Editing an event preserves its VALARM. The whole reason ical has both a tree
    and a flat Item is that saving from a form must not delete the reminder
    somebody set on their phone.
  * Every object is a readable MIME message over IMAP, with the content type the
    format actually calls for — text/calendar, text/vnote.

Requires: pip install duckdb==1.5.4
Run after `make` so the loadable extensions exist under build/release/extension.
"""
import datetime
import html
import http.cookiejar
import imaplib
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 18092
IMAP_PORT = 11432
BASE = f"http://{HOST}:{PORT}"

USER = "groupware"
PASSWORD = "secret"
# Chosen because its DST change is in the middle of the recurrence below.
ZONE = "America/New_York"


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


class Client:
    def __init__(self):
        self.jar = http.cookiejar.CookieJar()
        self.op = urllib.request.build_opener(
            NoRedirect(), urllib.request.HTTPCookieProcessor(self.jar))

    def get(self, path):
        return self._go(path, None)

    def post(self, path, fields):
        return self._go(path, urllib.parse.urlencode(fields).encode())

    def _go(self, path, body):
        try:
            r = self.op.open(BASE + path, body, timeout=20)
            return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")


def csrf(page):
    m = re.search(r'name="_csrf" value="([^"]+)"', page)
    assert m, "no CSRF token on that page"
    return m.group(1)


def room_of(home, label):
    m = re.search(r'href="(/bbs/room/(\d+))"[^>]*><span>' + label + r'</span>', home)
    assert m, f"the sidebar does not link a {label} room"
    return m.group(1)


def item_numbers(page, room_path):
    return re.findall(r'href="' + re.escape(room_path) + r'/item/(\d+)"', page)


def imap_body(im, mailbox, expect_type):
    typ, data = im.select(mailbox)
    assert typ == "OK", f"IMAP could not select {mailbox}: {data}"
    typ, nums = im.search(None, "ALL")
    ids = nums[0].split()
    assert ids, f"IMAP sees nothing in {mailbox}"
    typ, raw = im.fetch(ids[-1], "(RFC822)")
    body = raw[-1][1].decode("utf-8", "replace") if isinstance(raw[-1], tuple) else ""
    if not body:
        for part in raw:
            if isinstance(part, tuple):
                body = part[1].decode("utf-8", "replace")
                break
    assert expect_type in body, f"{mailbox} part is not {expect_type}: {body[:200]}"
    return len(ids), body


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_http", "quackmail_imap"):
        con.execute(f"LOAD '{ext(name)}'")
    con.execute("SELECT count(*) FROM qm_status()").fetchall()

    con.execute(f"CALL qm_user_add('{USER}', '{PASSWORD}')")
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    con.execute(f"CALL qm_http_start('{HOST}', {PORT})")
    con.execute(f"CALL qm_imap_start('{HOST}', {IMAP_PORT})")

    try:
        c = Client()
        _, page = c.get("/login")
        status, _ = c.post("/login", {"username": USER, "password": PASSWORD,
                                      "_csrf": csrf(page)})
        assert status == 303, f"sign-in returned {status}"

        # Pin the viewer's zone: every date the calendar renders and every TZID
        # it writes comes from this, so the DST assertion below depends on it.
        _, prefs = c.get("/prefs")
        status, _ = c.post("/prefs/settings", {"_csrf": csrf(prefs), "width": "80",
                                              "height": "24", "theme": "auto", "tz": ZONE})
        assert status == 303, f"saving the time zone returned {status}"
        _, prefs = c.get("/prefs")
        assert f'value="{ZONE}" selected' in prefs, "the time zone preference did not stick"

        _, home = c.get("/mail/")
        cal = room_of(home, "Calendar")
        tasks = room_of(home, "Tasks")
        notes = room_of(home, "Notes")

        # ---- calendar -------------------------------------------------------
        status, page = c.get(cal)
        assert status == 200, f"the calendar returned {status}"
        assert "calmonth" in page, "the calendar did not render a month grid"
        assert "New event" in page
        assert f"Times shown in {ZONE}" in page, "the calendar does not say which zone it used"

        status, form = c.get(cal + "/item/new")
        assert status == 200 and 'name="end_time"' in form, f"the event form returned {status}"

        # A weekly event starting before the November DST change, running past it.
        status, _ = c.post(cal + "/item/save", {
            "_csrf": csrf(form), "summary": "Weekly sync",
            "date": "2026-10-20", "time": "09:00",
            "end_date": "2026-10-20", "end_time": "10:00",
            "location": "Room 3", "rrule": "FREQ=WEEKLY",
            "description": "Recurs across the clocks going back.",
        })
        assert status == 303, f"saving an event returned {status}"

        # October: the 20th and 27th.
        _, page = c.get(cal + "?y=2026&m=10")
        assert "Weekly sync" in page, "the event is not on the October grid"
        assert page.count("Weekly sync") >= 2, \
            f"a weekly event should appear more than once in October, saw {page.count('Weekly sync')}"

        # The agenda view of the same month, which is VIEW_CALBRIEF's layout.
        _, agenda = c.get(cal + "?y=2026&m=10&v=list")
        assert "Weekly sync" in agenda and "Room 3" in agenda, "the agenda is missing the event"

        # November, after the change: still there, and still at 09:00 local.
        _, page = c.get(cal + "?y=2026&m=11")
        assert "09:00 Weekly sync" in page, \
            "the recurrence did not keep its local time across the DST change"
        assert "08:00 Weekly sync" not in page, \
            "the recurrence drifted an hour — it was expanded in UTC, not wall-clock"

        nums = item_numbers(page, cal)
        assert nums, "no event on the November grid links to an item"
        ev = nums[0]

        status, detail = c.get(f"{cal}/item/{ev}")
        assert status == 200 and "Weekly sync" in detail, f"the event page returned {status}"
        assert "Room 3" in detail
        assert "FREQ=WEEKLY" in detail, "the repeat rule is not shown"

        # An alarm added out of band — as a phone would — must survive an edit
        # made through the form.
        con.execute(
            """
            UPDATE citadel_messages
               SET raw = replace(decode(raw), 'END:VEVENT',
                     'BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT15M\r\n'
                     'DESCRIPTION:Ping\r\nEND:VALARM\r\nEND:VEVENT')::BLOB
             WHERE msgnum = ?
            """,
            [int(ev)],
        )
        status, form = c.get(f"{cal}/item/{ev}/edit")
        assert status == 200 and 'value="Weekly sync"' in form, "the edit form is not prefilled"
        status, _ = c.post(cal + "/item/save", {
            "_csrf": csrf(form), "msgnum": ev, "summary": "Weekly sync (renamed)",
            "date": "2026-10-20", "time": "09:00",
            "end_date": "2026-10-20", "end_time": "10:00",
            "location": "Room 4", "rrule": "FREQ=WEEKLY", "description": "Edited.",
        })
        assert status == 303, f"saving an edit returned {status}"

        _, page = c.get(cal + "?y=2026&m=10")
        assert "Weekly sync (renamed)" in page, "the edit did not take"
        newnums = item_numbers(page, cal)
        assert newnums, "the edited event vanished from the grid"
        stored = con.execute(
            "SELECT decode(raw) FROM citadel_messages WHERE msgnum = ?", [int(newnums[0])]
        ).fetchone()[0]
        assert "BEGIN:VALARM" in stored, \
            "editing through the form deleted the alarm — ApplyItem was bypassed"
        assert "TRIGGER:-PT15M" in stored
        assert "Room 4" in stored, "the edited location was not written"
        # One object, not two: the edit replaced it.
        count = con.execute(
            "SELECT count(*) FROM citadel_room_msgs WHERE room_num = "
            "(SELECT room_num FROM citadel_rooms WHERE room_num = ?)", [int(cal.split("/")[-1])]
        ).fetchone()[0]
        assert count == 1, f"the calendar holds {count} objects after an edit, not 1"

        # An all-day event has no time and covers its whole day.
        _, form = c.get(cal + "/item/new")
        status, _ = c.post(cal + "/item/save", {
            "_csrf": csrf(form), "summary": "Company holiday",
            "date": "2026-12-25", "time": "00:00",
            "end_date": "2026-12-25", "end_time": "00:00",
            "all_day": "1", "rrule": "",
        })
        assert status == 303, f"saving an all-day event returned {status}"
        _, page = c.get(cal + "?y=2026&m=12")
        assert "Company holiday" in page, "the all-day event is missing"
        assert "00:00 Company holiday" not in page, "an all-day event should not show a time"
        # And it lands on the 25th, not the 24th. An all-day value is a date, not
        # an instant, so shifting it through a zone west of Greenwich would move
        # it back a day — the bug this asserts against.
        # EscapeAttr renders '=' as &#61;, so match the decoded page.
        cell = re.search(
            r'<div class="num">[^<]*<a[^>]*\?date=2026-12-25"[^>]*>25</a></div>(.*?)</td>',
            html.unescape(page), re.S)
        assert cell, "no cell for 25 December on the grid"
        assert "Company holiday" in cell.group(1), \
            "the all-day event is not on the 25th — its date was shifted by the time zone"

        # A malformed date is refused rather than stored as 1970.
        _, form = c.get(cal + "/item/new")
        status, _ = c.post(cal + "/item/save", {"_csrf": csrf(form), "summary": "Bad",
                                                "date": "not-a-date", "time": "09:00"})
        assert status == 400, f"a malformed date returned {status}, not 400"
        # And an event that ends before it starts.
        _, form = c.get(cal + "/item/new")
        status, _ = c.post(cal + "/item/save", {
            "_csrf": csrf(form), "summary": "Backwards", "date": "2026-05-01", "time": "10:00",
            "end_date": "2026-05-01", "end_time": "09:00"})
        assert status == 400, f"an event ending before it starts returned {status}, not 400"

        # ---- tasks ----------------------------------------------------------
        status, page = c.get(tasks)
        assert status == 200 and "Nothing to do here" in page, f"tasks returned {status}"
        _, form = c.get(tasks + "/item/new")
        status, _ = c.post(tasks + "/item/save", {
            "_csrf": csrf(form), "summary": "Write the docs", "due": "2026-04-01",
            "priority": "2", "percent": "0", "description": "Including this file.",
        })
        assert status == 303, f"saving a task returned {status}"
        _, page = c.get(tasks)
        assert "Write the docs" in page, "the task is not listed"
        assert "2026-04-01" in page, "the due date is not listed"

        tnums = item_numbers(page, tasks)
        assert tnums, "the task does not link to a detail page"
        tid = tnums[0]

        # Completing from the list is a POST, not a link: it changes state.
        status, _ = c.post(tasks + "/item/complete", {"_csrf": csrf(page), "msgnum": tid,
                                                      "done": "1"})
        assert status == 303, f"completing a task returned {status}"
        _, page = c.get(tasks)
        assert "completed task hidden" in page, "a completed task is still shown by default"
        _, page = c.get(tasks + "?done=1")
        assert "taskdone" in page, "the completed task is not marked as done"
        assert "Write the docs" in page

        # Reopening it puts it back.
        tnums = item_numbers(page, tasks)
        status, _ = c.post(tasks + "/item/complete", {"_csrf": csrf(page), "msgnum": tnums[0],
                                                      "done": "0"})
        assert status == 303, f"reopening a task returned {status}"
        _, page = c.get(tasks)
        assert "Write the docs" in page and "completed task hidden" not in page, \
            "the task did not reopen"

        # ---- notes ----------------------------------------------------------
        status, page = c.get(notes)
        assert status == 200 and "No notes here yet" in page, f"notes returned {status}"
        _, form = c.get(notes + "/item/new")
        assert 'class="swatch' in form, "the note colour picker did not render as swatches"
        status, _ = c.post(notes + "/item/save", {
            "_csrf": csrf(form), "summary": "Remember", "body": "Milk, bread, a new BBS.",
            "color": "#ffff88",
        })
        assert status == 303, f"saving a note returned {status}"
        _, page = c.get(notes)
        assert "Remember" in page, "the note is not listed"
        assert "notegrid" in page, "notes did not render as a grid"
        assert "--note:#ffff88" in page, "the note colour was not applied"

        nnums = item_numbers(page, notes)
        assert nnums, "the note does not link to a detail page"
        status, detail = c.get(f"{notes}/item/{nnums[0]}")
        assert status == 200 and "Milk, bread" in detail, f"the note page returned {status}"

        # A colour we do not offer is dropped rather than reflected into style=.
        _, form = c.get(f"{notes}/item/{nnums[0]}/edit")
        status, _ = c.post(notes + "/item/save", {
            "_csrf": csrf(form), "msgnum": nnums[0], "summary": "Remember",
            "body": "Milk, bread, a new BBS.", "color": "red; background:url(x)",
        })
        assert status == 303, f"saving a note with a bad colour returned {status}"
        _, page = c.get(notes)
        assert "url(x)" not in page, "an unvalidated colour reached the page"

        # ---- a note in Markdown, rendered rather than escaped ----------------
        existing_nums = item_numbers(page, notes)
        _, form = c.get(notes + "/item/new")
        status, _ = c.post(notes + "/item/save", {
            "_csrf": csrf(form), "summary": "Formatted", "body": "**bold** and a list:\n\n- one\n- two",
            "format": "text/x-markdown",
        })
        assert status == 303, f"saving a markdown note returned {status}"
        _, page = c.get(notes)
        md_num = [n for n in item_numbers(page, notes) if n not in existing_nums]
        assert md_num, "the markdown note does not link to a detail page"
        status, detail = c.get(f"{notes}/item/{md_num[0]}")
        assert status == 200 and "<strong>bold</strong>" in detail, (
            "the markdown note did not render as HTML"
        )
        assert "richnote" in detail, "the rendered note is not in a richnote box"
        assert "<li>one</li>" in detail, "the markdown list did not render"

        # ---- blog -----------------------------------------------------------
        # No personal blog room exists by default, so make one and post to it.
        con.execute("CALL cit_room_add('Web log')")
        blog_num = con.execute(
            "SELECT room_num FROM citadel_rooms WHERE display_name = 'Web log'").fetchone()[0]
        con.execute("UPDATE citadel_rooms SET default_view = 10 WHERE room_num = ?", [blog_num])
        blog = f"/bbs/room/{blog_num}"

        status, page = c.get(blog + "/compose")
        assert status == 200, f"the blog compose form returned {status}"
        status, _ = c.post(blog + "/post", {"_csrf": csrf(page), "subject": "First post",
                                            "body": "Hello from a blog room."})
        assert status == 303, f"posting to a blog room returned {status}"

        status, page = c.get(blog)
        assert status == 200, f"the blog returned {status}"
        assert "First post" in page, "the entry is not on the blog page"
        assert "Hello from a blog room." in page, \
            "the blog shows only subjects — entries should render in full"
        assert 'class="entry"' in page, "the blog did not use the entry layout"
        assert "Write an entry" in page

        # ---- the escape hatch works for every view --------------------------
        for path in (cal, tasks, notes, blog):
            status, page = c.get(path + "?view=raw")
            assert status == 200 and "Mark all read" in path + page, \
                f"{path}?view=raw did not fall back to the message list ({status})"

        # ---- the same objects over IMAP -------------------------------------
        im = imaplib.IMAP4(HOST, IMAP_PORT)
        im.login(USER, PASSWORD)
        n, body = imap_body(im, "Calendar", "text/calendar")
        assert "BEGIN:VCALENDAR" in body, "the calendar part is not iCalendar"
        assert n == 2, f"IMAP sees {n} calendar objects, not 2"
        _, body = imap_body(im, "Tasks", "text/calendar")
        assert "BEGIN:VTODO" in body, f"the task is not a VTODO: {body[:200]}"
        _, body = imap_body(im, "Notes", "text/vnote")
        assert "BEGIN:VNOTE" in body, f"the note is not a vNote: {body[:200]}"
        im.logout()

    finally:
        con.execute("CALL qm_http_stop()")
        con.execute("CALL qm_imap_stop()")

    print("PASS: room views (calendar with DST-correct recurrence, tasks, notes, blog, IMAP parity)")


if __name__ == "__main__":
    sys.exit(main())
