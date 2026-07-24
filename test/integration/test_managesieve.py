#!/usr/bin/env python3
"""End-to-end test for ManageSieve (RFC 5804) and the Sieve engine behind it.

Two halves:
  * the protocol — STARTTLS, AUTHENTICATE, PUTSCRIPT/LISTSCRIPTS/GETSCRIPT/
    SETACTIVE/RENAMESCRIPT/DELETESCRIPT, and CHECKSCRIPT refusing a bad script,
  * the effect — a script installed over ManageSieve actually routes a message
    delivered over SMTP into the folder it names.

The second half is the point: a filter nobody can install is not a filter, and
a filter that installs but does not run is worse.

Requires: pip install duckdb==1.5.4. Run after `make`.
"""
import base64
import os
import smtplib
import socket
import ssl
import time
from email.mime.text import MIMEText

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
SIEVE_PORT = 4290
SMTP_PORT = 3526

GOOD_SCRIPT = """require ["fileinto"];
if header :contains "Subject" "[list]" {
    fileinto "Lists";
}
"""

BAD_SCRIPT = """require ["fileinto"];
if header :contains "Subject" "oops" {
    frobnicate "everything";
}
"""


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class Sieve:
    """A ManageSieve client: enough of RFC 5804 to drive the server."""

    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.f = self.sock.makefile("rb")
        self.greeting = self.read_response()

    def send(self, text):
        self.sock.sendall((text + "\r\n").encode())

    def line(self):
        raw = self.f.readline()
        if not raw:
            raise AssertionError("connection closed")
        return raw.decode("utf-8", "replace").rstrip("\r\n")

    def read_literal(self, header):
        """Read the N bytes announced by a '{N}' or '{N+}' line."""
        n = int(header.strip().lstrip("{").rstrip("}").rstrip("+"))
        buf = b""
        while len(buf) < n:
            chunk = self.f.read(n - len(buf))
            if not chunk:
                raise AssertionError("connection closed inside a literal")
            buf += chunk
        self.line()  # the CRLF that terminates the literal's line
        return buf.decode("utf-8", "replace")

    def read_response(self):
        """Collect data lines up to the terminating OK/NO/BYE."""
        lines = []
        while True:
            line = self.line()
            if line.startswith("{"):
                lines.append(self.read_literal(line))
                continue
            if line.startswith(("OK", "NO", "BYE")):
                return lines, line
            lines.append(line)

    def cmd(self, text):
        self.send(text)
        return self.read_response()

    def put(self, name, script):
        """PUTSCRIPT with the script as a non-synchronizing literal."""
        payload = script.replace("\n", "\r\n").encode()
        self.send(f'PUTSCRIPT "{name}" {{{len(payload)}+}}')
        self.sock.sendall(payload + b"\r\n")
        return self.read_response()

    def check(self, script):
        payload = script.replace("\n", "\r\n").encode()
        self.send(f"CHECKSCRIPT {{{len(payload)}+}}")
        self.sock.sendall(payload + b"\r\n")
        return self.read_response()

    def starttls(self):
        _, status = self.cmd("STARTTLS")
        assert status.startswith("OK"), f"STARTTLS refused: {status}"
        ctx = ssl._create_unverified_context()
        self.sock = ctx.wrap_socket(self.sock)
        self.f = self.sock.makefile("rb")
        return self.read_response()  # capabilities are re-sent after the upgrade

    def login(self, user, password):
        token = base64.b64encode(f"\0{user}\0{password}".encode()).decode()
        return self.cmd(f'AUTHENTICATE "PLAIN" "{token}"')

    def close(self):
        try:
            self.cmd("LOGOUT")
        except Exception:
            pass
        self.sock.close()


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    con.execute(f"LOAD '{ext('quackmail_managesieve')}'")
    con.execute(f"LOAD '{ext('quackmail_smtp_in')}'")

    assert con.execute("SELECT ok FROM qm_user_add('alice', 'secret')").fetchone()[0]

    note = con.execute(
        f"SELECT note FROM qm_managesieve_start('{HOST}', {SIEVE_PORT}, starttls=true)"
    ).fetchone()[0]
    assert note == "started", f"managesieve did not start: {note}"
    time.sleep(0.3)

    try:
        c = Sieve(HOST, SIEVE_PORT)
        caps, status = c.greeting
        assert status.startswith("OK"), f"greeting: {status}"
        joined = " ".join(caps)
        assert "IMPLEMENTATION" in joined and "STARTTLS" in joined, caps
        # Only the extensions the engine really implements are advertised.
        assert "fileinto" in joined and "reject" in joined, caps

        # Commands are refused before authentication.
        _, status = c.cmd("LISTSCRIPTS")
        assert status.startswith("NO"), f"LISTSCRIPTS before AUTH should fail: {status}"

        caps, status = c.starttls()
        assert status.startswith("OK"), f"post-TLS capabilities: {status}"
        assert "PLAIN" in " ".join(caps), f"SASL should be offered over TLS: {caps}"

        _, status = c.login("alice", "wrong")
        assert status.startswith("NO"), f"bad password should fail: {status}"
        _, status = c.login("alice", "secret")
        assert status.startswith("OK"), f"login failed: {status}"

        # A script that does not parse is refused, and nothing is stored.
        _, status = c.check(BAD_SCRIPT)
        assert status.startswith("NO"), f"bad script should be refused: {status}"
        _, status = c.check(GOOD_SCRIPT)
        assert status.startswith("OK"), f"good script should check out: {status}"
        _, status = c.put("broken", BAD_SCRIPT)
        assert status.startswith("NO"), f"PUTSCRIPT should refuse a bad script: {status}"

        _, status = c.put("filters", GOOD_SCRIPT)
        assert status.startswith("OK"), f"PUTSCRIPT failed: {status}"

        scripts, status = c.cmd("LISTSCRIPTS")
        assert status.startswith("OK") and any("filters" in s for s in scripts), scripts
        assert not any("broken" in s for s in scripts), f"refused script was stored: {scripts}"
        assert not any("ACTIVE" in s for s in scripts), f"nothing should be active yet: {scripts}"

        body, status = c.cmd('GETSCRIPT "filters"')
        assert status.startswith("OK"), f"GETSCRIPT failed: {status}"
        assert "fileinto" in body[0], f"GETSCRIPT returned: {body}"

        _, status = c.cmd('SETACTIVE "filters"')
        assert status.startswith("OK"), f"SETACTIVE failed: {status}"
        scripts, _ = c.cmd("LISTSCRIPTS")
        assert any("ACTIVE" in s for s in scripts), f"script should be active: {scripts}"

        # The active script is protected from deletion.
        _, status = c.cmd('DELETESCRIPT "filters"')
        assert status.startswith("NO"), f"deleting the active script should fail: {status}"

        _, status = c.cmd('GETSCRIPT "nosuch"')
        assert status.startswith("NO"), f"missing script should be NO: {status}"

        # Rename, then rename back so the delivery half below still finds it.
        _, status = c.cmd('RENAMESCRIPT "filters" "renamed"')
        assert status.startswith("OK"), f"RENAMESCRIPT failed: {status}"
        _, status = c.cmd('RENAMESCRIPT "renamed" "filters"')
        assert status.startswith("OK"), f"RENAMESCRIPT back failed: {status}"

        c.close()
    finally:
        con.execute("CALL qm_managesieve_stop()").fetchall()

    active = con.execute(
        "SELECT name FROM quackmail_sieve_scripts WHERE username = 'alice' AND active"
    ).fetchall()
    assert active == [("filters",)], f"expected 'filters' active, got {active}"

    # ---- the filter must actually run at delivery time --------------------
    note = con.execute(f"SELECT note FROM qm_smtp_in_start('{HOST}', {SMTP_PORT})").fetchone()[0]
    assert note == "started", f"smtp_in did not start: {note}"
    time.sleep(0.3)
    try:
        s = smtplib.SMTP(HOST, SMTP_PORT, timeout=10)
        for subject in ("[list] weekly digest", "an ordinary note"):
            m = MIMEText("body\n")
            m["Subject"] = subject
            m["From"] = "sender@example.com"
            m["To"] = "alice@quackmail.test"
            s.sendmail("sender@example.com", ["alice@quackmail.test"], m.as_string())
        s.quit()
    finally:
        con.execute("CALL qm_smtp_in_stop()").fetchall()

    placed = dict(
        con.execute(
            "SELECT m.subject, r.display_name FROM citadel_messages m "
            "JOIN citadel_room_msgs rm ON rm.msgnum = m.msgnum "
            "JOIN citadel_rooms r ON r.room_num = rm.room_num "
            "WHERE r.mailbox_owner > 0"
        ).fetchall()
    )
    assert placed.get("[list] weekly digest") == "Lists", f"fileinto did not route: {placed}"
    assert placed.get("an ordinary note") == "Mail", f"implicit keep did not apply: {placed}"

    print("PASS: ManageSieve stores/validates scripts and the active filter routes delivery")


if __name__ == "__main__":
    main()
