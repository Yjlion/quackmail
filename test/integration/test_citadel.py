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

        # Text clients send MSGP (message-format preference) during handshake
        # and abort login on a non-2xx reply; the server must accept it.
        resp = c.command("MSGP text/plain|text/html")
        assert resp.startswith("200"), f"MSGP not accepted: {resp}"

        # Create and log in a new user.
        resp = c.command("NEWU cituser")
        assert resp.startswith("200 "), f"NEWU failed: {resp}"

        # Enter the Lobby.
        resp = c.command("GOTO Lobby")
        assert resp.startswith("200 Lobby"), f"GOTO failed: {resp}"

        # Magic room aliases clients use to auto-navigate must resolve, and a
        # missing room must NOT return 540 (which clients read as "password
        # required" and loop on).
        resp = c.command("GOTO _BASEROOM_")
        assert resp.startswith("200 Lobby"), f"GOTO _BASEROOM_ failed: {resp}"
        resp = c.command("GOTO _MAIL_")
        assert resp.startswith("200 Mail"), f"GOTO _MAIL_ failed: {resp}"
        resp = c.command("GOTO does-not-exist")
        assert resp.startswith("550"), f"missing room should be 550, got: {resp}"
        c.command("GOTO Lobby")  # return to the Lobby for the message test

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

        # ---- CONF: get and set system configuration ----------------------
        # Taken from citadel/server/control.c. cituser is axlevel 4, so the
        # aide gate refuses first -- a gate tested only from the passing side
        # is not tested.
        resp = c.command("CONF GETVAL|c_nodename")
        assert resp.startswith("540"), f"CONF was allowed to a non-aide: {resp}"

        con.execute("UPDATE citadel_users SET axlevel = 6 WHERE username = 'cituser'")
        # axlevel is read at login, so this needs a fresh session.
        c.command("QUIT")
        c = Cit(HOST, PORT)
        c.readline()
        c.command("USER cituser")
        resp = c.command("PASS ")
        a = Cit(HOST, PORT)
        a.readline()
        a.command("NEWU citaide")
        con.execute("UPDATE citadel_users SET axlevel = 6 WHERE username = 'citaide'")
        a.command("QUIT")
        a = Cit(HOST, PORT)
        a.readline()
        a.command("USER citaide")
        a.command("PASS ")

        # GETVAL/PUTVAL/LISTVAL are the modern key/value form and map straight
        # onto citadel_config, which is already a keyed store.
        resp = a.command("CONF PUTVAL|c_humannode|Test Node")
        assert resp.startswith("200"), f"CONF PUTVAL: {resp}"
        assert con.execute(
            "SELECT value FROM citadel_config WHERE name = 'c_humannode'").fetchone()[0] == "Test Node"
        resp = a.command("CONF GETVAL|c_humannode")
        assert resp == "200 Test Node|", f"CONF GETVAL: {resp}"
        resp = a.command("CONF GETVAL|no_such_key_at_all")
        assert resp.startswith("500"), f"an absent key should be 500: {resp}"

        a.send("CONF LISTVAL")
        hdr = a.readline()
        assert hdr.startswith("100"), f"CONF LISTVAL header: {hdr}"
        vals = a.read_listing()
        assert any(v.startswith("c_humannode|Test Node") for v in vals), f"LISTVAL: {vals[:5]}"

        # CONF GET is the deprecated positional list. Citadel's own source says
        # "please do not add fields or change their order" -- and its text
        # client still sends it, so it has to work. 73 lines, counted off
        # control.c's own cprintf sequence, with the retired positions still
        # occupying a line each -- renumbering to close a gap would shift every
        # field after it.
        a.send("CONF GET")
        hdr = a.readline()
        assert hdr.startswith("100"), f"CONF GET header: {hdr}"
        fields = a.read_listing()
        assert len(fields) == 73, f"CONF GET returned {len(fields)} fields, not 73"
        assert fields[2] == "Test Node", f"c_humannode is not at position 2: {fields[:4]}"
        assert fields[3] == "", "position 3 is retired and must still be sent as a blank line"

        # And back the other way, positionally.
        a.send("CONF SET")
        hdr = a.readline()
        assert hdr.startswith("400"), f"CONF SET header: {hdr}"
        fields[2] = "Renamed Node"
        for f in fields:
            a.send(f)
        a.send("000")
        # CONF SET ends silently -- control.c's branch sends no reply, and a
        # client that waited for one would hang. So synchronise on the *next*
        # command: commands on one connection are answered in order, so a reply
        # to this one proves the SET above finished.
        resp = a.command("NOOP")
        assert resp.startswith("200"), f"NOOP after CONF SET: {resp}"
        assert con.execute(
            "SELECT value FROM citadel_config WHERE name = 'c_humannode'"
        ).fetchone()[0] == "Renamed Node", "CONF SET did not land"
        # It is also logged to the Aide room, which is the only record that the
        # configuration changed.
        logged = con.execute(
            "SELECT count(*) FROM citadel_messages m JOIN citadel_room_msgs rm USING (msgnum) "
            "WHERE rm.room_num = 1 AND m.subject = 'Citadel Configuration Manager Message'"
        ).fetchone()[0]
        assert logged == 1, f"CONF SET was not logged to the Aide room ({logged})"

        # GETSYS/PUTSYS store arbitrary stanzas as euid-keyed messages in the
        # Local System Configuration room -- Citadel's own mechanism, and the
        # one WebCit uses for host-specific config.
        a.send("CONF PUTSYS|application/x-test-config")
        hdr = a.readline()
        assert hdr.startswith("400"), f"CONF PUTSYS header: {hdr}"
        a.send("line one")
        a.send("line two")
        a.send("000")
        a.send("CONF GETSYS|application/x-test-config")
        hdr = a.readline()
        assert hdr.startswith("100"), f"CONF GETSYS header: {hdr}"
        got = a.read_listing()
        assert got == ["line one", "line two"], f"the stanza did not round-trip: {got}"
        resp = a.command("CONF GETSYS|application/x-never-stored")
        assert resp.startswith("550"), f"an absent stanza should be 550: {resp}"

        # ---- GPEX / SPEX / TDAP: message expiry ---------------------------
        # Worth knowing: the modern Citadel *server* implements none of these,
        # while its own text client still sends GPEX/SPEX -- so the client's
        # expiry editor talks to nothing. The policy model (0 next-level,
        # 1 manual, 2 by count, 3 by age) is Citadel's and is what its purger
        # reads.
        a.command("GOTO Lobby")
        resp = a.command("GPEX roompolicy")
        assert resp.startswith("200 0|0"), f"an unset policy is not next-level: {resp}"
        resp = a.command("SPEX roompolicy|2|3")
        assert resp.startswith("200"), f"SPEX: {resp}"
        resp = a.command("GPEX roompolicy")
        assert resp.startswith("200 2|3"), f"the policy did not stick: {resp}"

        # A count or age of zero would mean "purge everything", which is never
        # what somebody setting a policy meant. The client refuses to send it
        # and the server refuses to store it.
        resp = a.command("SPEX roompolicy|2|0")
        assert resp.startswith("550"), f"a zero value was accepted: {resp}"
        resp = a.command("SPEX nosuchlevel|1|0")
        assert resp.startswith("550"), f"an unknown level was accepted: {resp}"

        # Keep at most 1 message in the Lobby, then run the purger. TDAP is
        # Citadel's own "manually initiate auto-purger"; there is no EXPI.
        before = con.execute(
            "SELECT count(*) FROM citadel_room_msgs WHERE room_num = 0").fetchone()[0]
        assert before >= 1, "the Lobby should have the posted message"
        a.command("SPEX roompolicy|2|1")
        resp = a.command("TDAP")
        assert resp.startswith("200"), f"TDAP: {resp}"
        after = con.execute(
            "SELECT count(*) FROM citadel_room_msgs WHERE room_num = 0").fetchone()[0]
        assert after == 1, f"the Lobby kept {after} messages under a policy of 1"

        a.command("QUIT")
        c.command("QUIT")
    finally:
        con.execute("CALL cit_stop()").fetchall()

    # Verify persistence in the shared Citadel tables. Scoped to the Lobby: NEWU
    # also posts a "new user" notice into the Aide room, so an unqualified count
    # over citadel_messages would pick that up too.
    rows = con.execute(
        "SELECT m.author, m.subject FROM citadel_messages m "
        "JOIN citadel_room_msgs rm ON rm.msgnum = m.msgnum "
        "WHERE rm.room_num = 0 ORDER BY m.msgnum"
    ).fetchall()
    assert len(rows) == 1, f"expected 1 message in the Lobby, got {rows}"
    assert rows[0][0] == "cituser", rows[0]
    assert rows[0][1] == "Hello Subject", rows[0]

    ptr = con.execute(
        "SELECT count(*) FROM citadel_room_msgs WHERE room_num = 0"
    ).fetchone()[0]
    assert ptr == 1, "message should be pointed into the Lobby (room 0)"

    # Registering an account posted a system notice into the Aide room (room 1),
    # authored by the node rather than by any user. Both users registered here
    # are expected; so is the CONF SET audit line, which is the only record that
    # the configuration changed.
    notice = con.execute(
        "SELECT m.author, m.subject FROM citadel_messages m "
        "JOIN citadel_room_msgs rm ON rm.msgnum = m.msgnum "
        "WHERE rm.room_num = 1 ORDER BY m.msgnum"
    ).fetchall()
    assert notice == [
        ("quackcit", "New user: cituser"),
        ("quackcit", "New user: citaide"),
        ("quackcit", "Citadel Configuration Manager Message"),
    ], f"aide notices: {notice}"
    # Every one of them is authored by the node, not by whoever triggered it:
    # an aide-room notice is the server speaking.
    assert all(a == "quackcit" for a, _ in notice), f"an aide notice has a user author: {notice}"

    print("PASS: Citadel NEWU -> GOTO -> ENT0 -> MSGS -> MSG0 round-trip, Aide notice,")
    print("      CONF (GET/SET/GETVAL/PUTVAL/LISTVAL/GETSYS/PUTSYS), GPEX/SPEX/TDAP")


if __name__ == "__main__":
    main()
