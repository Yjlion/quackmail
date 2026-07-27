#!/usr/bin/env python3
"""End-to-end test for the web front-end (quackmail_http).

The web interface is the only part of QuackCit a browser can reach, so most of
what is asserted here is not "does the page render" but "does it refuse the
things it must refuse": CSRF, transport pinning, IDOR, XSS from message bodies,
the DKIM private key never reaching a page, and a slow client not being able to
hold a connection thread open forever.

Requires: pip install duckdb==1.5.4
Run after `make release` so the loadable extensions exist under
build/release/extension.
"""
import hashlib
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
PORT = 18080
PORT_TLS = 18443
BASE = f"http://{HOST}:{PORT}"
BASE_TLS = f"https://{HOST}:{PORT_TLS}"

# A subject a hostile sender would use, and the same thing hidden inside an
# RFC 2047 encoded-word. The second one only stays harmless if the server
# decodes before it escapes.
XSS = "<script>alert(1)</script>"
XSS_ENCODED = "=?utf-8?B?PHNjcmlwdD5hbGVydCgxKTwvc2NyaXB0Pg==?="


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """Follow nothing: the status and Location are what is under test."""

    def redirect_request(self, *args, **kwargs):
        return None


def opener(cookiejar=None, tls=False):
    handlers = [NoRedirect()]
    if cookiejar is not None:
        handlers.append(urllib.request.HTTPCookieProcessor(cookiejar))
    if tls:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        handlers.append(urllib.request.HTTPSHandler(context=ctx))
    return urllib.request.build_opener(*handlers)


def request(op, url, data=None, headers=None, method=None):
    """Return (status, headers, body) without raising on 4xx/5xx."""
    body = None
    if data is not None:
        body = urllib.parse.urlencode(data).encode()
    req = urllib.request.Request(url, data=body, method=method)
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        r = op.open(req, timeout=10)
        return r.status, dict(r.headers), r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read().decode("utf-8", "replace")


def csrf_of(page):
    marker = 'name="_csrf" value="'
    i = page.index(marker) + len(marker)
    return page[i : page.index('"', i)]


def sign_in(base, user, password, tls=False):
    """Complete the login flow; returns (opener, cookiejar)."""
    jar = http.cookiejar.CookieJar()
    op = opener(jar, tls=tls)
    _, _, page = request(op, base + "/login")
    status, headers, _ = request(
        op,
        base + "/login",
        {"_csrf": csrf_of(page), "username": user, "password": password, "next": "/mail/"},
    )
    assert status == 303, f"login for {user} returned {status}"
    return op, jar


def raw_send(payload, expect_close=True, timeout=10):
    """Speak HTTP by hand, for the cases a URL library will not produce."""
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    try:
        s.sendall(payload)
        chunks = []
        while True:
            b = s.recv(4096)
            if not b:
                break
            chunks.append(b)
            if not expect_close and chunks:
                break
        return b"".join(chunks).decode("utf-8", "replace")
    finally:
        s.close()


