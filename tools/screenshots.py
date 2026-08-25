#!/usr/bin/env python3
"""Photograph both of QuackCit's user interfaces.

The web front-end and the telnet BBS shell are the two things a person actually
looks at, and neither had a single image in the repository. This script starts a
throwaway server, seeds it with a demo world, and captures both.

Both halves render through the *same* browser: the web pages directly, and the
terminal by replaying the session into a `pyte` screen and emitting that grid as
styled HTML. One renderer, two interfaces, no second image toolchain.

Prerequisites (host packages, not repository dependencies):

    sudo pacman -S --needed chromium ttf-dejavu ttf-liberation noto-fonts noto-fonts-emoji
    ~/venv/bin/pip install duckdb==1.5.4 playwright pyte

Playwright drives the *system* Chromium, so `playwright install` is not needed.

Run after `make release`, from anywhere:

    ~/venv/bin/python tools/screenshots.py [--out screenshots] [--keep-running]
"""
import argparse
import base64
import datetime as dt
import html
import http.cookiejar
import os
import re
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

import duckdb
import pyte
from playwright.sync_api import sync_playwright

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")

HOST = "127.0.0.1"
PORT_HTTP = 18080
PORT_HTTPS = 18443
PORT_TELNET = 12300
BASE = f"http://{HOST}:{PORT_HTTP}"
# The browser uses the TLS listener. Every page served over plaintext carries a
# "this connection is not encrypted" banner, which is correct and which would
# dominate every screenshot of a server nobody would actually run that way.
# qm_https_start with no cert generates a self-signed one in memory.
BASE_TLS = f"https://{HOST}:{PORT_HTTPS}"

USER, PASSWORD = "alice", "correct-horse"
ADMIN, ADMIN_PW = "admin", "battery-staple"

CHROMIUM = "/usr/bin/chromium"
DESKTOP = {"width": 1440, "height": 900}
MOBILE = {"width": 390, "height": 844}

EXTENSIONS = ("quackmail", "quackmail_citadel", "quackmail_http", "quackmail_telnet",
              # For /admin/acme: the ACME functions live in the spool extension.
              "quackmail_spool")

# ---------------------------------------------------------------------------
# The demo world. Everything a screenshot shows comes from here, so this is the
# part to edit when the pictures should say something different.
# ---------------------------------------------------------------------------

INBOX = [
    ("Ada Lovelace <ada@analytical.example>",
     "Re: the difference engine's spool room",
     "The room mail gateway works exactly as you described - I posted from\n"
     "Thunderbird over IMAP and it came back out of the Citadel client with\n"
     "the same message number.\n\nAda"),
    ("release-bot <ci@quackcit.example>",
     "v0.7.0 shipped: DAV scheduling, IMAP IDLE, scrypt",
     "Green across sixteen integration suites.\n\n"
     "  * CalDAV free/busy and iTIP/iMIP\n"
     "  * IMAP IDLE over citadel_sessions\n"
     "  * scrypt password hashing, legacy rows upgraded on sign-in\n"
     "  * Sieve imap4flags, variables and vacation\n"),
    ("Grace Hopper <grace@compiler.example>",
     "Nanoseconds, and a question about NNTP peering",
     "Is the peer feed (IHAVE/CHECK/TAKETHIS) on the roadmap, or is the\n"
     "Citadel network mesh the intended path for that?\n\nGrace"),
    ("lists-owner <listserv@quackcit.example>",
     "[quackcit-dev] Digest, 4 messages",
     "Four posts to the development list today. The digest is assembled by\n"
     "the spool worker, not by a cron job outside the server.\n"),
    ("Karen Sparck Jones <ksj@retrieval.example>",
     "Full-text search across rooms",
     "Searching message text decodes format_type 4 bodies through the same\n"
     "citadel::BodyText the other front-ends use, so the results agree with\n"
     "what POP3 hands over. Nicely done.\n"),
]

LOBBY = [
    ("QuackCit", "Welcome to the Lobby",
     "This is an ordinary Citadel message board. Everything posted here reads\n"
     "back over the native protocol, telnet, NNTP, IMAP and POP3 - the web\n"
     "interface is a front-end, not a separate store."),
    ("alice", "The tables are the bus",
     "Extensions never share C++ state. Cross-session state - IDLE, presence,\n"
     "instant messages - lives in DuckDB tables that every front-end reads."),
    ("grace", "Re: The tables are the bus",
     "Which is why the who-list shows telnet users and native Citadel clients\n"
     "in the same listing. They are both rows in citadel_sessions."),
]

