#!/usr/bin/env python3
"""End-to-end test for the POP3 front-end (quackmail_pop3).

Checks the behaviours QuackCit matches against a stock Citadel POP3 server:
the greeting/response wording, the CAPA list, STLS, LAST, TOP honouring its line
count, and the UPDATE state applied at QUIT (deletions expunged, last-read
pointer advanced). Also exercises the implicit-TLS pop3s listener.

Requires: pip install duckdb==1.5.4
Run after `make` so the loadable extensions exist under build/release/extension.
"""
import os
import poplib
import socket
import ssl
import time

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 11100
PORT_TLS = 11995


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def deliver(con, user, subject, body):
    """Put a native (format 0) message into the user's Mail room."""
    con.execute(
        """
        INSERT INTO citadel_messages (msgnum, author, recipient, msgtime, subject, format_type, raw)
        VALUES (nextval('citadel_msg_seq'), 'alice', ?, epoch(now())::BIGINT, ?, 0, ?)
        """,
        [user, subject, body],
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


def raw_session(port, lines, use_tls=False):
    """Drive a POP3 session by hand and return every response line.

    Responses are not read one-per-command: a multi-line reply can arrive split
    across TCP segments, so we read what is available after each command and
    then drain the socket to EOF at the end. Order is preserved either way.
    """
    s = socket.create_connection((HOST, port), timeout=5)
    if use_tls:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        s = ctx.wrap_socket(s)
    s.settimeout(3)
    out = []
    buf = b""

    def read_some(until_eof=False):
        nonlocal buf
        eof = False
        try:
            while True:
                d = s.recv(8192)
                if not d:
                    eof = True
                    break
                buf += d
                if buf.endswith(b"\r\n") and not until_eof:
                    break
        except socket.timeout:
            pass
        except OSError:
            eof = True
        text, buf = buf.decode("utf-8", "replace"), b""
        lines = text.split("\r\n")
        # Keep a trailing partial line only when the peer hung up on it (the
        # server closes right after answering QUIT).
        out.extend(lines if eof and lines and lines[-1] else lines[:-1])

    read_some()
    for line in lines:
        s.sendall(line.encode() + b"\r\n")
        read_some()
    # Anything still in flight (the server closes right after answering QUIT).
    s.settimeout(1)
    read_some(until_eof=True)
    s.close()
    return out


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    con.execute(f"LOAD '{ext('quackmail_citadel')}'")
    con.execute(f"LOAD '{ext('quackmail_pop3')}'")
    con.execute("CALL qm_user_add('popuser', 'secret')")
    # Assign the usernum + default rooms the way a login would.
    con.execute(
        "INSERT INTO citadel_users (username, usernum, axlevel) "
        "VALUES ('popuser', nextval('citadel_user_seq'), 4)"
    )
    con.execute("CALL cit_room_add('Scratch')")  # forces schema/room seeding
    room = con.execute(
        "SELECT room_num FROM citadel_rooms WHERE display_name = 'Mail' AND mailbox_owner > 0"
    ).fetchall()

    note = con.execute(
        f"SELECT note FROM qm_pop3_start('{HOST}', {PORT}, starttls=>true)"
    ).fetchone()[0]
    assert note == "started", f"pop3 did not start: {note}"
    note = con.execute(
        f"SELECT note FROM qm_pop3s_start('{HOST}', {PORT_TLS}, implicit_tls=>true)"
    ).fetchone()[0]
    assert note == "started", f"pop3s did not start: {note}"
    time.sleep(0.3)

    try:
        # A login provisions the Mail room; do it once, then deliver mail.
        M = poplib.POP3(HOST, PORT, timeout=5)
        assert M.welcome == b"+OK QuackCit POP3 server ready.", M.welcome
        M.user("popuser")
        M.pass_("secret")
        M.quit()

        deliver(con, "popuser", "First message", "line one\nline two\nline three\nline four\n")
        deliver(con, "popuser", "Second message", "only body line\n")

        # --- wording + CAPA + LAST + TOP ------------------------------------
        out = raw_session(
            PORT,
            [
                "CAPA",
                "STAT",              # before login
                "USER nosuchuser",
                "USER popuser",
                "PASS wrong",
                "PASS secret",
                "STAT",
                "LIST",
                "UIDL 1",
                "LAST",
                "TOP 1 2",
                "DELE 1",
                "DELE 1",
                "LIST 1",
                "LIST 9",
                "RSET",
                "NOOP",
                "FROB",
                "QUIT",
            ],
        )
        joined = "\n".join(out)
        for expected in [
            "+OK Capability list follows",
            "IMPLEMENTATION QuackCit",
            "-ERR Not logged in.",
            "-ERR No such user.",
            "+OK Password required for popuser",
            "-ERR That is NOT the password.",
            "+OK popuser is logged in (2 messages)",
            "+OK Here's your mail:",
            "+OK Message 1 deleted.",
            "-ERR You already deleted that message.",
            "-ERR Sorry, you deleted that message.",
            "-ERR no such message, only 2 are here",
            "+OK Reset completed.",
            "+OK No operation.",
            "-ERR I'm afraid I can't do that.",
            "+OK Goodbye...",
        ]:
            assert expected in joined, f"missing response {expected!r} in:\n{joined}"
        assert "STLS" not in joined, "Citadel does not advertise STLS in CAPA"

        # TOP 1 2: full header block, then exactly two body lines.
        top_start = out.index("+OK Message 1:")
        top_end = out.index(".", top_start)
        block = out[top_start + 1 : top_end]
        blank = block.index("")
        assert any(h.startswith("Subject: First message") for h in block[:blank]), block
        assert block[blank + 1 :] == ["line one", "line two"], block[blank + 1 :]

        # --- STLS upgrades the plaintext listener ---------------------------
        s = socket.create_connection((HOST, PORT), timeout=5)
        assert s.recv(200).startswith(b"+OK")
        s.sendall(b"STLS\r\n")
        assert s.recv(200) == b"+OK Begin TLS negotiation now\r\n"
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        tls = ctx.wrap_socket(s)
        tls.sendall(b"USER popuser\r\nPASS secret\r\nSTAT\r\nQUIT\r\n")
        tls.settimeout(2)
        got = b""
        try:
            while b"Goodbye" not in got:
                chunk = tls.recv(4096)
                if not chunk:
                    break
                got += chunk
        except OSError:
            pass
        assert b"is logged in" in got, got
        tls.close()

        # --- implicit TLS listener -----------------------------------------
        out = raw_session(PORT_TLS, ["USER popuser", "PASS secret", "STAT", "QUIT"], use_tls=True)
        assert any("is logged in" in l for l in out), out

        # --- UPDATE state at QUIT: DELE then QUIT expunges ------------------
        M = poplib.POP3(HOST, PORT, timeout=5)
        M.user("popuser")
        M.pass_("secret")
        assert len(M.list()[1]) == 2, M.list()
        M.dele(1)
        M.quit()

        M = poplib.POP3(HOST, PORT, timeout=5)
        M.user("popuser")
        M.pass_("secret")
        assert len(M.list()[1]) == 1, "deletion was not applied at QUIT"
        # The previous session's QUIT advanced the last-read pointer, so the
        # remaining message counts as seen.
        assert M.stat()[0] == 1, M.stat()
        M.quit()

        # A DELE is a removal like any other, so it leaves a tombstone. This
        # path used to unlink the pointer with SQL of its own, which meant a
        # message deleted from a POP3 client stayed visible forever to JMAP's
        # Email/changes and DAV's sync-collection over the same room.
        tombs = con.execute("SELECT count(*) FROM citadel_room_tombstones").fetchone()[0]
        assert tombs >= 1, "DELE left no tombstone, so no synchronizing client can see it"
    finally:
        con.execute("CALL qm_pop3_stop()").fetchall()
        con.execute("CALL qm_pop3s_stop()").fetchall()

    left = con.execute(
        "SELECT count(*) FROM citadel_room_msgs rm JOIN citadel_rooms r USING (room_num) "
        "WHERE r.display_name = 'Mail' AND r.mailbox_owner > 0"
    ).fetchone()[0]
    assert left == 1, f"expected 1 message left in the Mail room, got {left}"

    print("PASS: POP3 Citadel wording, CAPA, STLS, pop3s, LAST/TOP, QUIT UPDATE state")


if __name__ == "__main__":
    main()
