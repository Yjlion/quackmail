#!/usr/bin/env python3
"""End-to-end test for wiki rooms.

The wiki is the one room view whose *storage* has to interoperate with software
we do not control. A page is an ordinary message keyed by its euid, and its
history is a second message holding a chain of reverse unified diffs, because
that is what `citadel/server/modules/wiki/serv_wiki.c` writes and what WebCit
reads. So most of what is asserted here is shape rather than behaviour:

  * the history message exists, is keyed `<page>_HISTORY_`, is authored by
    "Citadel", and holds one `text/x-diff` part per revision, newest first;
  * each part's filename is base64 of "<msgnum>|<timestamp>|<author>|",
    *including the trailing NUL* Citadel encodes;
  * every stored revision reconstructs to exactly the bytes that were saved;
  * a save that changes nothing is refused rather than stored as an empty diff,
    which is what Citadel does and what keeps the chain replayable;
  * versioning happens in the *store*, so a page written over IMAP gets history
    too — not only one written through the web form.

Requires: pip install duckdb==1.5.4
Run after `make release` so the loadable extensions exist under
build/release/extension.
"""
import base64
import email
import html
import http.cookiejar
import imaplib
import os
import re
import socket
import sys
import urllib.error
import urllib.parse
import urllib.request

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 18096
IMAP_PORT = 11436
CIT_PORT = 15046
BASE = f"http://{HOST}:{PORT}"