CONTACTS = [
    dict(given="Ada", family="Lovelace", fn="Ada Lovelace", org="Analytical Engine Co.",
         title="Principal Engineer", email="ada@analytical.example",
         tel="+44 20 7946 0001", url="https://analytical.example",
         bday="1815-12-10", note="Wrote the first algorithm intended for a machine."),
    dict(given="Grace", family="Hopper", fn="Grace Hopper", org="Compiler Works",
         title="Rear Admiral", email="grace@compiler.example",
         tel="+1 202 555 0142", url="", bday="1906-12-09",
         note="Keeps a nanosecond in her desk drawer."),
    dict(given="Karen", family="Sparck Jones", fn="Karen Sparck Jones",
         org="Retrieval Lab", title="Professor", email="ksj@retrieval.example",
         tel="+44 1223 555 0177", url="", bday="", note="IDF."),
    dict(given="Radia", family="Perlman", fn="Radia Perlman", org="Spanning Tree Ltd",
         title="Network Architect", email="radia@spanning.example",
         tel="", url="", bday="", note="Algorhyme."),
]

TASKS = [
    dict(summary="Ship the ACME client", due=+9, priority="1", percent="35",
         description="http-01 over the existing qm_http listener, renewal on the\n"
                     "spool worker, hot reload through <prefix>_tls_reload()."),
    dict(summary="Wiki view: page index and history", due=+3, priority="2", percent="60",
         description="Storage stays Citadel's: euid, _HISTORY_, reverse unified diffs."),
    dict(summary="Write docs/protocols.md", due=+16, priority="5", percent="0",
         description="One coverage matrix: protocol to RFCs to implemented."),
    dict(summary="Give wildmat a step budget", due=-2, priority="1", percent="0",
         description="MatchItem recurses over every * split with no counter."),
]

NOTES = [
    ("Parity oracle", "code.citadel.org/citadel.git serves the whole tree.\n"
                      "The cgit_access=verified cookie is not optional.", "#ffff88"),
    ("Build", "GEN=ninja CMAKE_BUILD_PARALLEL_LEVEL=2 make release\n\n"
              "RAM, not cores, is the constraint.", "#aaccff"),
    ("Don't forget", "Anything under core/ is a twelve-way rebuild.\n"
                     "Batch the core edits and pay it once.", "#ffccaa"),
    ("Ports", "citadel 5040 - smtp 2525 - imap 1143\n"
              "pop3 1110 - http 8080 - telnet 2300", "#aaffaa"),
]

# The wiki. Each entry is (page name, markdown), saved in order, so the second
# revision of the front page gives the history and diff views something to show.
WIKI_ROOM = "Handbook"
WIKI = [
    ("Home",
     "# The QuackCit Handbook\n\n"
     "A Citadel server that runs *inside* DuckDB. Start with the\n"
     "[[architecture]], or jump to [[running a server]].\n\n"
     "## What this room is\n\n"
     "An ordinary Citadel room whose `default_view` is 6. Every page is a\n"
     "message keyed by its name, and the history below is a chain of reverse\n"
     "diffs in a second message - exactly what WebCit reads.\n\n"
     "> The tables are the bus.\n"),
    ("Architecture",
     "# Architecture\n\n"
     "Each protocol front-end is a loadable DuckDB extension. They never share\n"
     "C++ state; they coordinate through SQL tables.\n\n"
     "| Extension | Speaks |\n|---|---|\n"
     "| `quackmail_citadel` | the native protocol |\n"
     "| `quackmail_imap` | IMAP4rev1 |\n"
     "| `quackmail_http` | web, DAV, JMAP |\n\n"
     "```sql\nSELECT display_name, default_view FROM citadel_rooms;\n```\n"),
    # A second revision of the front page, so History and Changes have content.
    ("Home",
     "# The QuackCit Handbook\n\n"
     "A Citadel server that runs *inside* DuckDB. Start with the\n"
     "[[architecture]], or jump to [[running a server]].\n\n"
     "## What this room is\n\n"
     "An ordinary Citadel room whose `default_view` is 6. Every page is a\n"
     "message keyed by its name, and the history below is a chain of reverse\n"
     "diffs in a second message - exactly what WebCit reads.\n\n"
     "## Editing\n\n"
     "Pages are Markdown or formatted text. A link to a page nobody has\n"
     "written yet, like [[glossary]], leads to the form that creates it.\n\n"
     "> The tables are the bus.\n"),
]

