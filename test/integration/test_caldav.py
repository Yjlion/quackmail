#!/usr/bin/env python3
"""End-to-end test for CalDAV and CardDAV over the groupware rooms.

Companion to test_groupware.py, which drives the same rooms through the web UI.
This one drives them the way a phone does, and then checks that both front-ends
and IMAP are looking at one object rather than three copies of it.

The assertions worth naming, because they are the ones that would regress
silently:

  * A PUT stores the client's bytes *verbatim*. A sync target that quietly
    rewrites what it was given loses every property the server does not model,
    and the user never finds out until the property mattered.
  * A deletion is visible to sync-collection. Nothing in the store recorded one
    before citadel_room_tombstones existed, so a cancelled event would have
    stayed on the phone forever.
  * If-Match actually refuses a stale write. Without it two clients editing one
    event silently overwrite each other.
  * CanPost is the gate on PUT and DELETE. Every other front-end asks it; a DAV
    module that re-derived the rule is how a read-only room stops being one.

Requires: pip install duckdb==1.5.4
Run after `make release` so the loadable extensions exist under
build/release/extension.
"""
import base64
import imaplib
import os
import re
import sys
import urllib.error
import urllib.request

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 18096
IMAP_PORT = 11436
BASE = f"http://{HOST}:{PORT}"

USER = "davuser"
PASSWORD = "secret"
OTHER = "davother"
OTHER_PASSWORD = "secret2"

EVENT_UID = "party-2026@example.com"
EVENT = (
    "BEGIN:VCALENDAR\r\n"
    "VERSION:2.0\r\n"
    "PRODID:-//Test//EN\r\n"
    "BEGIN:VEVENT\r\n"
    f"UID:{EVENT_UID}\r\n"
    "DTSTAMP:20260701T120000Z\r\n"
    "DTSTART:20260815T170000Z\r\n"
    "DTEND:20260815T190000Z\r\n"
    "SUMMARY:Rooftop party\r\n"
    # Deliberately something the flat ical::Item does not model. If a PUT ever
    # starts round-tripping through ical::Emit, this is what disappears.
    "X-CUSTOM-VENDOR-FIELD:keep me exactly as written\r\n"
    "BEGIN:VALARM\r\n"
    "ACTION:DISPLAY\r\n"
    "TRIGGER:-PT15M\r\n"
    "DESCRIPTION:Soon\r\n"
    "END:VALARM\r\n"
    "END:VEVENT\r\n"
    "END:VCALENDAR\r\n"
)

FAR_UID = "distant-2027@example.com"
FAR_EVENT = (
    "BEGIN:VCALENDAR\r\n"
    "VERSION:2.0\r\n"
    "PRODID:-//Test//EN\r\n"
    "BEGIN:VEVENT\r\n"
    f"UID:{FAR_UID}\r\n"
    "DTSTAMP:20260701T120000Z\r\n"
    "DTSTART:20271201T100000Z\r\n"
    "DTEND:20271201T110000Z\r\n"
    "SUMMARY:Next year entirely\r\n"
    "END:VEVENT\r\n"
    "END:VCALENDAR\r\n"
)

CARD_UID = "contact-77@example.com"
CARD = (
    "BEGIN:VCARD\r\n"
    "VERSION:3.0\r\n"
    f"UID:{CARD_UID}\r\n"
    "FN:Ada Lovelace\r\n"
    "N:Lovelace;Ada;;;\r\n"
    "EMAIL;TYPE=WORK:ada@example.com\r\n"
    "END:VCARD\r\n"
)


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def dav_name(euid):
    """The encoding core/davxml.cpp uses. Kept here rather than derived so the
    test pins the wire format instead of agreeing with whatever the code does."""
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
    """A DAV client: no cookies, no CSRF, Basic on every request."""

    def __init__(self, user=USER, password=PASSWORD):
        self.auth = base64.b64encode(f"{user}:{password}".encode()).decode()
        self.op = urllib.request.build_opener()

    def go(self, method, path, body=None, headers=None, auth=True):
        req = urllib.request.Request(BASE + path, data=body, method=method)
        if auth:
            req.add_header("Authorization", "Basic " + self.auth)
        for k, v in (headers or {}).items():
            req.add_header(k, v)
        try:
            r = self.op.open(req, timeout=20)
            return r.status, dict(r.headers), r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), e.read().decode("utf-8", "replace")

    def propfind(self, path, body, depth="0"):
        return self.go("PROPFIND", path, body.encode(),
                       {"Depth": depth, "Content-Type": "application/xml"})

    def report(self, path, body, depth="1"):
        return self.go("REPORT", path, body.encode(),
                       {"Depth": depth, "Content-Type": "application/xml"})


