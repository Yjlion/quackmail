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

        # <X> hides the menu.
        before = len(c.log)
        out = c.send("X")[before:]
        assert "Expert mode ON" in out, out
        before = len(c.log)
        out = c.send("K")[before:]
        assert "<K>nown rooms" not in out, "menu should be hidden in expert mode"

        out = c.send("T")
        assert "Goodbye." in out, out
        c.close()

        # The paged message is queued for the other user.
        pending = con.execute(
            "SELECT from_user, text FROM citadel_express WHERE lower(to_user) = 'pageme'"
        ).fetchall()
        assert pending == [("bbsuser", "ping from the BBS")], pending

        # The posted message is in the Lobby, readable by every other front-end.
        rows = con.execute(
            "SELECT m.author, m.subject FROM citadel_messages m "
            "JOIN citadel_room_msgs rm USING (msgnum) "
            "JOIN citadel_rooms r USING (room_num) WHERE r.name = 'Lobby'"
        ).fetchall()
        assert rows == [("bbsuser", "Hello from telnet")], rows

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