EVENTS = [
    ("Sprint planning", +1, "09:30", "10:30", "Room 3", "FREQ=WEEKLY",
     "Weekly, and it keeps its local hour across a DST change."),
    ("Protocol parity review", +3, "14:00", "15:30", "Lobby", "",
     "Diff our NNTP against the oracle's captured fixtures."),
    ("Release cut", +8, "16:00", "17:00", "", "", "Tag, build, publish."),
    ("Citadel BBS anniversary", -4, "12:00", "13:00", "The Cloud", "", ""),
    ("Docs day", +12, "10:00", "12:00", "", "", "Split the README into docs/."),
]

# The pages to capture: (filename stem, path, viewport).
WEB_SHOTS = [
    ("login", "/login", DESKTOP),
    ("mail-folders", "/mail/", DESKTOP),
    ("mail-compose", "/mail/compose", DESKTOP),
    ("bbs-floors", "/bbs/", DESKTOP),
    ("bbs-who", "/bbs/who", DESKTOP),
    ("search", "/search?q=citadel", DESKTOP),
    ("prefs", "/prefs", DESKTOP),
    ("prefs-sieve", "/prefs/sieve", DESKTOP),
]

# Captured in a second browser context, signed in as the aide.
ADMIN_SHOTS = [
    ("admin", "/admin/"),
    ("admin-users", "/admin/users"),
    ("admin-rooms", "/admin/rooms"),
    ("admin-dkim", "/admin/dkim"),
    ("admin-acme", "/admin/acme"),
    ("admin-queue", "/admin/queue"),
]

# Terminal sessions: (stem, caption, keystrokes). LF only - CRLF through the
# telnet negotiation puts CR NUL CR LF on the wire and eats the next prompt.
# Expert mode (US_EXPERT) hides the citadel.rc menu, and it *persists* in
# citadel_users.flags - which is the point of it, and which makes toggling it
# with <X> between scripts order-dependent. So each entry states the mode it
# wants and the driver sets the bit before connecting.
US_EXPERT = 32
US_COLOR = 16384   # the BBS emits ANSI only for a user who asked for it

TEXT_SHOTS = [
    ("login", "Signing in: the banner, the Lobby, and the citadel.rc menu",
     False, [USER, PASSWORD]),
    ("known-rooms", "<K>nown rooms, in expert mode",
     True, [USER, PASSWORD, "K"]),
    ("read-new", "<.G>oto a room and read <N>ew messages",
     True, [USER, PASSWORD, ".G", "Lobby", "N"]),
    ("who", "<.W>holist - telnet and native Citadel sessions in one listing",
     True, [USER, PASSWORD, ".W"]),
    ("enter", "<E>nter a message: subject, body, and '.' to finish",
     True, [USER, PASSWORD, "E", "Posted from the BBS shell",
            "This went in as an ordinary format_type 0 Citadel message, so it",
            "reads back over IMAP, POP3, NNTP and the web interface unchanged.",
            "."]),
]


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


# ---------------------------------------------------------------------------
# A minimal HTTP client, used only to seed. The captures themselves go through
# the browser, which signs in the same way a person does.
# ---------------------------------------------------------------------------

class NoRedirect(urllib.request.HTTPRedirectHandler):
    """Follow nothing: a 303 is the success signal, not something to chase."""

    def redirect_request(self, *args, **kwargs):
        return None


class Client:
    def __init__(self):
        self.jar = http.cookiejar.CookieJar()
        self.op = urllib.request.build_opener(
            NoRedirect(), urllib.request.HTTPCookieProcessor(self.jar))

    def get(self, path):
        try:
            with self.op.open(BASE + path, timeout=15) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")

    def post(self, path, fields):
        data = urllib.parse.urlencode(fields).encode()
        req = urllib.request.Request(BASE + path, data=data)
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
        try:
            with self.op.open(req, timeout=30) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")


def csrf(page):
    m = re.search(r'name="_csrf" value="([^"]+)"', page)
    if not m:
        raise SystemExit("no CSRF token on the page - did sign-in fail?")
    return m.group(1)


def room_href(page, label):
    m = re.search(r'href="(/bbs/room/\d+)"[^>]*>' + re.escape(label), page)
    if not m:
        m = re.search(r'href="(/bbs/room/\d+)"[^>]*>[^<]*' + re.escape(label), page)
    if not m:
        raise SystemExit(f"no link to a room called {label!r}")
    return m.group(1)