def deliver(con, user, subject, raw, fmt=4):
    """File a message directly into a user's Mail room."""
    con.execute(
        """
        INSERT INTO citadel_messages
            (msgnum, author, recipient, msgtime, subject, format_type, raw)
        VALUES (nextval('citadel_msg_seq'), 'sender@example.com', ?,
                epoch(now())::BIGINT, ?, ?, ?)
        """,
        [user, subject, fmt, raw],
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
    return con.execute("SELECT max(msgnum) FROM citadel_messages").fetchone()[0]


MULTIPART = (
    "MIME-Version: 1.0\r\n"
    'Content-Type: multipart/mixed; boundary="BOUND"\r\n'
    "\r\n"
    "--BOUND\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n\r\n"
    "plain body\r\n"
    "--BOUND\r\n"
    "Content-Type: text/html; charset=utf-8\r\n\r\n"
    '<p onclick="x()">hi</p><script>alert(2)</script>'
    '<img src="http://tracker.example/pixel.gif">\r\n'
    "--BOUND\r\n"
    'Content-Type: application/octet-stream; name="notes.bin"\r\n'
    'Content-Disposition: attachment; filename="notes.bin"\r\n'
    "Content-Transfer-Encoding: base64\r\n\r\n"
    "AAECAwQF\r\n"
    "--BOUND--\r\n"
)


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_citadel", "quackmail_http", "quackmail_pop3"):
        con.execute(f"LOAD '{ext(name)}'")

    for user, pw, ax in (("webuser", "secret", 4), ("otheruser", "secret", 4), ("admin", "adminpw", 6)):
        con.execute(f"CALL qm_user_add('{user}', '{pw}')")
        con.execute(
            "INSERT INTO citadel_users (username, usernum, axlevel) "
            f"VALUES ('{user}', nextval('citadel_user_seq'), {ax})"
        )
    con.execute("CALL cit_room_add('Scratch')")

    # A dev box has no certificate, so the redirect has to be off for the
    # plaintext listener to serve anything. Its behaviour is asserted below by
    # switching it back on for one request.
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    con.execute("CALL qm_config_set('c_fqdn', 'quackmail.test')")

    note = con.execute(f"SELECT note FROM qm_http_start('{HOST}', {PORT})").fetchone()[0]
    assert note == "started", f"http did not start: {note}"
    tls_up = False
    try:
        note = con.execute(
            f"SELECT note FROM qm_https_start('{HOST}', {PORT_TLS}, implicit_tls=>true)"
        ).fetchone()[0]
        tls_up = note == "started"
    except Exception:
        tls_up = False
    time.sleep(0.4)

    try:
        # ---- health and anonymous access --------------------------------
        status, _, body = request(opener(), BASE + "/healthz")
        assert status == 200 and body.strip() == "ok", "healthz"

        status, headers, _ = request(opener(), BASE + "/mail/")
        assert status == 303, f"anonymous /mail/ returned {status}"
        assert "/login" in headers.get("Location", ""), "anonymous /mail/ should bounce to /login"

        status, headers, page = request(opener(), BASE + "/login")
        assert status == 200, "login page"
        assert 'name="_csrf"' in page, "login form carries no CSRF token"
        assert "Set-Cookie" not in headers, "the login page must not mint a session"

        # ---- CSRF --------------------------------------------------------
        status, _, _ = request(
            opener(), BASE + "/login", {"username": "webuser", "password": "secret"}
        )
        assert status == 403, f"login without a CSRF token returned {status}"

        # ---- account enumeration ----------------------------------------
        # The two pages must not differ in any way that depends on whether the
        # account exists. Two things legitimately differ between any two
        # responses and are normalized out first: the per-response CSP nonce,
        # and the user name the form echoes back (which the client supplied, so
        # it tells an attacker nothing they did not already know).
        op = opener()
        _, _, page = request(op, BASE + "/login")
        tok = csrf_of(page)
        s1, h1, b1 = request(op, BASE + "/login", {"_csrf": tok, "username": "webuser", "password": "wrong"})
        s2, h2, b2 = request(op, BASE + "/login", {"_csrf": tok, "username": "nobody", "password": "wrong"})
        assert "Set-Cookie" not in h1 and "Set-Cookie" not in h2, "a failed login minted a session"
        assert s1 == s2, f"status differs by account existence: {s1} vs {s2}"

        def scrub(body, username):
            out = re.sub(r'nonce="[^"]*"', 'nonce="N"', body)
            return out.replace(username, "USER")

        assert scrub(b1, "webuser") == scrub(b2, "nobody"), (
            "the wrong-password and unknown-user pages differ; that enumerates accounts"
        )

        # ---- a real sign-in ----------------------------------------------
        op, jar = sign_in(BASE, "webuser", "secret")
        cookie = [c for c in jar if c.name == "qcsid"]
        assert cookie, "no session cookie"
        raw_token = cookie[0].value

        rows = con.execute(
            "SELECT token_hash, username, tls FROM quackmail_web_sessions"
        ).fetchall()
        assert len(rows) == 1, f"expected one session row, got {len(rows)}"
        stored, who, sess_tls = rows[0]
        assert who == "webuser"
        assert stored == hashlib.sha256(raw_token.encode()).hexdigest(), "token is not stored hashed"
        assert stored != raw_token, "the raw token is in the database"
        assert sess_tls is False, "a plaintext session is recorded as TLS"

        status, _, _ = request(op, BASE + "/mail/")
        assert status == 200, f"authenticated /mail/ returned {status}"

        # ---- security headers --------------------------------------------
        _, headers, _ = request(op, BASE + "/mail/")
        csp = headers.get("Content-Security-Policy", "")
        assert "nonce-" in csp, "CSP carries no nonce"
        assert "'unsafe-inline'" not in csp.split("style-src")[0], "script-src allows inline script"
        assert headers.get("X-Content-Type-Options") == "nosniff"
        assert headers.get("X-Frame-Options") == "DENY"
        assert "no-store" in headers.get("Cache-Control", "")
        assert "close" in headers.get("Connection", "").lower(), "the connection is not closed"

        # ---- open redirect -----------------------------------------------
        jar2 = http.cookiejar.CookieJar()
        op2 = opener(jar2)
        _, _, page = request(op2, BASE + "/login?next=https://evil.example/")
        _, headers, _ = request(
            op2,
            BASE + "/login",
            {
                "_csrf": csrf_of(page),
                "username": "webuser",
                "password": "secret",
                "next": "https://evil.example/",
            },
        )
        loc = headers.get("Location", "")
        assert not loc.startswith("http") and not loc.startswith("//"), f"open redirect to {loc}"

        # ---- XSS from message bodies -------------------------------------
        deliver(con, "webuser", XSS, "a plain body", fmt=0)
        deliver(con, "webuser", XSS_ENCODED, "another body", fmt=0)
        mail_room = con.execute(
            "SELECT r.room_num FROM citadel_rooms r JOIN citadel_users u "
            "ON u.usernum = r.mailbox_owner WHERE u.username = 'webuser' "
            "AND r.display_name = 'Mail'"
        ).fetchone()[0]

        status, _, page = request(op, f"{BASE}/bbs/room/{mail_room}")
        assert status == 200, f"mail room listing returned {status}"
        assert "<script>alert(1)" not in page, "an unescaped <script> reached the page"
        assert "&lt;script&gt;alert(1)" in page, "the hostile subject was not rendered escaped"
        # The encoded-word one proves the decode-then-escape ordering: if the
        # server escaped first it would show the raw =?utf-8?B?...?= instead.
        assert page.count("&lt;script&gt;alert(1)") >= 2, "encoded-word subject was not decoded"

        # ---- IDOR ---------------------------------------------------------
        # otheruser has to sign in first so their personal rooms exist. Without
        # that, deliver() files the message into no room at all and the 404
        # below would pass for the wrong reason — proving nothing about the
        # ownership check it is supposed to be testing.
        sign_in(BASE, "otheruser", "secret")
        secret_num = deliver(con, "otheruser", "otheruser private mail", "secret body", fmt=0)
        other_room = con.execute(
            "SELECT r.room_num FROM citadel_rooms r JOIN citadel_users u "
            "ON u.usernum = r.mailbox_owner WHERE u.username = 'otheruser' "
            "AND r.display_name = 'Mail'"
        ).fetchone()[0]
        assert con.execute(
            "SELECT count(*) FROM citadel_room_msgs WHERE room_num = ? AND msgnum = ?",
            [other_room, secret_num],
        ).fetchone()[0] == 1, "the fixture message is not actually in otheruser's Mail room"

        # Their message number, asked for through *our* room.
        status, _, page = request(op, f"{BASE}/bbs/room/{mail_room}/msg/{secret_num}")
        assert status == 404, f"reading another user's message returned {status}"
        assert "secret body" not in page
        # Their room, by number.
        status, _, page = request(op, f"{BASE}/bbs/room/{other_room}")
        assert status == 404, "another user's mailbox room is reachable by number"
        # Their message through their own room, which we may not resolve either.
        status, _, page = request(op, f"{BASE}/bbs/room/{other_room}/msg/{secret_num}")
        assert status == 404, "another user's message is readable through their room"
        assert "secret body" not in page

        # ---- MIME: attachments and the HTML part -------------------------
        mime_num = deliver(con, "webuser", "with attachment", MULTIPART, fmt=4)
        status, _, page = request(op, f"{BASE}/bbs/room/{mail_room}/msg/{mime_num}")
        assert status == 200
        assert "plain body" in page
        assert "sandbox" in page, "the HTML part is not framed with sandbox"
        assert "allow-scripts" not in page, "the HTML frame allows scripts"
        assert "allow-same-origin" not in page, "the HTML frame is not a separate origin"

        status, headers, html = request(op, f"{BASE}/bbs/room/{mail_room}/msg/{mime_num}/html")
        assert status == 200
        assert headers.get("Content-Type", "").startswith("text/html")
        assert "default-src 'none'" in headers.get("Content-Security-Policy", "")
        assert "img-src data:" in headers.get("Content-Security-Policy", "")
        assert "<script>" not in html, "the sanitizer left a script tag"
        assert "onclick" not in html, "the sanitizer left an event handler"

        # Attachments are never served as the sender's type.
        status, headers, blob = request(op, f"{BASE}/bbs/room/{mail_room}/msg/{mime_num}/part/3")
        assert status == 200, f"attachment fetch returned {status}"
        assert headers.get("Content-Type") == "application/octet-stream"
        assert "attachment" in headers.get("Content-Disposition", "")
        assert headers.get("X-Content-Type-Options") == "nosniff"

        # ---- posting, and the cross-protocol proof -----------------------
        _, _, page = request(op, f"{BASE}/bbs/room/0/compose")
        status, _, _ = request(
            op,
            f"{BASE}/bbs/room/0/post",
            {"_csrf": csrf_of(page), "subject": "from the web", "body": "hello lobby", "refs": ""},
        )
        assert status == 303, f"posting returned {status}"
        got = con.execute(
            "SELECT count(*) FROM citadel_messages m JOIN citadel_room_msgs rm "
            "ON rm.msgnum = m.msgnum WHERE rm.room_num = 0 AND m.subject = 'from the web'"
        ).fetchone()[0]
        assert got == 1, "the post did not land in the Lobby"

        # ---- CSRF is per session -----------------------------------------
        admin_op, _ = sign_in(BASE, "admin", "adminpw")
        _, _, admin_page = request(admin_op, f"{BASE}/bbs/room/0/compose")
        status, _, _ = request(
            op,
            f"{BASE}/bbs/room/0/post",
            {"_csrf": csrf_of(admin_page), "subject": "stolen", "body": "x", "refs": ""},
        )
        assert status == 403, "one session's CSRF token was accepted by another"

        # ---- the admin console is closed by default ----------------------
        status, _, _ = request(op, BASE + "/admin/users")
        assert status == 403, f"/admin/users as a normal user returned {status}"
        status, _, _ = request(admin_op, BASE + "/admin/users")
        assert status == 403, "the admin console is reachable while disabled"

        con.execute("CALL qm_config_set('qm_web_admin_enabled', '1')")
        status, _, _ = request(admin_op, BASE + "/admin/users")
        assert status == 403, "the admin console is reachable over plaintext"

        con.execute("CALL qm_config_set('qm_web_admin_require_tls', '0')")
        status, _, page = request(admin_op, BASE + "/admin/users")
        assert status == 200, f"the admin console is still closed to an aide: {status}"
        status, _, _ = request(op, BASE + "/admin/users")
        assert status == 403, "a non-aide reached the admin console"

        # Re-authentication guards the sharpest actions.
        status, _, _ = request(
            admin_op,
            BASE + "/admin/users/add",
            {"_csrf": csrf_of(page), "username": "newbie", "password": "pw", "admin_password": "wrong"},
        )
        assert status == 403, "user creation went through without the operator's password"
        status, _, _ = request(
            admin_op,
            BASE + "/admin/users/add",
            {
                "_csrf": csrf_of(page),
                "username": "newbie",
                "password": "pw12345",
                "admin_password": "adminpw",
            },
        )
        assert status == 303, "user creation failed with the right password"
        assert con.execute(
            "SELECT count(*) FROM quackmail_users WHERE username = 'newbie'"
        ).fetchone()[0] == 1

        # ---- the DKIM private key never reaches a page -------------------
        con.execute("CALL qm_dkim_keygen('example.com', 'sel', 1024)")
        status, _, page = request(admin_op, BASE + "/admin/dkim")
        assert status == 200
        assert "PRIVATE KEY" not in page, "the DKIM private key is on the page"
        assert "MII" not in page.split("v=DKIM1")[0], "something key-shaped precedes the DNS record"
        assert "v=DKIM1" in page, "the DNS record is missing"

        # ---- HTTPS redirect ----------------------------------------------
        con.execute("CALL qm_config_set('qm_web_force_https', '1')")
        status, headers, _ = request(opener(), BASE + "/mail/")
        assert status == 301, f"with force_https on, plain /mail/ returned {status}"
        assert headers.get("Location", "").startswith("https://quackmail.test"), (
            "the redirect target is not built from c_fqdn"
        )
        status, _, _ = request(opener(), BASE + "/healthz")
        assert status == 200, "healthz must stay reachable for a load balancer"
        con.execute("CALL qm_config_set('qm_web_force_https', '0')")

        # ---- logout -------------------------------------------------------
        # Logout ends *this* session, not every session for the account — the
        # open-redirect check above signed webuser in a second time and that
        # one must survive. (Only a password change signs everything out.)
        before = con.execute(
            "SELECT count(*) FROM quackmail_web_sessions WHERE username = 'webuser'"
        ).fetchone()[0]
        assert before >= 2, f"expected more than one webuser session, got {before}"

        _, _, page = request(op, BASE + "/prefs")
        status, _, _ = request(op, BASE + "/logout", {"_csrf": csrf_of(page)})
        assert status == 303
        assert con.execute(
            "SELECT count(*) FROM quackmail_web_sessions WHERE token_hash = ?",
            [hashlib.sha256(raw_token.encode()).hexdigest()],
        ).fetchone()[0] == 0, "logout left this session's row behind"
        after = con.execute(
            "SELECT count(*) FROM quackmail_web_sessions WHERE username = 'webuser'"
        ).fetchone()[0]
        assert after == before - 1, "logout revoked more than the session that asked"

        # The cookie it held is now inert.
        status, _, _ = request(
            opener(), BASE + "/mail/", headers={"Cookie": f"qcsid={raw_token}"}
        )
        assert status == 303, "a revoked cookie still authenticates"

        # ---- malformed requests -------------------------------------------
        long_line = b"GET /" + b"a" * 9000 + b" HTTP/1.1\r\nHost: x\r\n\r\n"
        assert "414" in raw_send(long_line), "an over-long request line was not refused"

        many = b"GET /login HTTP/1.1\r\nHost: x\r\n"
        many += b"".join(b"X-Pad-%d: y\r\n" % i for i in range(200))
        many += b"\r\n"
        assert "431" in raw_send(many), "a header flood was not refused"

        big = b"POST /login HTTP/1.1\r\nHost: x\r\nContent-Length: 999999999\r\n\r\n"
        assert "413" in raw_send(big), "an oversized Content-Length was not refused"

        chunked = b"POST /login HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"
        assert "411" in raw_send(chunked), "chunked encoding was not refused"

        smuggle = (
            b"POST /login HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
            b"Transfer-Encoding: chunked\r\n\r\n"
        )
        assert "400" in raw_send(smuggle), "a smuggling-shaped request was not refused"

        assert "400" in raw_send(b"GET /login\r\n\r\n"), "HTTP/0.9 was not refused"

        traversal = b"GET /%2e%2e%2fetc%2fpasswd HTTP/1.1\r\nHost: x\r\n\r\n"
        out = raw_send(traversal)
        assert "400" in out or "404" in out, "a traversal path was not refused"
        assert "root:" not in out

        # HEAD gets the headers and no body.
        head = raw_send(b"HEAD /login HTTP/1.1\r\nHost: x\r\n\r\n")
        assert head.startswith("HTTP/1.1 200"), "HEAD /login failed"
        assert head.rstrip().endswith("\r\n") or "\r\n\r\n" in head
        assert "<form" not in head, "HEAD returned a body"

        # ---- slow loris ----------------------------------------------------
        # Open a socket, send nothing, and confirm the server drops it rather
        # than pinning a thread forever. This is the direct regression test for
        # ClientStream::SetTimeouts and the request deadline.
        s = socket.create_connection((HOST, PORT), timeout=40)
        try:
            started = time.time()
            s.settimeout(40)
            data = s.recv(4096)
            elapsed = time.time() - started
            assert data == b"", f"a silent client got {data!r} instead of being dropped"
            assert elapsed < 35, f"a silent client held the connection for {elapsed:.1f}s"
        finally:
            s.close()

        # ---- implicit TLS ---------------------------------------------------
        if tls_up:
            status, headers, _ = request(opener(tls=True), BASE_TLS + "/login")
            assert status == 200, f"the TLS listener returned {status}"
            tls_op, tls_jar = sign_in(BASE_TLS, "webuser", "secret", tls=True)
            tls_cookie = [c for c in tls_jar if c.name == "qcsid"][0].value

            # Transport pinning: a cookie minted over TLS must not work in the
            # clear, and the row is revoked when it is tried.
            plain = opener()
            status, _, _ = request(
                plain, BASE + "/mail/", headers={"Cookie": f"qcsid={tls_cookie}"}
            )
            assert status == 303, "a TLS session was accepted on the plaintext listener"
            revoked = con.execute(
                "SELECT revoked FROM quackmail_web_sessions WHERE token_hash = ?",
                [hashlib.sha256(tls_cookie.encode()).hexdigest()],
            ).fetchone()
            assert revoked and revoked[0] is True, "the misused TLS session was not revoked"
        else:
            print("note: no certificate configured, skipping the HTTPS listener checks")

    finally:
        con.execute("CALL qm_http_stop()")
        if tls_up:
            con.execute("CALL qm_https_stop()")

    print("PASS: quackmail_http (auth, CSRF, IDOR, XSS, admin gating, framing, timeouts)")


if __name__ == "__main__":
    main()
