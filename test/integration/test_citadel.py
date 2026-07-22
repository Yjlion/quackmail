#!/usr/bin/env python3
"""End-to-end test for the native Citadel protocol (quackmail_citadel).

Loads the extensions into an in-memory DuckDB, starts the Citadel listener, then
drives a real client session over a socket: create a user, enter the Lobby, post
a message, list it, and read it back. Finally verifies the message landed in the
citadel_* tables.

Requires: pip install duckdb==1.5.4
Run after `make` so the loadable extensions exist under build/release/extension.
"""
import os
import socket
import time

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 15041


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class Cit:
    """A tiny line-oriented Citadel client."""

    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.buf = b""

    def readline(self):
        while b"\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("connection closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line.decode("utf-8", "replace")

    def send(self, line):
        self.sock.sendall(line.encode("utf-8") + b"\r\n")

    def command(self, line):
        """Send a command and return the single result line."""
        self.send(line)
        return self.readline()

    def read_listing(self):
        """Read lines of a '100' listing up to the '000' terminator."""
        lines = []
        while True:
            ln = self.readline()
            if ln == "000":
                return lines
            lines.append(ln)


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    con.execute(f"LOAD '{ext('quackmail_citadel')}'")

    note = con.execute(
        f"SELECT note FROM cit_start('{HOST}', {PORT})"
    ).fetchone()[0]
    assert note == "started", f"server did not start: {note}"
    time.sleep(0.3)

    try:
        c = Cit(HOST, PORT)

        greeting = c.readline()
        assert greeting.startswith("200 "), f"bad greeting: {greeting}"

        # Create and log in a new user.
        resp = c.command("NEWU cituser")
        assert resp.startswith("200 "), f"NEWU failed: {resp}"

        # Enter the Lobby.
        resp = c.command("GOTO Lobby")
        assert resp.startswith("200 Lobby"), f"GOTO failed: {resp}"

        # Post a message: ENT0 <post=1>|<rcpt>|<anon>|<format>|<subject>
        resp = c.command("ENT0 1||0|0|Hello Subject")
        assert resp.startswith("400"), f"ENT0 did not ask for text: {resp}"
        c.send("Hello, Citadel world.")
        resp = c.command("000")
        assert resp.startswith("200 "), f"ENT0 save failed: {resp}"
        msgnum = resp.split(" ", 1)[1].strip()

        # List messages in the Lobby.
        c.send("MSGS all")
        hdr = c.readline()
        assert hdr.startswith("100"), f"MSGS header: {hdr}"
        nums = c.read_listing()
        assert msgnum in nums, f"posted message {msgnum} not listed: {nums}"

        # Read the message back.
        c.send(f"MSG0 {msgnum}|0")
        hdr = c.readline()
        assert hdr.startswith("100"), f"MSG0 header: {hdr}"
        fields = c.read_listing()
        assert "subj=Hello Subject" in fields, f"subject missing: {fields}"
        assert "Hello, Citadel world." in fields, f"body missing: {fields}"

        c.command("QUIT")
    finally:
        con.execute("CALL cit_stop()").fetchall()

    # Verify persistence in the shared Citadel tables.
    rows = con.execute(
        "SELECT author, subject FROM citadel_messages ORDER BY msgnum"
    ).fetchall()
    assert len(rows) == 1, f"expected 1 message, got {rows}"
    assert rows[0][0] == "cituser", rows[0]
    assert rows[0][1] == "Hello Subject", rows[0]

    ptr = con.execute(
        "SELECT count(*) FROM citadel_room_msgs WHERE room_num = 0"
    ).fetchone()[0]
    assert ptr == 1, "message should be pointed into the Lobby (room 0)"

    print("PASS: Citadel NEWU -> GOTO -> ENT0 -> MSGS -> MSG0 round-trip")


if __name__ == "__main__":
    main()