# ---------------------------------------------------------------------------
# Seeding
# ---------------------------------------------------------------------------

def rfc822(sender, subject, body):
    when = dt.datetime.now(dt.timezone.utc).strftime("%a, %d %b %Y %H:%M:%S +0000")
    return (f"From: {sender}\r\nTo: {USER}@quackcit.example\r\n"
            f"Subject: {subject}\r\nDate: {when}\r\n"
            f"Content-Type: text/plain; charset=utf-8\r\n\r\n"
            + body.replace("\n", "\r\n") + "\r\n")


def deliver(con, user, sender, subject, body):
    """File a message into a user's Mail room, as test_http.py's deliver() does."""
    con.execute(
        """
        INSERT INTO citadel_messages
            (msgnum, author, recipient, msgtime, subject, format_type, raw)
        VALUES (nextval('citadel_msg_seq'), ?, ?, epoch(now())::BIGINT, ?, 4, ?)
        """,
        [sender, user, subject, rfc822(sender, subject, body).encode()],
    )
    con.execute(
        """
        INSERT INTO citadel_room_msgs (room_num, msgnum)
        SELECT r.room_num, (SELECT max(msgnum) FROM citadel_messages)
          FROM citadel_rooms r
          JOIN citadel_users u ON u.usernum = r.mailbox_owner
         WHERE u.username = ? AND r.display_name = 'Mail'
        """,
        [user],
    )


def post_to_lobby(con, author, subject, body):
    con.execute(
        """
        INSERT INTO citadel_messages
            (msgnum, author, msgtime, subject, format_type, raw)
        VALUES (nextval('citadel_msg_seq'), ?, epoch(now())::BIGINT, ?, 0, ?)
        """,
        [author, subject, body.encode()],
    )
    con.execute(
        """
        INSERT INTO citadel_room_msgs (room_num, msgnum)
        SELECT room_num, (SELECT max(msgnum) FROM citadel_messages)
          FROM citadel_rooms WHERE display_name = 'Lobby'
        """
    )


def seed_groupware(c):
    """Create the groupware objects through the real forms.

    Hand-rolling vCard and iCalendar here would be a second writer for objects
    the server already knows how to build, and the first thing a malformed one
    breaks is the picture it was supposed to appear in.
    """
    _, home = c.get("/mail/")
    rooms = {label: room_href(home, label)
             for label in ("Calendar", "Contacts", "Tasks", "Notes")}

    def save(room, fields):
        _, form = c.get(rooms[room] + "/item/new")
        fields = dict(fields, _csrf=csrf(form))
        status, page = c.post(rooms[room] + "/item/save", fields)
        if status not in (200, 303):
            raise SystemExit(f"saving into {room} returned {status}\n{page[:400]}")

    for card in CONTACTS:
        save("Contacts", card)

    today = dt.date.today()
    for summary, offset, start, end, where, rrule, desc in EVENTS:
        day = (today + dt.timedelta(days=offset)).isoformat()
        save("Calendar", dict(summary=summary, date=day, time=start,
                              end_date=day, end_time=end, location=where,
                              rrule=rrule, description=desc))

    for t in TASKS:
        save("Tasks", dict(summary=t["summary"],
                           due=(today + dt.timedelta(days=t["due"])).isoformat(),
                           priority=t["priority"], percent=t["percent"],
                           description=t["description"]))

    for title, text, colour in NOTES:
        save("Notes", dict(summary=title, body=text, color=colour))

    return rooms


def seed_wiki(c, con):
    """A wiki room with two pages, one of them edited twice."""
    con.execute("CALL cit_room_add(?)", [WIKI_ROOM])
    con.execute("UPDATE citadel_rooms SET default_view = 6 WHERE display_name = ?", [WIKI_ROOM])
    room_num = con.execute(
        "SELECT room_num FROM citadel_rooms WHERE display_name = ?", [WIKI_ROOM]).fetchone()[0]
    room = f"/bbs/room/{room_num}"
    for name, text in WIKI:
        _, form = c.get(room + "/wiki/edit")
        status, page = c.post(room + "/wiki/save", {
            "_csrf": csrf(form), "page": name, "format": "text/x-markdown", "body": text})
        if status not in (200, 303):
            raise SystemExit(f"saving the wiki page {name!r} returned {status}\n{page[:400]}")
    return room