USER = "wikiuser"
PASSWORD = "secret"
ROOM = "Handbook"


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


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_citadel", "quackmail_http", "quackmail_imap"):
        con.execute(f"LOAD '{ext(name)}'")
    con.execute("SELECT count(*) FROM qm_status()").fetchall()

    con.execute(f"CALL qm_user_add('{USER}', '{PASSWORD}')")
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    con.execute(f"CALL cit_room_add('{ROOM}')")
    con.execute(
        "UPDATE citadel_rooms SET default_view = 6 WHERE display_name = ?", [ROOM])
    room_num = con.execute(
        "SELECT room_num FROM citadel_rooms WHERE display_name = ?", [ROOM]).fetchone()[0]
    con.execute(f"CALL qm_http_start('{HOST}', {PORT})")
    con.execute(f"CALL qm_imap_start('{HOST}', {IMAP_PORT})")
    con.execute(f"CALL cit_start('{HOST}', {CIT_PORT})")

    room = f"/bbs/room/{room_num}"

    def page_msg(euid):
        row = con.execute(
            "SELECT m.msgnum, m.author, m.subject, m.raw "
            "  FROM citadel_room_msgs rm JOIN citadel_messages m ON m.msgnum = rm.msgnum "
            " WHERE rm.room_num = ? AND m.euid = ?", [room_num, euid]).fetchone()
        return row

    try:
        c = Client()
        _, page = c.get("/login")
        status, _ = c.post("/login", {"username": USER, "password": PASSWORD,
                                      "_csrf": csrf(page)})
        assert status == 303, f"sign-in returned {status}"

        # ---- the room renders as a wiki, not a message board ---------------
        status, index = c.get(room)
        assert status == 200, f"the wiki room returned {status}"
        assert "This wiki has no pages yet" in index, index[:400]
        assert "Start the front page" in index

        # ?view=raw still escapes to the message list, as for every other view.
        status, raw = c.get(room + "?view=raw")
        assert status == 200 and "This wiki has no pages yet" not in raw, \
            "?view=raw did not fall back to the message list"

        # ---- create ---------------------------------------------------------
        status, form = c.get(room + "/wiki/edit")
        assert status == 200 and 'name="body"' in form, f"the edit form returned {status}"

        V1 = "# Handbook\n\nWelcome to the [[glossary]].\n"
        status, _ = c.post(room + "/wiki/save", {
            "_csrf": csrf(form), "page": "Home", "format": "text/x-markdown", "body": V1})
        assert status == 303, f"creating a page returned {status}"

        row = page_msg("home")
        assert row is not None, "the page was not stored under the euid 'home'"
        first_msgnum, _, subject, _ = row
        assert subject == "Home", f"the subject should be the name as typed, got {subject!r}"

        status, shown = c.get(room + "/wiki?page=home")
        assert status == 200, f"reading the page returned {status}"
        assert "<h1>Handbook</h1>" in shown, "markdown was not rendered"
        # A link to a page nobody has written yet is marked as such - the
        # affordance that makes a wiki a wiki.
        assert 'class="wanted"' in shown, "a link to a missing page was not marked"
        # A wanted link points at the create form, not at a page that 404s.
        assert "page=glossary" in html.unescape(shown), \
            "the wiki link does not carry the page name"
        assert "/wiki/edit?page=glossary" in html.unescape(shown), \
            "a link to a missing page should go to the create form"

        # A page with no history yet says so rather than 404ing.
        status, hist = c.get(room + "/wiki/history?page=home")
        assert status == 200 and "has not been edited" in hist, hist[:400]

        # ---- edit twice -----------------------------------------------------
        V2 = "# Handbook\n\nWelcome to the [[glossary]].\n\nSecond revision.\n"
        V3 = "# Handbook\n\nWelcome.\n\nSecond revision.\n\nThird.\n"
        for text in (V2, V3):
            _, form = c.get(room + "/wiki/edit?page=home")
            status, body = c.post(room + "/wiki/save", {
                "_csrf": csrf(form), "page": "Home", "format": "text/x-markdown", "body": text})
            assert status == 303, f"editing returned {status}: {body[:300]}"

        # ---- the history message has Citadel's shape ------------------------
        hist_row = page_msg("home_HISTORY_")
        assert hist_row is not None, "no history message was written"
        _, hist_author, hist_subject, hist_raw = hist_row
        assert hist_author == "Citadel", f"history author is {hist_author!r}"
        assert hist_subject == "home_HISTORY_", f"history subject is {hist_subject!r}"
        hist_text = bytes(hist_raw).decode("utf-8", "replace")
        assert "multipart/mixed" in hist_text, hist_text[:300]
        assert "This is a Citadel wiki history" in hist_text, \
            "the preamble Citadel writes is missing"

        parsed = email.message_from_string(hist_text)
        parts = [p for p in parsed.walk() if p.get_content_type() == "text/x-diff"]
        assert len(parts) == 2, f"expected two revisions, found {len(parts)}"

        # Each part's filename is base64 of "<msgnum>|<timestamp>|<author>|",
        # *with* the trailing NUL Citadel encodes.
        memos = []
        for p in parts:
            fn = p.get_filename() or p.get_param("filename", header="content-disposition")
            assert fn, "a history part has no filename memo"
            memo = base64.b64decode(fn + "=" * (-len(fn) % 4))
            assert memo.endswith(b"\0"), "the memo is not NUL-terminated as Citadel writes it"
            fields = memo[:-1].decode().split("|")
            assert len(fields) >= 3, f"memo has too few fields: {fields}"
            memos.append(fields)
        revs = [int(m[0]) for m in memos]
        # Newest first: new revisions are prepended after the opening boundary.
        assert revs == sorted(revs, reverse=True), f"revisions are not newest-first: {revs}"
        assert revs[-1] == first_msgnum, \
            f"the oldest revision should be the first msgnum {first_msgnum}, got {revs[-1]}"
        assert all(m[2] == USER for m in memos), f"authors are wrong: {memos}"

        # ---- the history page lists them ------------------------------------
        status, hist = c.get(room + "/wiki/history?page=home")
        assert status == 200, f"history returned {status}"
        # The page shell escapes '=' as &#61; as well as '&', so compare
        # against the unescaped text rather than guessing at the entities.
        unescaped = html.unescape(hist)
        for rev in revs:
            assert f"rev={rev}" in unescaped, f"revision {rev} is not listed"

        # ---- every revision reconstructs to what was saved -------------------
        # The revision is rendered, so it is identified by what it contains and
        # what it does not rather than by a literal line of source.
        oldest, newest = revs[-1], revs[0]
        checks = {
            oldest: ("glossary", "Second revision"),   # V1: before the second edit
            newest: ("Second revision", "Third"),      # V2: after it, before the third
        }
        for rev, (present, absent) in checks.items():
            status, shown = c.get(f"{room}/wiki/rev?page=home&rev={rev}")
            assert status == 200, f"revision {rev} returned {status}"
            assert "old revision" in shown, "the historical banner is missing"
            body = shown[shown.find('<div class="wiki">'):]
            assert present in body, f"revision {rev} is missing {present!r}"
            assert absent not in body, \
                f"revision {rev} contains {absent!r}, which was added later"

        # The diff view renders forwards: what changed since then.
        status, changes = c.get(f"{room}/wiki/diff?page=home&rev={revs[0]}")
        assert status == 200 and 'class="diff"' in changes, changes[:400]
        assert "Third." in changes, "the diff does not mention the added line"

        # ---- a save that changes nothing is refused --------------------------
        _, form = c.get(room + "/wiki/edit?page=home")
        status, _ = c.post(room + "/wiki/save", {
            "_csrf": csrf(form), "page": "Home", "format": "text/x-markdown", "body": V3})
        assert status == 303, f"an unchanged save returned {status}"
        parsed = email.message_from_string(
            bytes(page_msg("home_HISTORY_")[3]).decode("utf-8", "replace"))
        again = [p for p in parsed.walk() if p.get_content_type() == "text/x-diff"]
        assert len(again) == 2, \
            f"an unchanged save wrote a history entry ({len(again)} parts)"

        # ---- revert ----------------------------------------------------------
        status, _ = c.post(room + "/wiki/revert", {
            "_csrf": csrf(hist), "page": "home", "rev": str(revs[-1])})
        assert status == 303, f"revert returned {status}"
        status, shown = c.get(room + "/wiki?page=home")
        assert "Welcome to the" in shown, "the revert did not restore the old text"
        # A revert is an edit, so it is itself undoable.
        parsed = email.message_from_string(
            bytes(page_msg("home_HISTORY_")[3]).decode("utf-8", "replace"))
        after = [p for p in parsed.walk() if p.get_content_type() == "text/x-diff"]
        assert len(after) == 3, f"revert did not record a revision ({len(after)} parts)"

        # ---- markdown is stored as source, and rendered safely ---------------
        _, form = c.get(room + "/wiki/edit?page=home")
        assert V1.strip().splitlines()[0] in html.unescape(form), \
            "the edit form does not show the markdown source"

        _, form = c.get(room + "/wiki/edit")
        status, _ = c.post(room + "/wiki/save", {
            "_csrf": csrf(form), "page": "Nasty",
            "format": "text/x-markdown",
            "body": "<script>alert(1)</script>\n\n[x](javascript:alert(1))\n"})
        assert status == 303
        _, shown = c.get(room + "/wiki?page=nasty")
        assert "<script>" not in shown, "raw HTML survived into the page"
        assert "javascript:" not in shown, "a javascript: URL survived into an href"

        # ---- the index lists pages, not history companions -------------------
        status, index = c.get(room)
        assert status == 200
        assert "_HISTORY_" not in index, "the page index lists history companions"
        assert ">Home<" in index and ">Nasty<" in index, index[:600]

        # ---- a page is an ordinary MIME message over IMAP --------------------
        # The parity assertion every other groupware view makes: whatever the
        # web interface stored has to read back as a sensible message elsewhere,
        # because that is the whole reason objects are stored as messages.
        #
        # Note what is *not* claimed here. Versioning hooks citadel::UpsertByEuid
        # — the euid-keyed write path — so every front-end that saves a page by
        # name records history. A plain IMAP APPEND carries no euid, so it adds
        # a new message rather than a revision of an existing page, which is
        # what it asked for.
        im = imaplib.IMAP4(HOST, IMAP_PORT)
        im.login(USER, PASSWORD)
        typ, _ = im.select(ROOM)
        assert typ == "OK", f"IMAP could not select {ROOM}"
        typ, data = im.search(None, "ALL")
        assert typ == "OK" and data[0].split(), "no messages visible over IMAP"
        seen_page = False
        for num in data[0].split():
            typ, fetched = im.fetch(num, "(RFC822)")
            if typ != "OK" or not fetched or not isinstance(fetched[0], tuple):
                continue
            msg = email.message_from_bytes(fetched[0][1])
            types = {p.get_content_type() for p in msg.walk()}
            if "text/x-markdown" not in types:
                continue
            if (msg.get("Subject") or "").strip() != "Home":
                continue
            seen_page = True
            body = msg.get_payload(decode=True) or b""
            # Markdown is stored as *source*: it is rendered safely on the way
            # out, not mangled on the way in, so the bytes are what was typed.
            assert b"Handbook" in body, \
                f"the front page read back over IMAP as {body[:80]!r}"
        im.logout()
        assert seen_page, "the front page was not readable over IMAP as text/x-markdown"

        # ---- the native protocol serves the same history ---------------------
        # The whole reason for storing history in Citadel's shape: a client
        # speaking the native protocol can browse it. WIKI history| and WIKI rev|
        # are what WebCit and the Citadel text client call.
        cit = socket.create_connection((HOST, CIT_PORT), timeout=10)
        cf = cit.makefile("rwb")

        def cmd(line):
            cf.write(line.encode() + b"\n")
            cf.flush()
            return cf.readline().decode().rstrip("\r\n")

        def listing():
            out = []
            while True:
                ln = cf.readline().decode().rstrip("\r\n")
                if ln == "000":
                    return out
                out.append(ln)

        cf.readline()  # greeting
        assert cmd(f"USER {USER}").startswith("3"), "USER refused"
        assert cmd(f"PASS {PASSWORD}").startswith("2"), "PASS refused"
        assert cmd(f"GOTO {ROOM}").startswith("2"), f"GOTO {ROOM} refused"

        reply = cmd("WIKI history|home")
        assert reply.startswith("1"), f"WIKI history returned {reply!r}"
        memos = listing()
        assert len(memos) == 3, f"WIKI history listed {len(memos)} revisions: {memos}"
        cit_revs = [int(m.split("|")[0]) for m in memos]
        assert cit_revs == sorted(cit_revs, reverse=True), \
            f"WIKI history is not newest-first: {cit_revs}"

        reply = cmd(f"WIKI rev|home|{cit_revs[-1]}|showrev")
        assert reply.startswith("1"), f"WIKI rev returned {reply!r}"
        text = "\n".join(listing())
        assert "Handbook" in text, f"WIKI rev did not return the page: {text[:200]!r}"

        # A room that is not a wiki says so rather than pretending.
        assert cmd("GOTO Lobby").startswith("2")
        assert cmd("WIKI history|home").startswith("5"), \
            "WIKI answered in a room that is not a wiki"

        cmd("QUIT")
        cit.close()

        # ---- a page cannot be called _HISTORY_ -------------------------------
        _, form = c.get(room + "/wiki/edit")
        status, body = c.post(room + "/wiki/save", {
            "_csrf": csrf(form), "page": "sneaky_HISTORY_",
            "format": "text/x-markdown", "body": "x\n"})
        assert status == 400, f"a reserved page name was accepted ({status})"

        print("test_wiki: OK")
    finally:
        try:
            con.execute("CALL qm_http_stop()")
            con.execute("CALL qm_imap_stop()")
            con.execute("CALL cit_stop()")
        except Exception:
            pass


if __name__ == "__main__":
    main()