ALLPROP = '<?xml version="1.0"?><D:propfind xmlns:D="DAV:"><D:allprop/></D:propfind>'


def prop(names):
    """A PROPFIND body asking for named properties, prefixes and all."""
    body = ['<?xml version="1.0"?><D:propfind xmlns:D="DAV:" '
            'xmlns:C="urn:ietf:params:xml:ns:caldav" '
            'xmlns:CARD="urn:ietf:params:xml:ns:carddav" '
            'xmlns:CS="http://calendarserver.org/ns/"><D:prop>']
    body += ["<%s/>" % n for n in names]
    body.append("</D:prop></D:propfind>")
    return "".join(body)


def hrefs(xml):
    return re.findall(r"<D:href>([^<]*)</D:href>", xml)


def between(xml, tag):
    m = re.search(r"<%s>(.*?)</%s>" % (tag, tag), xml, re.S)
    return m.group(1) if m else None


def responses(xml):
    """Split a multistatus into (href, body) pairs."""
    out = []
    for block in re.findall(r"<D:response>(.*?)</D:response>", xml, re.S):
        m = re.search(r"<D:href>([^<]*)</D:href>", block)
        out.append((m.group(1) if m else "", block))
    return out


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_http", "quackmail_imap"):
        con.execute(f"LOAD '{ext(name)}'")
    con.execute("SELECT count(*) FROM qm_status()").fetchall()

    con.execute(f"CALL qm_user_add('{USER}', '{PASSWORD}')")
    con.execute(f"CALL qm_user_add('{OTHER}', '{OTHER_PASSWORD}')")
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    con.execute("CALL qm_config_set('c_fqdn', 'dav.example.com')")
    con.execute(f"CALL qm_http_start('{HOST}', {PORT})")
    con.execute(f"CALL qm_imap_start('{HOST}', {IMAP_PORT})")

    try:
        d = Dav()

        # ---- discovery ---------------------------------------------------
        # RFC 6764: a client given only an address resolves it through
        # .well-known. Anonymous on purpose — a client that had to authenticate
        # before it could find the endpoint could not bootstrap at all.
        for wk in ("/.well-known/caldav", "/.well-known/carddav"):
            status, headers, _ = d.go("PROPFIND", wk, auth=False)
            assert status == 301, f"{wk} returned {status}, not a redirect"
            assert headers.get("Location") == "/dav/", \
                f"{wk} points at {headers.get('Location')}"

        status, headers, _ = d.go("OPTIONS", "/dav/")
        assert status == 204, f"OPTIONS returned {status}"
        dav_header = headers.get("DAV", "")
        for token in ("calendar-access", "addressbook-access"):
            assert token in dav_header, f"OPTIONS does not advertise {token}: {dav_header}"
        # Level 2 would promise LOCK, which we deliberately do not implement.
        assert not re.search(r"\b2\b", dav_header), \
            f"DAV header claims level 2 without locking: {dav_header}"
        assert "PROPFIND" in headers.get("Allow", "")

        # ---- authentication ----------------------------------------------
        # A browser gets bounced to /login; a client must get 401 with a
        # challenge, or it has nothing to respond to.
        status, headers, _ = d.go("PROPFIND", "/dav/", ALLPROP.encode(), auth=False)
        assert status == 401, f"unauthenticated PROPFIND returned {status}"
        assert headers.get("WWW-Authenticate", "").startswith("Basic "), \
            f"no Basic challenge: {headers.get('WWW-Authenticate')}"

        bad = Dav(USER, "wrong")
        status, _, _ = bad.go("PROPFIND", "/dav/", ALLPROP.encode())
        assert status == 401, f"a wrong password returned {status}"

        # ---- principal discovery ------------------------------------------
        status, _, xml = d.propfind("/dav/", prop(["D:current-user-principal"]))
        assert status == 207, f"PROPFIND / returned {status}"
        principal = f"/dav/principals/users/{USER}/"
        assert principal in hrefs(xml), f"current-user-principal is not {principal}: {xml}"

        status, _, xml = d.propfind(
            principal, prop(["C:calendar-home-set", "CARD:addressbook-home-set"]))
        assert status == 207
        cal_home = f"/dav/calendars/{USER}/"
        card_home = f"/dav/addressbooks/{USER}/"
        assert cal_home in xml, f"no calendar-home-set: {xml}"
        assert card_home in xml, f"no addressbook-home-set: {xml}"

        # ---- the home listing ----------------------------------------------
        status, _, xml = d.propfind(
            cal_home,
            prop(["D:resourcetype", "D:displayname",
                  "C:supported-calendar-component-set", "CS:getctag", "D:sync-token"]),
            depth="1")
        assert status == 207, f"calendar home Depth 1 returned {status}"
        collections = {}
        for href, block in responses(xml):
            m = re.search(r"<D:displayname>([^<]*)</D:displayname>", block)
            if m and href != cal_home:
                collections[m.group(1)] = (href, block)
        for want in ("Calendar", "Tasks"):
            assert want in collections, f"the calendar home does not list {want}: {sorted(collections)}"
        # A tasks room is still a calendar collection — CalDAV has no other kind
        # — but it must advertise VTODO or a client will file events into it.
        assert 'name="VTODO"' in collections["Tasks"][1], \
            f"the Tasks collection does not advertise VTODO: {collections['Tasks'][1]}"
        assert 'name="VEVENT"' in collections["Calendar"][1]
        assert "<C:calendar/>" in collections["Calendar"][1]

        calendar = collections["Calendar"][0]
        tasks = collections["Tasks"][0]

        # Notes are stored as vNote for parity with WebCit, and vNote is not a
        # DAV resource type — so the Notes room must not appear here.
        assert "Notes" not in collections, "Notes should not be a DAV collection"

        status, _, xml = d.propfind(card_home, prop(["D:displayname", "D:resourcetype"]), depth="1")
        assert status == 207
        books = {re.search(r"<D:displayname>([^<]*)</D:displayname>", b).group(1): h
                 for h, b in responses(xml)
                 if re.search(r"<D:displayname>([^<]*)</D:displayname>", b) and h != card_home}
        assert "Contacts" in books, f"no Contacts address book: {sorted(books)}"
        contacts = books["Contacts"]

        # ---- PUT, GET, ETag -------------------------------------------------
        event_href = calendar + dav_name(EVENT_UID) + ".ics"
        status, headers, _ = d.go("PUT", event_href, EVENT.encode(),
                                  {"Content-Type": "text/calendar; charset=utf-8"})
        assert status == 201, f"creating an event returned {status}"
        etag = headers.get("ETag")
        assert etag, "a PUT that created an object returned no ETag"

        status, headers, body = d.go("GET", event_href)
        assert status == 200, f"GET returned {status}"
        assert headers.get("ETag") == etag, "the ETag changed without the object changing"
        assert body == EVENT, "the stored bytes are not the bytes that were PUT"
        assert "X-CUSTOM-VENDOR-FIELD:keep me exactly as written" in body, \
            "a property the server does not model was dropped on the way through"
        assert "BEGIN:VALARM" in body, "the alarm did not survive the round trip"

        # A GET of a name that decodes to no object is a 404, not a 500.
        status, _, _ = d.go("GET", calendar + "nothing-here.ics")
        assert status == 404

        # The name has to match the UID: the store keys an object by its own
        # UID, so a resource stored under a different name could never be found
        # again at the URL the client remembers.
        status, _, xml = d.go("PUT", calendar + "some-other-name.ics", EVENT.encode(),
                              {"Content-Type": "text/calendar"})
        assert status == 409, f"a name/UID mismatch returned {status}"
        assert "no-uid-conflict" in xml, f"no precondition named: {xml}"

        # Something that is not iCalendar at all.
        status, _, _ = d.go("PUT", calendar + "junk.ics", b"this is not a calendar",
                            {"Content-Type": "text/calendar"})
        assert status == 415, f"a non-iCalendar body returned {status}"

        # ---- conditional writes ---------------------------------------------
        # Create-only, against something that exists.
        status, _, _ = d.go("PUT", event_href, EVENT.encode(),
                            {"Content-Type": "text/calendar", "If-None-Match": "*"})
        assert status == 412, f"If-None-Match: * on an existing object returned {status}"

        # A stale ETag must lose.
        status, _, _ = d.go("PUT", event_href, EVENT.encode(),
                            {"Content-Type": "text/calendar", "If-Match": '"999999"'})
        assert status == 412, f"a stale If-Match returned {status}"

        # The current one wins, and moves the ETag on.
        edited = EVENT.replace("SUMMARY:Rooftop party", "SUMMARY:Rooftop party (moved)")
        status, headers, _ = d.go("PUT", event_href, edited.encode(),
                                  {"Content-Type": "text/calendar", "If-Match": etag})
        assert status == 204, f"a matching If-Match returned {status}"
        new_etag = headers.get("ETag")
        assert new_etag and new_etag != etag, "the ETag did not change on a write"

        # And the replace really replaced: one object, not two.
        status, _, xml = d.propfind(calendar, prop(["D:getetag"]), depth="1")
        assert status == 207
        objects = [h for h, _ in responses(xml) if h.endswith(".ics")]
        assert objects == [event_href], f"the collection holds {objects}, not one event"

        # ---- REPORT ----------------------------------------------------------
        # PROPFIND must not carry calendar data; a REPORT must.
        status, _, xml = d.propfind(calendar, prop(["D:getetag", "C:calendar-data"]), depth="1")
        assert "BEGIN:VCALENDAR" not in xml, "PROPFIND returned calendar data"

        multiget = ('<?xml version="1.0"?><C:calendar-multiget '
                    'xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:caldav">'
                    "<D:prop><D:getetag/><C:calendar-data/></D:prop>"
                    f"<D:href>{event_href}</D:href></C:calendar-multiget>")
        status, _, xml = d.report(calendar, multiget)
        assert status == 207, f"calendar-multiget returned {status}"
        assert "SUMMARY:Rooftop party (moved)" in xml, f"no calendar data came back: {xml[:400]}"

        # A multiget must not be a way to read another collection by naming it.
        stolen = ('<?xml version="1.0"?><C:calendar-multiget '
                  'xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:caldav">'
                  "<D:prop><C:calendar-data/></D:prop>"
                  f"<D:href>{tasks}anything.ics</D:href></C:calendar-multiget>")
        status, _, xml = d.report(calendar, stolen)
        assert status == 207
        assert "BEGIN:VCALENDAR" not in xml, "a multiget reached outside its collection"

        # A time-range query filters, and does it on the expanded occurrences.
        d.go("PUT", calendar + dav_name(FAR_UID) + ".ics", FAR_EVENT.encode(),
             {"Content-Type": "text/calendar"})
        query = ('<?xml version="1.0"?><C:calendar-query '
                 'xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:caldav">'
                 "<D:prop><D:getetag/><C:calendar-data/></D:prop>"
                 '<C:filter><C:comp-filter name="VCALENDAR">'
                 '<C:comp-filter name="VEVENT">'
                 '<C:time-range start="20260801T000000Z" end="20260901T000000Z"/>'
                 "</C:comp-filter></C:comp-filter></C:filter></C:calendar-query>")
        status, _, xml = d.report(calendar, query)
        assert status == 207, f"calendar-query returned {status}"
        assert EVENT_UID in xml, "the in-range event is missing from the query result"
        assert FAR_UID not in xml, "the out-of-range event was returned anyway"

        # ---- CardDAV ---------------------------------------------------------
        card_href = contacts + dav_name(CARD_UID) + ".vcf"
        status, _, _ = d.go("PUT", card_href, CARD.encode(), {"Content-Type": "text/vcard"})
        assert status == 201, f"creating a contact returned {status}"
        status, headers, body = d.go("GET", card_href)
        assert status == 200 and body == CARD, "the contact did not round-trip verbatim"
        assert headers.get("Content-Type", "").startswith("text/vcard")

        cardquery = ('<?xml version="1.0"?><CARD:addressbook-query '
                     'xmlns:D="DAV:" xmlns:CARD="urn:ietf:params:xml:ns:carddav">'
                     "<D:prop><D:getetag/><CARD:address-data/></D:prop>"
                     '<CARD:filter><CARD:prop-filter name="FN">'
                     '<CARD:text-match match-type="contains">lovelace</CARD:text-match>'
                     "</CARD:prop-filter></CARD:filter></CARD:addressbook-query>")
        status, _, xml = d.report(contacts, cardquery)
        assert status == 207, f"addressbook-query returned {status}"
        assert "Ada Lovelace" in xml, f"the matching contact is missing: {xml[:400]}"

        cardmiss = cardquery.replace("lovelace", "babbage")
        status, _, xml = d.report(contacts, cardmiss)
        assert "Ada Lovelace" not in xml, "a non-matching contact came back anyway"

        # ---- PROPPATCH --------------------------------------------------------
        patch = ('<?xml version="1.0"?><D:propertyupdate xmlns:D="DAV:" '
                 'xmlns:A="http://apple.com/ns/ical/"><D:set><D:prop>'
                 "<A:calendar-color>#FF0000</A:calendar-color>"
                 "</D:prop></D:set></D:propertyupdate>")
        status, _, xml = d.go("PROPPATCH", calendar, patch.encode(),
                              {"Content-Type": "application/xml"})
        assert status == 207, f"PROPPATCH returned {status}"
        assert "200 OK" in xml, f"calendar-color was not accepted: {xml}"
        status, _, xml = d.propfind(calendar, prop(["A:calendar-color"]).replace(
            "<D:prop>", '<D:prop xmlns:A="http://apple.com/ns/ical/">'))
        assert "#FF0000" in xml, f"the colour did not survive: {xml}"

        # A property we do not store must come back 403, not be silently
        # swallowed — a client told nothing keeps sending it forever.
        nope = ('<?xml version="1.0"?><D:propertyupdate xmlns:D="DAV:" '
                'xmlns:Z="http://example.com/ns/"><D:set><D:prop>'
                "<Z:invented>x</Z:invented></D:prop></D:set></D:propertyupdate>")
        status, _, xml = d.go("PROPPATCH", calendar, nope.encode(),
                              {"Content-Type": "application/xml"})
        assert status == 207 and "403 Forbidden" in xml, \
            f"an unknown property was not refused: {xml}"

        # ---- sync-collection ---------------------------------------------------
        status, _, xml = d.propfind(calendar, prop(["D:sync-token", "CS:getctag"]))
        token = between(xml, "D:sync-token")
        ctag = between(xml, "CS:getctag")
        assert token and ctag, f"no sync token or ctag: {xml}"

        def sync(tok):
            body = ('<?xml version="1.0"?><D:sync-collection xmlns:D="DAV:" '
                    'xmlns:C="urn:ietf:params:xml:ns:caldav">'
                    f"<D:sync-token>{tok}</D:sync-token>"
                    "<D:sync-level>1</D:sync-level>"
                    "<D:prop><D:getetag/></D:prop></D:sync-collection>")
            return d.report(calendar, body)

        # Nothing has happened since that token.
        status, _, xml = sync(token)
        assert status == 207, f"sync-collection returned {status}"
        assert not [h for h, _ in responses(xml) if h.endswith(".ics")], \
            f"an idle sync reported changes: {xml}"

        # A deletion has to show up, and the collection's version has to move.
        # Neither was possible before citadel_room_tombstones: DeleteMessage
        # unlinked a row and left nothing behind for a client to notice.
        status, _, _ = d.go("DELETE", event_href)
        assert status == 204, f"DELETE returned {status}"
        status, _, _ = d.go("GET", event_href)
        assert status == 404, "the event survived its own deletion"

        status, _, xml = sync(token)
        assert status == 207
        gone = [(h, b) for h, b in responses(xml) if h == event_href]
        assert gone, f"sync-collection did not report the deletion: {xml}"
        assert "404" in gone[0][1], f"the deleted resource was not reported as gone: {gone[0][1]}"

        status, _, xml = d.propfind(calendar, prop(["CS:getctag"]))
        assert between(xml, "CS:getctag") != ctag, "the ctag did not move on a delete"

        # A token we never minted means "resynchronize", not "nothing changed".
        status, _, xml = sync("urn:example:not-ours")
        assert status == 207
        assert [h for h, _ in responses(xml) if h.endswith(".ics")], \
            "an unrecognised sync token returned an empty diff instead of the collection"

        # ---- permissions --------------------------------------------------------
        # CanPost is the gate. Assert it against a real read-only room rather
        # than trusting that the DAV layer remembered to ask.
        con.execute("CALL cit_room_add('Shared Diary')")
        room = con.execute(
            "SELECT room_num FROM citadel_rooms WHERE display_name = 'Shared Diary'").fetchone()[0]
        con.execute("UPDATE citadel_rooms SET default_view = 3 WHERE room_num = ?", [room])
        # Readable by anyone, writable by nobody: QR_READONLY is 8192.
        con.execute("UPDATE citadel_rooms SET qr_flags = qr_flags | 8192 WHERE room_num = ?", [room])

        shared = f"/dav/calendars/{USER}/{room}/"
        status, _, xml = d.propfind(shared, prop(["D:current-user-privilege-set"]))
        assert status == 207, f"the shared room is not visible over DAV: {status}"
        assert "<D:read/>" in xml, f"no read privilege on a readable room: {xml}"
        assert "<D:write-content/>" not in xml, \
            f"a read-only room advertises write-content: {xml}"

        status, _, _ = d.go("PUT", shared + dav_name(EVENT_UID) + ".ics", EVENT.encode(),
                            {"Content-Type": "text/calendar"})
        assert status == 403, f"a PUT into a read-only room returned {status}"

        # One account cannot reach another's collections by editing the URL.
        status, _, _ = d.go("PROPFIND", f"/dav/calendars/{OTHER}/", ALLPROP.encode(),
                            {"Depth": "1"})
        assert status == 403, f"another user's home returned {status}"

        # ---- cross-protocol -------------------------------------------------------
        # The point of storing groupware as ordinary messages: what a phone wrote
        # is the same object the web UI edits and IMAP serves.
        im = imaplib.IMAP4(HOST, IMAP_PORT)
        im.login(USER, PASSWORD)
        typ, _ = im.select("Contacts")
        assert typ == "OK"
        typ, nums = im.search(None, "ALL")
        ids = nums[0].split()
        assert ids, "IMAP sees nothing in Contacts"
        typ, raw = im.fetch(ids[-1], "(RFC822)")
        body = next(p[1].decode("utf-8", "replace") for p in raw if isinstance(p, tuple))
        assert "text/vcard" in body, f"the contact is not a vCard part over IMAP: {body[:300]}"
        assert "Ada Lovelace" in body
        assert f"Message-ID: <{CARD_UID}>" in body, \
            "the object's UID is not its Message-ID, so the two views disagree about identity"
        im.logout()

    finally:
        con.execute("CALL qm_http_stop()")
        con.execute("CALL qm_imap_stop()")
        con.close()

    print("PASS: CalDAV and CardDAV (discovery, Basic auth, PROPFIND, REPORT, "
          "conditional writes, sync-collection, permissions, IMAP parity)")


if __name__ == "__main__":
    sys.exit(main())