def start_server(con):
    for name in EXTENSIONS:
        con.execute(f"LOAD '{ext(name)}'")
    # Warm the catalog: store::EnsureSchema runs from a table function's init,
    # so citadel_users and friends do not exist after LOAD alone.
    con.execute("SELECT count(*) FROM qm_status()").fetchall()

    for user, pw, ax in ((USER, PASSWORD, 4), (ADMIN, ADMIN_PW, 6)):
        con.execute("CALL qm_user_add(?, ?)", [user, pw])
        con.execute(
            "INSERT INTO citadel_users (username, usernum, axlevel) "
            "VALUES (?, nextval('citadel_user_seq'), ?)", [user, ax])

    con.execute("CALL qm_config_set('c_fqdn', 'quackcit.example')")
    con.execute("CALL qm_config_set('c_humannode', 'QuackCit BBS')")
    # A dev box has no certificate, so the redirect has to be off or the
    # plaintext listener 301s every request to https://c_fqdn.
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    # /admin is gated twice over, and both gates are on by default.
    con.execute("CALL qm_config_set('qm_web_admin_enabled', '1')")
    con.execute("CALL qm_config_set('qm_web_admin_require_tls', '0')")

    note = con.execute("SELECT note FROM qm_http_start(?, ?)",
                       [HOST, PORT_HTTP]).fetchone()[0]
    if note != "started":
        raise SystemExit(f"qm_http did not start: {note}")
    note = con.execute(
        "SELECT note FROM qm_https_start(?, ?, implicit_tls => true)",
        [HOST, PORT_HTTPS]).fetchone()[0]
    if note != "started":
        raise SystemExit(f"qm_https did not start: {note}")
    note = con.execute("SELECT note FROM qm_telnet_start(?, ?)",
                       [HOST, PORT_TELNET]).fetchone()[0]
    if note != "started":
        raise SystemExit(f"qm_telnet did not start: {note}")
    time.sleep(0.5)


# ---------------------------------------------------------------------------
# The telnet half: drive a session, replay it into a terminal, render as HTML.
# ---------------------------------------------------------------------------

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
OPT_ECHO, OPT_SGA, OPT_TTYPE, OPT_NAWS = 1, 3, 24, 31

COLS, ROWS = 80, 30


def telnet_session(keystrokes, settle=0.7):
    """Run a scripted BBS session and return the transcript.

    NAWS matters: core/src/telnet.cpp only engages the screenful pager when the
    peer negotiates a window size, so without it the capture is an unpaged
    stream that looks nothing like the real thing.

    Echo is *local*: we decline the server's WILL ECHO and splice each command
    into the transcript ourselves, which is what a telnet client in line mode
    does. Accepting server-side echo makes the BBS read a spurious empty
    command after every real one ("Unknown command. Press ? for help."), which
    is a bug in the telnet front-end rather than in this driver - see TODO.md.
    """
    s = socket.create_connection((HOST, PORT_TELNET), timeout=10)
    s.settimeout(0.6)
    out = bytearray()

    def drain():
        try:
            while True:
                b = s.recv(4096)
                if not b:
                    return False
                out.extend(negotiate(s, b))
        except socket.timeout:
            return True

    def negotiate(sock, data):
        """Answer IAC negotiation; return the data with commands stripped."""
        clean, i = bytearray(), 0
        while i < len(data):
            if data[i] != IAC:
                clean.append(data[i])
                i += 1
                continue
            if i + 1 >= len(data):
                break
            cmd = data[i + 1]
            if cmd in (DO, DONT, WILL, WONT):
                if i + 2 >= len(data):
                    break
                opt = data[i + 2]
                if cmd == DO and opt == OPT_NAWS:
                    sock.sendall(bytes([IAC, WILL, OPT_NAWS]))
                    sock.sendall(bytes([IAC, SB, OPT_NAWS,
                                        0, COLS, 0, ROWS, IAC, SE]))
                elif cmd == DO and opt == OPT_SGA:
                    sock.sendall(bytes([IAC, WILL, opt]))
                elif cmd == DO and opt == OPT_TTYPE:
                    # Refuse rather than accept: agreeing to WILL TTYPE invites
                    # a subnegotiation this driver has no answer for.
                    sock.sendall(bytes([IAC, WONT, opt]))
                elif cmd == DO:
                    sock.sendall(bytes([IAC, WONT, opt]))
                elif cmd == WILL and opt == OPT_ECHO:
                    # Decline server-side echo and echo locally instead (see
                    # the note in this file's telnet_session). A real client in
                    # line mode does exactly this.
                    sock.sendall(bytes([IAC, DONT, opt]))
                elif cmd == WILL:
                    sock.sendall(bytes([IAC, DO, opt]))
                i += 3
                continue
            if cmd == SB:
                end = data.find(bytes([IAC, SE]), i)
                i = len(data) if end < 0 else end + 2
                continue
            i += 2
        return clean

    def answer_pager():
        """Clear any <more> prompts before the next command line goes out.

        The pager reads a single character (core/src/telnet.cpp MorePrompt),
        not a line. Left unanswered it swallows the first byte of whatever is
        sent next, so the command after a paged listing silently becomes an
        empty line - which the BBS quite correctly calls an unknown command.
        """
        for _ in range(20):
            if not out[-8:].endswith(b"<more> "):
                return
            s.sendall(b" ")
            time.sleep(settle)
            if not drain():
                return

    time.sleep(settle)
    drain()
    answer_pager()
    for key in keystrokes:
        # Local echo, masked at a password prompt exactly as the server would
        # have masked it - a screenshot must not publish the demo password.
        shown = "*" * len(key) if out.rstrip().endswith(b"Password:") else key
        out.extend(shown.encode() + b"\r\n")
        s.sendall(key.encode() + b"\n")
        time.sleep(settle)
        if not drain():
            break
        answer_pager()
    # No <T>erminate: it prints a farewell and one more copy of the menu, and
    # the last thing a reader should see is the prompt, not "Goodbye".
    s.close()
    return bytes(out)


