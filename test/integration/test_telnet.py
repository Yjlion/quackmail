#!/usr/bin/env python3
"""End-to-end test for the telnet BBS shell (quackmail_telnet).

Drives a full session the way a user would: register, land in the Lobby, enter a
message, read it back, list known rooms, page another user, check the who-list,
and terminate. Also verifies that the message really landed in the Citadel store
(so the BBS shell and the other front-ends share one room store) and that the
implicit-TLS telnets listener works.

Requires: pip install duckdb==1.5.4
Run after `make` so the loadable extensions exist under build/release/extension.

Driving the shell by hand with the real client works too, but feed it LF-only
input: piping CRLF text through `telnet` puts CR NUL CR LF on the wire (two line
endings per line), which empties the following prompt.

    (printf "leo\\nleo\\nK\\nT\\n"; sleep 4) | telnet 127.0.0.1 2300
"""
import base64
import http.cookiejar
import os
import re
import socket
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 12300
PORT_TLS = 12992
WEB_PORT = 12993


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class Bbs:
    """A dumb line-oriented client: send keystrokes, collect everything back."""

    def __init__(self, port, use_tls=False):
        s = socket.create_connection((HOST, port), timeout=5)
        if use_tls:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            s = ctx.wrap_socket(s)
        s.settimeout(1.5)
        self.s = s
        self.log = ""
        self.read()

    def read(self):
        try:
            while True:
                d = self.s.recv(8192)
                if not d:
                    break
                # Drop telnet IAC negotiation so assertions see plain text.
                text = bytes(b for b in d if b < 128).decode("utf-8", "replace")
                self.log += text
        except OSError:
            pass
        return self.log

    def send(self, text):
        self.s.sendall(text.encode() + b"\r\n")
        time.sleep(0.25)
        return self.read()

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    con.execute(f"LOAD '{ext('quackmail_citadel')}'")
    con.execute(f"LOAD '{ext('quackmail_telnet')}'")
    con.execute("CALL qm_user_add('bbsuser', 'secret')")
    con.execute("CALL qm_user_add('pageme', 'secret')")
    # An aide, for the .Admin commands. bbsuser stays at axlevel 4 on purpose so
    # the refusals below are real; axlevel is read at login, so it cannot be
    # raised mid-session.
    con.execute("CALL qm_user_add('bbsaide', 'secret')")
    # The citadel_users row is created at first login, so an axlevel has to be
    # inserted rather than updated -- the same way test_http.py seeds its admin.
    con.execute("INSERT INTO citadel_users (username, usernum, axlevel) "
                "VALUES ('bbsaide', nextval('citadel_user_seq'), 6)")

    for call in (
        f"SELECT note FROM qm_telnet_start('{HOST}', {PORT})",
        f"SELECT note FROM qm_telnets_start('{HOST}', {PORT_TLS}, implicit_tls=>true)",
    ):
        note = con.execute(call).fetchone()[0]
        assert note == "started", f"listener did not start: {note}"
    time.sleep(0.3)

    try:
        c = Bbs(PORT)
        assert "QuackCit BBS" in c.log, c.log
        assert "Enter your name" in c.log, c.log

        c.send("bbsuser")
        assert "Password" in c.log, c.log
        out = c.send("secret")
        assert "Welcome, bbsuser." in out, out
        # The session lands in the Lobby and the menu is shown.
        assert "Lobby>" in out, out
        assert "<K>nown rooms" in out, out

        # Enter a message in the Lobby.
        c.send("E")
        assert "Subject:" in c.log, c.log
        c.send("Hello from telnet")
        assert "End with a '.'" in c.log, c.log
        c.send("first line")
        c.send("second line")
        out = c.send(".")
        assert "Message saved." in out, out

        # Read it back with <F>orward read.
        before = len(c.log)
        out = c.send("F")[before:]
        assert "Hello from telnet" in out, out
        assert "first line" in out and "second line" in out, out
        assert "from bbsuser" in out, out

        # <K>nown rooms lists the Lobby and the user's own Mail room.
        before = len(c.log)
        out = c.send("K")[before:]
        assert "Lobby>" in out, out
        assert "Mail>" in out, out

        # <W>ho is online shows this session.
        before = len(c.log)
        out = c.send("W")[before:]
        assert "bbsuser" in out and "Telnet session" in out, out

        # <P>age another user queues an instant message.
        c.send("P")
        c.send("pageme")
        out = c.send("ping from the BBS")
        assert "Message sent." in out, out

        # A dot command: .Goto Lobby
        before = len(c.log)
        out = c.send(".Goto Lobby")[before:]
        assert "Lobby>" in out, out

        # <X> hides the menu. Wording matches the real text client
        # (textclient/user_functions.c: "Expert mode now ON").
        before = len(c.log)
        out = c.send("X")[before:]
        assert "Expert mode now ON" in out, out
        before = len(c.log)
        out = c.send("K")[before:]
        assert "<K>nown rooms" not in out, "menu should be hidden in expert mode"

        # Expert mode is a persisted US_EXPERT bit, not a per-connection flag.
        assert con.execute(
            "SELECT flags & 32 FROM citadel_users WHERE username = 'bbsuser'"
        ).fetchone()[0] == 32, "expert mode was not persisted"
        before = len(c.log)
        out = c.send("X")[before:]
        assert "Expert mode now OFF" in out, out

        # --- floors ---------------------------------------------------------
        # ;C configures floor mode; the answer persists as US_FLOORS.
        c.send(";C")
        out = c.send("yes")
        assert "Floor mode now ON" in out, out
        assert con.execute(
            "SELECT flags & 8192 FROM citadel_users WHERE username = 'bbsuser'"
        ).fetchone()[0] == 8192, "floor mode was not persisted"
        # With floor mode on, <K>nown groups by floor.
        before = len(c.log)
        out = c.send("K")[before:]
        assert "Main Floor" in out, out

        # --- skip vs goto ----------------------------------------------------
        # Post a second message so the Lobby has something unread, then confirm
        # <S>kip leaves it unread where <G>oto would not. This is the behaviour
        # the two commands used to share.
        c.send(".G Lobby")
        con.execute(
            "INSERT INTO citadel_messages (msgnum, author, msgtime, subject, format_type, raw) "
            "VALUES (nextval('citadel_msg_seq'), 'someone', epoch(now())::BIGINT, 'unread one', 0, 'body')"
        )
        con.execute(
            "INSERT INTO citadel_room_msgs (room_num, msgnum) "
            "SELECT 0, max(msgnum) FROM citadel_messages"
        )

        def lobby_unread():
            return con.execute(
                "SELECT count(*) FROM citadel_room_msgs rm WHERE rm.room_num = 0 AND rm.msgnum > "
                "coalesce((SELECT last_read FROM citadel_room_state "
                "          WHERE username = 'bbsuser' AND room_num = 0), 0)"
            ).fetchone()[0]

        assert lobby_unread() > 0, "fixture message is not unread"
        c.send("S")
        assert lobby_unread() > 0, "<S>kip marked the room read; it must not"
        c.send(".G Lobby")
        c.send("G")
        assert lobby_unread() == 0, "<G>oto did not mark the room read"

        # --- zap -------------------------------------------------------------
        c.send(".G Lobby")
        c.send("Z")  # asks for confirmation first
        out = c.send("yes")
        assert "cannot zap the lobby" in out.lower(), out
        con.execute("CALL cit_room_add('Zappable')")
        c.send(".G Zappable")
        c.send("Z")
        out = c.send("yes")
        assert "forgotten" in out.lower(), out
        assert con.execute(
            "SELECT flags & 1 FROM citadel_room_state cs JOIN citadel_rooms r USING (room_num) "
            "WHERE cs.username = 'bbsuser' AND r.display_name = 'Zappable'"
        ).fetchone()[0] == 1, "the room was not marked zapped"
        # It drops out of <K>nown and turns up under .Known Zapped.
        before = len(c.log)
        out = c.send("K")[before:]
        assert "Zappable" not in out, "a forgotten room is still listed"
        before = len(c.log)
        out = c.send(".KZ")[before:]
        assert "Zappable" in out, out
        # Going to it by name brings it back, as Citadel does.
        c.send(".G Zappable")
        assert con.execute(
            "SELECT flags & 1 FROM citadel_room_state cs JOIN citadel_rooms r USING (room_num) "
            "WHERE cs.username = 'bbsuser' AND r.display_name = 'Zappable'"
        ).fetchone()[0] == 0, "visiting a forgotten room did not restore it"

        # --- file areas ------------------------------------------------------
        # QR_DIRECTORY|QR_UPLOAD|QR_DOWNLOAD|QR_VISDIR = 32|64|128|256. These
        # four bits have existed in citadel_store.hpp since the beginning and
        # nothing ever read them; a directory room's only visible effect was the
        # ']' on the prompt.
        con.execute("CALL cit_room_add('Uploads')")
        con.execute("UPDATE citadel_rooms SET qr_flags = qr_flags | 32 | 64 | 128 | 256 "
                    "WHERE display_name = 'Uploads'")
        c.send(".G Uploads")

        before = len(c.log)
        out = c.send(".RF")[before:]
        assert "no files" in out.lower(), out

        # Upload by pasting base64, which is the path that needs no transfer
        # protocol at the client end.
        c.send(".EF")
        c.send("notes.txt")
        c.send("Some notes")
        c.send("B")
        c.send(base64.b64encode(b"hello from telnet\n").decode())
        out = c.send(".")
        assert "stored notes.txt" in out.lower(), out

        # A file is a message: that is the whole storage design, and it is what
        # makes quotas, tombstones and the room ACL apply without being told
        # about files.
        rows = con.execute(
            "SELECT count(*) FROM citadel_messages m JOIN citadel_room_msgs rm USING (msgnum) "
            "JOIN citadel_rooms r USING (room_num) "
            "WHERE r.display_name = 'Uploads' AND m.euid = 'file/notes.txt'").fetchone()[0]
        assert rows == 1, f"the upload did not land as a message ({rows})"

        before = len(c.log)
        out = c.send(".RF")[before:]
        assert "notes.txt" in out, out
        assert "Some notes" in out, "the description is missing from the listing"

        # Typing it out, which is what a reader usually wants for a text file.
        c.send(".RFG")
        c.send("notes.txt")
        out = c.send("T")
        assert "hello from telnet" in out, out

        # And base64 back out again, byte for byte.
        c.send(".RFG")
        c.send("notes.txt")
        out = c.send("B")
        m = re.search(r"---- begin notes\.txt ----(.*?)---- end notes\.txt ----", out, re.S)
        assert m, f"no base64 block in the reply: {out[:300]}"
        assert base64.b64decode("".join(m.group(1).split())) == b"hello from telnet\n", \
            "the file did not round-trip through base64"

        # .Admin File is aide-only, like the rest of the .A family. bbsuser is
        # axlevel 4, so it refuses -- and that refusal is the first half of the
        # assertion, because a gate only tested from the passing side is not
        # tested.
        before = len(c.log)
        out = c.send(".AFD")[before:]
        assert "Higher access required" in out, out
        before = len(c.log)
        out = c.send(".RF")[before:]
        assert "notes.txt" in out, "the refused delete removed the file anyway"

        # An aide, on a second connection, can do both -- which is what proves
        # the refusal above was the gate rather than something else about the
        # command. A second session also exercises the file area being shared
        # rather than per-connection state.
        a = Bbs(PORT)
        a.send("bbsaide")
        a.send("secret")
        a.send(".G Uploads")
        a.send(".AFE")
        a.send("notes.txt")
        out = a.send("Rewritten description")
        assert "description updated" in out.lower(), out[-800:]

        before = len(c.log)
        out = c.send(".RF")[before:]
        assert "Rewritten description" in out, "the other session did not see the new description"

        a.send(".AFD")
        out = a.send("notes.txt")
        assert "deleted notes.txt" in out.lower(), out
        a.send("T")
        a.close()

        before = len(c.log)
        out = c.send(".RF")[before:]
        assert "no files" in out.lower(), out

        # A room without the bits is not a file area at all.
        c.send(".G Lobby")
        before = len(c.log)
        out = c.send(".RF")[before:]
        assert "not a file directory" in out.lower(), out

        # --- help ------------------------------------------------------------
        # There was no help mechanism: <?> printed a hard-coded menu and each
        # dot-command family printed its own blurb. .H is prose, <?> is still
        # the menu.
        before = len(c.log)
        out = c.send(".H")[before:]
        assert "Help topics" in out, out
        assert "files" in out, out
        before = len(c.log)
        out = c.send(".H files")[before:]
        assert "File directories" in out, out
        assert ".RFG" in out, out
        before = len(c.log)
        out = c.send(".H nosuchtopic")[before:]
        assert "No help on" in out, out

        # --- chat ------------------------------------------------------------
        # Built on citadel_express, so a line typed here is the same row the web
        # /chat view and XMPP read. That is the point of not giving it a channel
        # table of its own.
        # Deliberately not `pageme`: the <P>age assertions below pin that user's
        # queue exactly, and a chat line is the same kind of row.
        c.send("C")
        c.send("bbsaide")
        c.send("evening")
        out = c.send("")
        assert "leaving chat" in out.lower(), out
        rows = con.execute(
            "SELECT count(*) FROM citadel_express "
            "WHERE from_user = 'bbsuser' AND to_user = 'bbsaide' AND text = 'evening'").fetchone()[0]
        assert rows == 1, "the chat line did not reach citadel_express"

        # --- registration ----------------------------------------------------
        c.send(".EG")
        for field in ("Ada Lovelace", "1 Analytical Way", "London", "", "NW1", "555", "ada@example.com"):
            c.send(field)
        out = c.send("England")
        assert "Registration saved." in out, out
        row = con.execute(
            "SELECT real_name, city, email, country FROM citadel_user_reg WHERE username = 'bbsuser'"
        ).fetchone()
        assert row == ("Ada Lovelace", "London", "ada@example.com", "England"), row
        # Filling it in sets US_REGIS.
        assert con.execute(
            "SELECT flags & 1024 FROM citadel_users WHERE username = 'bbsuser'"
        ).fetchone()[0] == 1024, "US_REGIS was not set"

        # The user listing honours the same record.
        before = len(c.log)
        out = c.send(".RU")[before:]
        assert "bbsuser" in out and "pageme" in out, out

        # --- message deletion -------------------------------------------------
        msgnum = con.execute(
            "SELECT msgnum FROM citadel_messages WHERE subject = 'Hello from telnet'"
        ).fetchone()[0]
        c.send(".G Lobby")
        c.send("D")
        out = c.send(str(msgnum))
        assert "Message deleted." in out, out
        assert con.execute(
            "SELECT count(*) FROM citadel_room_msgs WHERE room_num = 0 AND msgnum = ?", [msgnum]
        ).fetchone()[0] == 0, "the message pointer survived deletion"

        # --- aide gating -------------------------------------------------------
        # bbsuser is axlevel 4, so the admin submenu must refuse.
        before = len(c.log)
        out = c.send(".AK")[before:]
        assert "Higher access required" in out, out

        out = c.send("T")
        assert "Goodbye." in out, out
        c.close()

        # The paged message is queued for the other user.
        pending = con.execute(
            "SELECT from_user, text FROM citadel_express WHERE lower(to_user) = 'pageme'"
        ).fetchall()
        assert pending == [("bbsuser", "ping from the BBS")], pending

        check_web_crossover(con)

        # What the Lobby holds now: the message posted over telnet was read back
        # earlier and then removed by the <D>elete test, so only the fixture
        # message planted for the skip/goto check should remain.
        rows = con.execute(
            "SELECT m.author, m.subject FROM citadel_messages m "
            "JOIN citadel_room_msgs rm USING (msgnum) "
            "JOIN citadel_rooms r USING (room_num) WHERE r.name = 'Lobby' "
            "ORDER BY m.msgnum"
        ).fetchall()
        assert rows == [("someone", "unread one")], rows
        # The message row itself survives; only the room pointer went, which is
        # what makes deletion safe for a message shared across rooms.
        assert con.execute(
            "SELECT count(*) FROM citadel_messages WHERE subject = 'Hello from telnet'"
        ).fetchone()[0] == 1, "deletion removed the message row, not just the pointer"

        # The session row is cleaned up on disconnect.
        time.sleep(0.5)
        assert con.execute("SELECT count(*) FROM citadel_sessions").fetchone()[0] == 0

        # --- implicit TLS listener -----------------------------------------
        c = Bbs(PORT_TLS, use_tls=True)
        assert "Enter your name" in c.log, c.log
        c.send("bbsuser")
        out = c.send("secret")
        assert "Welcome, bbsuser." in out, out
        c.send("T")
        c.close()
    finally:
        con.execute("CALL qm_telnet_stop()").fetchall()
        con.execute("CALL qm_telnets_stop()").fetchall()

    print("PASS: telnet login, Lobby, enter/read message, known rooms, who, page,")
    print("      file areas, help, chat, telnets,")
    print("      and the telnet/web crossover in both directions")


