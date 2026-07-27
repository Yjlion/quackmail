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
import os
import socket
import ssl
import time

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 12300
PORT_TLS = 12992


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

    print("PASS: telnet login, Lobby, enter/read message, known rooms, who, page, telnets")


if __name__ == "__main__":
    main()