PALETTE = {
    "black": "#22262b", "red": "#e06c60", "green": "#8bc98b",
    "brown": "#d9b36a", "yellow": "#d9b36a", "blue": "#7aa6da",
    "magenta": "#c98fd0", "cyan": "#78c6c6", "white": "#d6d2c8",
    "default": "#d6d2c8",
}
BRIGHT = {
    "black": "#6a7079", "red": "#ff8a80", "green": "#b4e3a8",
    "brown": "#ffd479", "yellow": "#ffd479", "blue": "#a5c8ff",
    "magenta": "#e6aaf0", "cyan": "#9fe6e6", "white": "#ffffff",
    "default": "#ffffff",
}


def terminal_html(raw, caption):
    """Replay bytes into a pyte screen and emit it as one styled <pre>."""
    screen = pyte.Screen(COLS, ROWS)
    stream = pyte.Stream(screen)
    stream.feed(raw.decode("utf-8", "replace"))

    lines = []
    for row in range(ROWS):
        chars, spans, run, style = screen.buffer[row], [], "", None

        def flush():
            if run:
                spans.append(f'<span style="{style}">{html.escape(run)}</span>')

        for col in range(COLS):
            ch = chars[col]
            table = BRIGHT if ch.bold else PALETTE
            colour = table.get(ch.fg, "#" + ch.fg if ch.fg != "default" else table["default"])
            css = f"color:{colour}"
            if ch.bg != "default":
                css += f";background:#{ch.bg}"
            if ch.bold:
                css += ";font-weight:600"
            if css != style:
                flush()
                run, style = "", css
            run += ch.data or " "
        flush()
        lines.append("".join(spans).rstrip() or "&nbsp;")

    while len(lines) > 4 and lines[-1] == "&nbsp;":
        lines.pop()

    return f"""<!doctype html><meta charset="utf-8"><style>
  html {{ background:#0e1013; }}
  body {{ margin:0; padding:20px; background:#0e1013;
         font:15px/1.35 "DejaVu Sans Mono","Liberation Mono",monospace; }}
  .term {{ background:#181b20; border-radius:8px; padding:16px 18px;
          box-shadow:0 8px 30px rgba(0,0,0,.5); display:inline-block; }}
  .cap {{ color:#8a919b; font:12px/1.6 "DejaVu Sans",sans-serif;
         padding:0 0 8px 2px; }}
  pre {{ margin:0; color:#d6d2c8; white-space:pre; }}
</style><div class="cap">{html.escape(caption)}</div>
<div class="term"><pre>{chr(10).join(lines)}</pre></div>"""


# ---------------------------------------------------------------------------
# Capture
# ---------------------------------------------------------------------------

