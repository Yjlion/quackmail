#!/usr/bin/env python3
"""End-to-end test for the XMPP front-end (quackmail_xmpp).

Drives a c2s session the way a client does — stream open, feature discovery,
SASL PLAIN, stream restart, resource bind, session, roster, presence, disco,
vCard, ping — then sends a <message>, checks it was queued as a Citadel express
message, and verifies a second session receives one pushed to it as a stanza.
Also checks the implicit-TLS listener.

Requires: pip install duckdb==1.5.4
Run after `make` so the loadable extensions exist under build/release/extension.
"""
import base64
import os
import socket
import ssl
import time

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 15322
PORT_TLS = 15323

STREAM = (
    '<stream:stream to="{node}" xmlns="jabber:client" '
    'xmlns:stream="http://etherx.jabber.org/streams" version="1.0">'
)


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class Client:
    def __init__(self, port, use_tls=False, node="quackcit"):
        s = socket.create_connection((HOST, port), timeout=5)
        if use_tls:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            s = ctx.wrap_socket(s)
        s.settimeout(1.5)
        self.s = s
        self.node = node

    def send(self, xml):
        self.s.sendall(xml.encode())

    def read(self, wait=1.2):
        """Collect whatever the server sends within `wait` seconds."""
        self.s.settimeout(wait)
        got = ""
        try:
            while True:
                d = self.s.recv(8192)
                if not d:
                    break
                got += d.decode("utf-8", "replace")
        except OSError:
            pass
        return got

    def open_stream(self):
        self.send(STREAM.format(node=self.node))
        return self.read()

    def login(self, user, pw):
        feats = self.open_stream()
        assert "<mechanisms" in feats and "PLAIN" in feats, feats
        token = base64.b64encode(f"\0{user}\0{pw}".encode()).decode()
        self.send(f'<auth xmlns="urn:ietf:params:xml:ns:xmpp-sasl" mechanism="PLAIN">{token}</auth>')
        out = self.read()
        assert "<success" in out, out
        feats = self.open_stream()
        assert "<bind" in feats, feats
        self.send(
            '<iq type="set" id="b1"><bind xmlns="urn:ietf:params:xml:ns:xmpp-bind">'
            "<resource>test</resource></bind></iq>"
        )
        out = self.read()
        assert f"<jid>{user}@{self.node}/test</jid>" in out, out
        self.send('<iq type="set" id="s1"><session xmlns="urn:ietf:params:xml:ns:xmpp-session"/></iq>')
        assert '<iq type="result" id="s1"' in self.read(), "session failed"

    def close(self):
        try:
            self.send("</stream:stream>")
            self.s.close()
        except OSError:
            pass


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    con.execute(f"LOAD '{ext('quackmail_citadel')}'")
    con.execute(f"LOAD '{ext('quackmail_xmpp')}'")
    con.execute("CALL qm_user_add('alice', 'secret')")
    con.execute("CALL qm_user_add('bob', 'secret')")

    for call in (
        f"SELECT note FROM qm_xmpp_start('{HOST}', {PORT}, starttls=>true)",
        f"SELECT note FROM qm_xmpps_start('{HOST}', {PORT_TLS}, implicit_tls=>true)",
    ):
        assert con.execute(call).fetchone()[0] == "started"
    time.sleep(0.3)

    try:
        # Stream features before authentication mirror Citadel's set.
        probe = Client(PORT)
        feats = probe.open_stream()
        for want in ("<starttls", "urn:ietf:params:xml:ns:xmpp-sasl", "PLAIN",
                     "jabber.org/features/iq-auth", "xmpp-bind", "xmpp-session"):
            assert want in feats, f"{want} missing from stream features: {feats}"
        # A bad password is rejected.
        token = base64.b64encode(b"\0alice\0wrong").decode()
        probe.send(f'<auth xmlns="urn:ietf:params:xml:ns:xmpp-sasl" mechanism="PLAIN">{token}</auth>')
        assert "<failure" in probe.read(), "bad password should fail"
        probe.close()

        alice = Client(PORT)
        alice.login("alice", "secret")

        # Roster: presence-derived, so it lists other logged-in users only.
        alice.send('<iq type="get" id="r1"><query xmlns="jabber:iq:roster"/></iq>')
        out = alice.read()
        assert 'xmlns="jabber:iq:roster"' in out and 'id="r1"' in out, out
        assert "bob@" not in out, "bob is not online yet"

        # disco, vCard and ping all answer.
        alice.send('<iq type="get" id="d1" to="quackcit">'
                   '<query xmlns="http://jabber.org/protocol/disco#info"/></iq>')
        assert 'id="d1"' in alice.read(), "disco#info"
        alice.send('<iq type="get" id="v1"><vCard xmlns="vcard-temp"/></iq>')
        out = alice.read()
        assert "<fn>alice</fn>" in out and "<nickname>alice</nickname>" in out, out
        alice.send('<iq type="get" id="p1"><ping xmlns="urn:xmpp:ping"/></iq>')
        assert '<iq type="result"' in alice.read(), "ping"
        # An unknown namespace gets Citadel's 503.
        alice.send('<iq type="get" id="z1"><query xmlns="jabber:iq:private"/></iq>')
        out = alice.read()
        assert 'code="503"' in out and "service-unavailable" in out, out

        # bob logs in; alice's roster and presence now see him.
        bob = Client(PORT)
        bob.login("bob", "secret")
        alice.send('<iq type="get" id="r2"><query xmlns="jabber:iq:roster"/></iq>')
        out = alice.read()
        assert 'jid="bob@quackcit"' in out and 'subscription="both"' in out, out
        alice.send("<presence/>")
        out = alice.read()
        assert '<presence from="bob@quackcit"' in out, out

        # A message from alice reaches bob as a pushed stanza.
        alice.send('<message to="bob@quackcit" type="chat"><body>hello bob</body></message>')
        time.sleep(0.2)
        rows = con.execute(
            "SELECT from_user, text FROM citadel_express WHERE lower(to_user) = 'bob'"
        ).fetchall()
        assert rows == [("alice", "hello bob")], rows

        out = bob.read(2.0)
        assert "<message" in out and "<body>hello bob</body>" in out, out
        assert 'from="alice@quackcit"' in out, out

        # ... and it is marked delivered, so it is not pushed twice.
        assert bob.read(1.0).find("hello bob") == -1, "message pushed twice"
        delivered = con.execute("SELECT delivered FROM citadel_express").fetchall()
        assert delivered == [(True,)], delivered

        bob.close()
        alice.close()
        time.sleep(0.5)
        assert con.execute("SELECT count(*) FROM citadel_sessions").fetchone()[0] == 0

        # --- implicit TLS listener -----------------------------------------
        c = Client(PORT_TLS, use_tls=True)
        c.login("alice", "secret")
        c.close()
    finally:
        con.execute("CALL qm_xmpp_stop()").fetchall()
        con.execute("CALL qm_xmpps_stop()").fetchall()

    print("PASS: XMPP SASL/bind/session, roster, presence, disco, vCard, ping, message push, xmpps")


if __name__ == "__main__":
    main()
