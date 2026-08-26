#!/usr/bin/env python3
"""End-to-end test for the web contacts view (quackmail_http, VIEW_ADDRESSBOOK).

Loads the extensions into an in-memory DuckDB, starts the HTTP and IMAP
listeners, and drives the address book the way a browser does: list, create,
read, edit, delete. Then reads the same objects back over IMAP.

That last part is the point. Groupware objects are stored as ordinary messages
(format_type 4, one text/vcard part, keyed by euid), so a contact created in the
browser must be a readable MIME message to every other front-end with no code
of its own. If that stops being true, this file fails rather than someone
discovering it from a phone months later.

The other assertion worth naming: editing a contact *replaces* it. Saving twice
must leave one contact, not two, and the pre-edit message number must go away
rather than lingering as a second copy.

Requires: pip install duckdb==1.5.4
Run after `make` so the loadable extensions exist under build/release/extension.
"""
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
PORT = 18091
IMAP_PORT = 11431
BASE = f"http://{HOST}:{PORT}"

USER = "webcontact"
PASSWORD = "secret"


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """Follow nothing: the status and Location are part of what is under test."""

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


def item_number(page, room_path):
    """The message number of the first contact linked on a listing page."""
    m = re.search(r'href="' + re.escape(room_path) + r'/item/(\d+)"', page)
    return m.group(1) if m else None


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_http", "quackmail_imap"):
        con.execute(f"LOAD '{ext(name)}'")
    # EnsureSchema runs from a table function's init, not from LOAD, so warm the
    # catalog before touching citadel_* directly.
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

        # The sidebar links the user's own groupware rooms, provisioned by
        # EnsureUserRooms at first login.
        _, home = c.get("/mail/")
        m = re.search(r'href="(/bbs/room/(\d+))"[^>]*><span>Contacts</span>', home)
        assert m, "the sidebar does not link a Contacts room"
        room_path = m.group(1)

        # ---- the room renders as an address book, not a message list --------
        status, page = c.get(room_path)
        assert status == 200, f"the contacts room returned {status}"
        assert "No contacts here yet" in page, "an empty address book did not say so"
        assert "Add a contact" in page
        # Any room can still be read as what it physically is.
        assert "View as messages" in page

        # ---- create ---------------------------------------------------------
        status, form = c.get(room_path + "/item/new")
        assert status == 200 and 'name="family"' in form, f"the add form returned {status}"
        status, _ = c.post(room_path + "/item/save", {
            "_csrf": csrf(form),
            "given": "Ada", "family": "Lovelace",
            "org": "Analytical Engines Ltd",
            "email": "ada@example.org",
            "tel": "+44 20 7946 0000",
            "note": "First programmer",
        })
        assert status == 303, f"saving a contact returned {status}"

        _, page = c.get(room_path)
        assert "Ada Lovelace" in page, "the new contact is not listed"
        assert "ada@example.org" in page
        assert "Analytical Engines" in page
        assert "1 contact." in page, "the count is wrong after one contact"

        msgnum = item_number(page, room_path)
        assert msgnum, "the contact does not link to a detail page"

        # ---- read -----------------------------------------------------------
        status, page = c.get(f"{room_path}/item/{msgnum}")
        assert status == 200, f"the detail page returned {status}"
        assert "Ada Lovelace" in page
        assert "+44 20 7946 0000" in page
        assert "First programmer" in page
        # An address book exists to let you write to people. EscapeAttr renders
        # '=' as &#61;, which a browser decodes back, so compare the decoded href.
        assert "/mail/compose?to=ada%40example.org" in html.unescape(page), \
            "the address is not a link that composes to it"

        # ---- edit replaces, rather than appending ---------------------------
        status, form = c.get(f"{room_path}/item/{msgnum}/edit")
        assert status == 200 and 'value="Lovelace"' in form, \
            f"the edit form returned {status} or was not prefilled"
        status, _ = c.post(room_path + "/item/save", {
            "_csrf": csrf(form), "msgnum": msgnum,
            "given": "Augusta Ada", "family": "Lovelace",
            "org": "Analytical Engines Ltd", "email": "ada@example.org",
            "tel": "+44 20 7946 0000", "note": "First programmer",
        })
        assert status == 303, f"saving an edit returned {status}"

        _, page = c.get(room_path)
        assert "Augusta Ada Lovelace" in page, "the edit did not take"
        assert "1 contact." in page, "editing appended a second contact instead of replacing"

        # Replacing means a new message number. The old one must be gone, not
        # left pointing at a stale copy.
        status, _ = c.get(f"{room_path}/item/{msgnum}")
        assert status == 404, f"the pre-edit message number still resolves ({status})"
        msgnum = item_number(page, room_path)
        assert msgnum, "the edited contact has no message number"

        # ---- a second contact, and ordering ---------------------------------
        _, form = c.get(room_path + "/item/new")
        c.post(room_path + "/item/save", {"_csrf": csrf(form), "given": "Alan",
                                          "family": "Turing", "email": "alan@example.org"})
        _, page = c.get(room_path)
        assert "2 contacts." in page, "the second contact is missing"
        assert page.index("Alan Turing") < page.index("Augusta Ada Lovelace"), \
            "contacts are not sorted by display name"

        # ---- the escape hatch -----------------------------------------------
        status, page = c.get(room_path + "?view=raw")
        assert status == 200 and "Mark all read" in page, \
            "?view=raw did not fall back to the message list"

        # ---- the same objects over IMAP -------------------------------------
        im = imaplib.IMAP4(HOST, IMAP_PORT)
        im.login(USER, PASSWORD)
        typ, data = im.select("Contacts")
        assert typ == "OK", f"IMAP could not select Contacts: {data}"
        typ, nums = im.search(None, "ALL")
        ids = nums[0].split()
        assert len(ids) == 2, f"IMAP sees {len(ids)} contacts, not 2"
        typ, raw = im.fetch(ids[0], "(RFC822)")
        body = raw[0][1].decode("utf-8", "replace")
        assert "BEGIN:VCARD" in body, f"the IMAP body is not a vCard: {body[:200]}"
        assert "text/vcard" in body, "the part is not typed as a vCard"
        assert "Subject: " in body, "the message has no subject to list it by"
        im.logout()

        # ---- the compose form offers this contact ----------------------------
        status, page = c.get("/mail/compose")
        assert status == 200
        assert '<datalist id="addressbook">' in page, "compose has no address-book datalist"
        assert "Augusta Ada Lovelace &lt;ada@example.org&gt;" in page, (
            "the contact's address is not offered in the address-book picker"
        )
        assert 'list="addressbook"' in page, "the To field is not wired to the datalist"

        # ---- delete ---------------------------------------------------------
        _, page = c.get(f"{room_path}/item/{msgnum}")
        status, _ = c.post(room_path + "/item/delete",
                           {"_csrf": csrf(page), "msgnum": msgnum})
        assert status == 303, f"delete returned {status}"
        _, page = c.get(room_path)
        assert "1 contact." in page, "delete did not remove one"
        assert "Augusta Ada Lovelace" not in page, "the deleted contact is still listed"

    finally:
        con.execute("CALL qm_http_stop()")
        con.execute("CALL qm_imap_stop()")

    print("PASS: contacts view (create, read, edit-in-place, delete, IMAP parity)")


if __name__ == "__main__":
    sys.exit(main())