def sign_in(page, user, password):
    page.goto(BASE_TLS + "/login", wait_until="networkidle")
    page.fill('input[name="username"]', user)
    page.fill('input[name="password"]', password)
    # The forms use a bare <button>, which submits by default.
    page.click('form[action="/login"] button')
    page.wait_for_load_state("networkidle")


def set_theme(page, theme):
    """Set the colour theme the way a person does - through the real form."""
    page.goto(BASE_TLS + "/prefs", wait_until="networkidle")
    page.select_option('select[name="theme"]', theme)
    page.click('form[action="/prefs/settings"] button')
    page.wait_for_load_state("networkidle")


def shoot(page, out, stem, path, viewport):
    page.set_viewport_size(viewport)
    page.goto(BASE_TLS + path, wait_until="networkidle")
    dest = os.path.join(out, stem + ".png")
    page.screenshot(path=dest, full_page=True)
    print(f"  {stem}.png  <- {path}")
    return stem


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(REPO, "screenshots"))
    ap.add_argument("--keep-running", action="store_true",
                    help="leave the demo server up after capturing")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    if not os.path.exists(ext("quackmail_http")):
        raise SystemExit(f"no extensions under {EXT_DIR} - run `make release` first")
    if not os.path.exists(CHROMIUM):
        raise SystemExit(f"{CHROMIUM} is missing - see this file's docstring")

    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    print("starting the demo server")
    start_server(con)

    print("seeding")
    # Sign in first: EnsureUserRooms runs on use, so Mail/Calendar/Contacts/
    # Tasks/Notes do not exist for a freshly added account until then, and
    # delivering into a Mail room that is not there yet silently files nothing.
    c = Client()
    _, page = c.get("/login")
    status, _ = c.post("/login", {"username": USER, "password": PASSWORD,
                                  "_csrf": csrf(page)})
    if status != 303:
        raise SystemExit(f"seed sign-in returned {status}")

    for sender, subject, body in INBOX:
        deliver(con, USER, sender, subject, body)
    for author, subject, body in LOBBY:
        post_to_lobby(con, author, subject, body)
    rooms = seed_groupware(c)
    wiki_room = seed_wiki(c, con)

    # The Lobby by number: a Citadel room name may contain '/', so rooms are
    # addressed by number everywhere, and the sidebar only lists rooms with
    # something new in them.
    lobby = "/bbs/room/%d" % con.execute(
        "SELECT room_num FROM citadel_rooms WHERE display_name = 'Lobby'").fetchone()[0]
    # The inbox is the user's own Mail room, which renders as a mailbox rather
    # than a message board because its default_view is VIEW_MAILBOX.
    inbox = "/bbs/room/%d" % con.execute(
        """
        SELECT r.room_num FROM citadel_rooms r
          JOIN citadel_users u ON u.usernum = r.mailbox_owner
         WHERE u.username = ? AND r.display_name = 'Mail'
        """, [USER]).fetchone()[0]

    captured = []
    with sync_playwright() as p:
        browser = p.chromium.launch(executable_path=CHROMIUM, args=["--no-sandbox"])
        ctx = browser.new_context(viewport=DESKTOP, device_scale_factor=2,
                                  ignore_https_errors=True)
        page = ctx.new_page()

        print("capturing the web interface")
        shoot(page, args.out, "web-login", "/login", DESKTOP)
        sign_in(page, USER, PASSWORD)

        for stem, path, viewport in WEB_SHOTS:
            if stem == "login":
                continue
            captured.append((shoot(page, args.out, "web-" + stem, path, viewport), path))

        # The room views, by number, since a Citadel room name may contain '/'.
        for label, href in rooms.items():
            captured.append((shoot(page, args.out, "web-room-" + label.lower(),
                                   href, DESKTOP), href))
        captured.append((shoot(page, args.out, "web-bbs-room", lobby, DESKTOP), lobby))

        # The wiki: the page itself, the index, the history and a diff.
        captured.append((shoot(page, args.out, "web-wiki", wiki_room + "/wiki?page=home",
                               DESKTOP), wiki_room + "/wiki?page=home"))
        captured.append((shoot(page, args.out, "web-wiki-index", wiki_room, DESKTOP), wiki_room))
        captured.append((shoot(page, args.out, "web-wiki-history",
                               wiki_room + "/wiki/history?page=home", DESKTOP),
                         wiki_room + "/wiki/history?page=home"))
        _, hist = c.get(wiki_room + "/wiki/history?page=home")
        m = re.search(r"/wiki/diff\?page=home&(?:amp;|#38;)?rev=(\d+)", html.unescape(hist))
        if m:
            path = f"{wiki_room}/wiki/diff?page=home&rev={m.group(1)}"
            captured.append((shoot(page, args.out, "web-wiki-diff", path, DESKTOP), path))

        captured.append((shoot(page, args.out, "web-mail-inbox", inbox, DESKTOP), inbox))

        # One message in the read pane.
        _, listing = c.get(inbox)
        m = re.search(r'href="(/bbs/room/\d+/msg/\d+)"', listing)
        if m:
            captured.append((shoot(page, args.out, "web-mail-read", m.group(1), DESKTOP),
                             m.group(1)))
        else:
            print("  (no message link in the inbox listing - skipping the read pane)")

        # Themes. The theme is a stored preference, not a query parameter.
        for theme in ("dark", "amber"):
            set_theme(page, theme)
            captured.append((shoot(page, args.out, f"web-mail-inbox-{theme}",
                                   inbox, DESKTOP), inbox))
            captured.append((shoot(page, args.out, f"web-bbs-room-{theme}",
                                   lobby, DESKTOP), lobby))
        set_theme(page, "auto")

        # Narrow: the sidebar is a CSS-only toggle, so it is worth showing.
        for stem, path in (("mail-inbox", inbox), ("bbs-room", lobby)):
            captured.append((shoot(page, args.out, f"web-mobile-{stem}", path, MOBILE),
                             path + "  (390x844)"))

        # The admin console runs as an aide.
        ctx2 = browser.new_context(viewport=DESKTOP, device_scale_factor=2,
                                   ignore_https_errors=True)
        page2 = ctx2.new_page()
        sign_in(page2, ADMIN, ADMIN_PW)
        for stem, path in ADMIN_SHOTS:
            captured.append((shoot(page2, args.out, "web-" + stem, path, DESKTOP), path))
        ctx2.close()

        print("capturing the BBS shell")
        term = ctx.new_page()
        term_shots = []
        for stem, caption, expert, keys in TEXT_SHOTS:
            con.execute(
                "UPDATE citadel_users SET flags = "
                "  (CASE WHEN ? THEN flags | ? ELSE flags & ~(?::BIGINT) END) | ? "
                " WHERE username = ?",
                [expert, US_EXPERT, US_EXPERT, US_COLOR, USER])
            raw = telnet_session(keys)
            term.set_content(terminal_html(raw, caption), wait_until="load")
            dest = os.path.join(args.out, f"text-{stem}.png")
            box = term.locator(".term").bounding_box()
            term.set_viewport_size({"width": int(box["width"]) + 40,
                                    "height": int(box["height"]) + 60})
            term.screenshot(path=dest, full_page=True)
            print(f"  text-{stem}.png  <- {' '.join(k or '<enter>' for k in keys[2:]) or 'sign in'}")
            term_shots.append((f"text-{stem}", caption))

        browser.close()

    write_index(args.out, captured, term_shots)

    if args.keep_running:
        print(f"\nserver still up: {BASE}  (sign in as {USER}/{PASSWORD})")
        print("press ctrl-c to stop")
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            pass
    con.execute("CALL qm_http_stop()")
    con.execute("CALL qm_https_stop()")
    con.execute("CALL qm_telnet_stop()")
    con.close()
    print(f"\n{len(captured) + len(term_shots)} images in {args.out}")


def write_index(out, web, term):
    lines = [
        "# Screenshots",
        "",
        "Generated by [`tools/screenshots.py`](../tools/screenshots.py) against a",
        "throwaway server seeded with a demo world. Regenerate them with:",
        "",
        "```bash",
        "~/venv/bin/python tools/screenshots.py",
        "```",
        "",
        "## The web interface",
        "",
    ]
    for stem, path in web:
        lines += [f"### `{path}`", "", f"![{stem}]({stem}.png)", ""]
    lines += ["## The BBS shell (telnet)", "",
              "`quackmail_telnet` *is* the Citadel text client, running server-side,",
              "so a plain `telnet` gets the BBS. These are real sessions replayed",
              "through a terminal emulator.", ""]
    for stem, caption in term:
        lines += [f"### {caption}", "", f"![{stem}]({stem}.png)", ""]
    with open(os.path.join(out, "README.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


if __name__ == "__main__":
    main()