def check_web_crossover(con):
    """The "tables are the bus" claim, exercised across two front-ends.

    A message paged from telnet has to reach a browser, and a browser has to
    show up in the telnet who-list. Neither worked before: nothing on the web
    side ever called PendingExpress, and nothing on the web side ever registered
    a presence row.
    """
    con.execute(f"LOAD '{ext('quackmail_http')}'")
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    con.execute(f"CALL qm_http_start('{HOST}', {WEB_PORT})")
    time.sleep(0.4)
    base = f"http://{HOST}:{WEB_PORT}"
    try:
        jar = http.cookiejar.CookieJar()
        op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
        page = op.open(base + "/login").read().decode()
        tok = re.search(r'name="_csrf" value="([^"]*)"', page).group(1)
        op.open(base + "/login", urllib.parse.urlencode(
            {"_csrf": tok, "username": "pageme", "password": "secret"}).encode()).read()

        # The browser is now a presence row, so RWHO and the telnet who-list see
        # it — a web user was previously undiscoverable and therefore unpageable.
        rows = con.execute(
            "SELECT client FROM citadel_sessions WHERE username = 'pageme'").fetchall()
        assert rows == [("Web session",)], f"the browser is not in citadel_sessions: {rows}"

        # And the page telnet sent earlier arrives in the browser's chat.
        chat = op.open(base + "/chat").read().decode()
        assert "ping from the BBS" in chat, "a message paged from telnet never reached the web"
        assert "bbsuser" in chat, "the sender is not named in the web transcript"

        # Reading it delivered it, the same way ShowPendingExpress does at the
        # telnet prompt — so the two front-ends agree about what is unread.
        left = con.execute(
            "SELECT count(*) FROM citadel_express WHERE lower(to_user) = 'pageme' "
            "AND NOT delivered").fetchone()[0]
        assert left == 0, "the web read the message without marking it delivered"

        # Signing out takes the presence row with it. The cascade lives in
        # websession.cpp rather than in the logout handler, so that no revoke
        # path — logout, admin revoke, password change, expiry sweep — can
        # forget it.
        out_tok = re.search(r'name="_csrf" value="([^"]*)"', chat).group(1)
        try:
            op.open(base + "/logout", urllib.parse.urlencode({"_csrf": out_tok}).encode()).read()
        except urllib.error.HTTPError:
            pass
        rows = con.execute(
            "SELECT count(*) FROM citadel_sessions WHERE client = 'Web session'").fetchone()[0]
        assert rows == 0, "signing out left the browser's presence row behind"

        # ---- one file store, two front doors --------------------------------
        # This is the assertion the whole file-area design exists for. A file
        # uploaded over telnet has to be the same bytes fetched over WebDAV: not
        # a copy, not a re-encoding, the same message. Without this, "three
        # front doors over one store" is a claim rather than a fact.
        room = con.execute(
            "SELECT room_num FROM citadel_rooms WHERE display_name = 'Uploads'").fetchone()[0]
        payload = bytes(range(256))  # every byte, including 0xFF and NUL
        con.execute("CALL qm_config_set('qm_web_force_https', '0')")

        c = Bbs(PORT)
        c.send("bbsuser")
        c.send("secret")
        c.send(".G Uploads")
        c.send(".EF")
        c.send("crossover.bin")
        c.send("from the BBS")
        c.send("B")
        c.send(base64.b64encode(payload).decode())
        out = c.send(".")
        assert "stored crossover.bin" in out.lower(), out
        c.send("T")
        c.close()

        req = urllib.request.Request(
            f"{base}/dav/files/bbsuser/{room}/crossover.bin", method="GET")
        req.add_header("Authorization", "Basic " + base64.b64encode(b"bbsuser:secret").decode())
        got = urllib.request.build_opener().open(req, timeout=10).read()
        assert got == payload, (
            f"the telnet upload did not come back over WebDAV byte for byte: "
            f"{len(got)} of {len(payload)}")
    finally:
        con.execute("CALL qm_http_stop()").fetchall()


if __name__ == "__main__":
    main()
