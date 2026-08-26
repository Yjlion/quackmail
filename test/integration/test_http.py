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
import base64
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


def raw_session(timeout=10):
    """A socket the caller drives request by request, for keep-alive checks."""
    return socket.create_connection((HOST, PORT), timeout=timeout)


def read_response(sock):
    """Read exactly one response off `sock`, honouring Content-Length.

    Cannot use recv-until-EOF: the whole point of a persistent connection is
    that EOF does not arrive between responses, so the framing has to be
    parsed or the read blocks forever.
    """
    buf = b""
    while b"\r\n\r\n" not in buf:
        b = sock.recv(4096)
        if not b:
            return buf.decode("utf-8", "replace"), None
        buf += b
    head, rest = buf.split(b"\r\n\r\n", 1)
    text = head.decode("utf-8", "replace")
    length = 0
    for line in text.split("\r\n")[1:]:
        if line.lower().startswith("content-length:"):
            length = int(line.split(":", 1)[1].strip())
    while len(rest) < length:
        b = sock.recv(4096)
        if not b:
            break
        rest += b
    return text, rest.decode("utf-8", "replace")


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

        # ---- a legacy password row is upgraded in place --------------------
        #
        # Passwords used to be one round of salted SHA-256, which a leaked
        # quackmail_users table gives up at GPU speed. Storing new ones with
        # scrypt fixes nothing on an install that already exists: every account
        # created before the change keeps the weak hash until somebody happens
        # to set a new password, which for most accounts is never. So a correct
        # sign-in rewrites the row, and that is the half worth asserting.
        legacy_salt = "0123456789abcdef0123456789abcdef"
        legacy_hash = hashlib.sha256(f"{legacy_salt}:oldpass".encode()).hexdigest()
        con.execute(
            "INSERT OR REPLACE INTO quackmail_users "
            "(username, password_hash, salt, algo, enabled, created_at) "
            "VALUES ('legacyuser', ?, ?, 'sha256', true, now())",
            [legacy_hash, legacy_salt],
        )
        assert con.execute(
            "SELECT algo FROM quackmail_users WHERE username = 'legacyuser'"
        ).fetchone()[0] == "sha256"

        # The old password still works — an upgrade that locked existing users
        # out would be worse than the weakness it fixes.
        sign_in(BASE, "legacyuser", "oldpass")

        algo, new_hash = con.execute(
            "SELECT algo, password_hash FROM quackmail_users WHERE username = 'legacyuser'"
        ).fetchone()
        assert algo.startswith("scrypt$"), f"a legacy row survived a sign-in as {algo!r}"
        assert new_hash != legacy_hash, "the row says scrypt but still holds the old digest"
        # And it still works afterwards, through the new hash this time.
        sign_in(BASE, "legacyuser", "oldpass")
        assert con.execute(
            "SELECT algo FROM quackmail_users WHERE username = 'legacyuser'"
        ).fetchone()[0] == algo, "a scrypt row was rewritten again for no reason"

        # ---- form origin --------------------------------------------------
        # Empty qm_web_origins means any origin may post. This server is reached
        # by whatever name its operator points at it, so the check is opt-in;
        # the CSRF token above is what actually stops a cross-site post.
        def login_from(origin):
            jar = http.cookiejar.CookieJar()
            o = opener(jar)
            _, _, p = request(o, BASE + "/login")
            return request(
                o,
                BASE + "/login",
                {"_csrf": csrf_of(p), "username": "webuser", "password": "secret", "next": "/mail/"},
                headers={"Origin": origin},
            )

        for origin in ("http://192.0.2.1:8080", "https://mail.example.com", "http://[::1]:8443"):
            status, _, _ = login_from(origin)
            assert status == 303, f"with no allow-list, Origin {origin} returned {status}"

        con.execute("CALL qm_config_set('qm_web_origins', 'mail.example.com')")

        status, _, body = login_from("http://192.0.2.1:8080")
        assert status == 403, f"with an allow-list, a foreign Origin returned {status}"
        assert "another site" in body, "the rejection is not the origin check"

        status, _, _ = login_from("https://mail.example.com")
        assert status == 303, "a listed Origin must be accepted"

        status, _, _ = login_from("https://mail.example.com:8443")
        assert status == 303, "the port must not take part in the origin match"

        # Loopback is always its own origin, allow-list or not. The bracketed
        # form is the regression: splitting the port off at the first colon
        # reduced "[::1]:8443" to "[" and rejected the request.
        for origin in ("http://localhost:8080", "http://127.0.0.1:8080", "http://[::1]:8443"):
            status, _, _ = login_from(origin)
            assert status == 303, f"loopback Origin {origin} returned {status}"

        con.execute("CALL qm_config_set('qm_web_origins', 'mail.example.com,*.corp.example')")
        status, _, _ = login_from("https://vpn.corp.example")
        assert status == 303, "a glob entry in the allow-list must match"

        con.execute("CALL qm_config_set('qm_web_origins', '')")
        # Every accepted probe above completed a real sign-in. Clear them, so
        # the session assertions further down still count from zero.
        con.execute("DELETE FROM quackmail_web_sessions")

        # ---- account enumeration ----------------------------------------
        # The two pages must not differ in any way that depends on whether the
        # account exists. Two things legitimately differ between any two
        # responses and are normalized out first: the per-response CSP nonce,
        # and the user name the form echoes back (which the client supplied, so
        # it tells an attacker nothing they did not already know).
        op = opener()
        _, _, page = request(op, BASE + "/login")
        tok = csrf_of(page)
        assert 'class="anon"' in page, "the signed-out login page carries no anon body class"
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

        _, _, page = request(op, BASE + "/prefs")
        assert 'class="anon"' not in page, "a signed-in page still carries the anon body class"

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

        # ---- mail list density preference ----------------------------------
        assert 'layout-' not in page, "a mail room carries a layout class before any preference is set"
        _, _, prefs = request(op, BASE + "/prefs")
        status, _, _ = request(
            op, BASE + "/prefs/settings",
            {"_csrf": csrf_of(prefs), "width": "80", "height": "24", "theme": "auto",
             "tz": "", "mail_layout": "compact"},
        )
        assert status == 303, "saving the mail layout returned " + str(status)
        status, _, page = request(op, f"{BASE}/bbs/room/{mail_room}")
        assert status == 200 and "layout-compact" in page, (
            "the mail room did not pick up the compact layout class"
        )
        # A page that is not a mail room must not carry the class at all — it is
        # scoped to VIEW_MAILBOX/VIEW_DRAFTS, not applied site-wide.
        _, _, prefs_page = request(op, BASE + "/prefs")
        assert "layout-" not in prefs_page, "the layout class leaked onto a non-mail page"
        request(
            op, BASE + "/prefs/settings",
            {"_csrf": csrf_of(prefs), "width": "80", "height": "24", "theme": "auto",
             "tz": "", "mail_layout": "comfortable"},
        )

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
        frame_csp = headers.get("Content-Security-Policy", "")
        assert "default-src 'none'" in frame_csp, frame_csp
        # img-src carries 'self' so a cid: image — which travelled inside the
        # message and reveals nothing by loading — can be served from our own
        # route. The frame is sandboxed with no allow-same-origin, so this grants
        # it no origin access; see the assertions just above.
        assert "img-src 'self' data:" in frame_csp, frame_csp
        # Remote images stay behind the ?images=1 opt-in: those are trackers.
        assert "https:" not in frame_csp.split("img-src")[1].split(";")[0], frame_csp
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

        # ---- search ---------------------------------------------------------
        # Three fixtures, each proving a different thing: a term that lives only
        # in a subject, one that lives only inside a base64 body, and one in a
        # room the searcher must never be shown.
        deliver(con, "webuser", "quarterly zorblat report", "nothing to see here", fmt=0)
        body_only = (
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Transfer-Encoding: base64\r\n"
            "\r\n" + base64.b64encode(b"the codeword is fnordium, buried in the body").decode() + "\r\n"
        )
        deliver(con, "webuser", "an innocuous subject", body_only, fmt=4)
        deliver(con, "otheruser", "zorblat belongs to otheruser", "private", fmt=0)

        def search(params):
            return request(op, BASE + "/search?" + urllib.parse.urlencode(params))

        status, headers, _ = request(opener(), BASE + "/search?q=zorblat")
        assert status == 303 and "/login" in headers.get("Location", ""), "search is not behind login"

        status, _, page = search({"q": "zorblat"})
        assert status == 200, f"search returned {status}"
        assert "quarterly zorblat report" in page, "a subject match was not found"
        # The whole point of resolving the room set through ListRooms: another
        # user's mailbox holds the same term and must never surface.
        assert "belongs to otheruser" not in page, "search crossed into another user's mailbox"

        # The field selector really selects. The sender of every fixture is
        # sender@example.com, so the two directions are distinguishable.
        status, _, page = search({"q": "zorblat", "in": "from"})
        assert "quarterly zorblat report" not in page, "in=from matched a subject"
        status, _, page = search({"q": "sender@example.com", "in": "from"})
        assert "quarterly zorblat report" in page, "in=from did not match the sender"

        # The body case. `fnordium` appears nowhere but inside a base64-encoded
        # part, so finding it proves citadel::BodyText is decoding the message
        # rather than anything matching the stored bytes.
        status, _, page = search({"q": "fnordium"})
        assert "an innocuous subject" not in page, "a body-only word matched as a subject"
        status, _, page = search({"q": "fnordium", "in": "body"})
        assert "an innocuous subject" in page, "message text was not searched"

        # Scoping to one room, and a room number the caller may not read.
        status, _, page = search({"q": "zorblat", "room": str(mail_room)})
        assert "quarterly zorblat report" in page, "scoping to a readable room lost the match"
        status, _, page = search({"q": "zorblat", "room": str(other_room)})
        assert "belongs to otheruser" not in page, "an unreadable room was accepted as a search scope"

        # A window that closed before any fixture was delivered.
        status, _, page = search({"q": "zorblat", "since": "2000-01-01", "until": "2000-01-02"})
        assert "quarterly zorblat report" not in page, "the date bounds were not applied"
        assert "Nothing matched" in page

        # A term full of things that mean something to LIKE and to HTML. It must
        # match nothing, break nothing, and come back escaped.
        status, _, page = search({"q": "100%_" + XSS})
        assert status == 200, f"a term with metacharacters returned {status}"
        assert "<script>alert(1)" not in page, "the search term was reflected unescaped"
        assert "Nothing matched" in page, "a LIKE metacharacter was treated as a wildcard"

        # A passworded room is not searchable until this session has unlocked
        # it — the same rule RequireUnlocked applies one room at a time.
        con.execute("CALL cit_room_add('Vault')")
        vault = con.execute(
            "SELECT room_num FROM citadel_rooms WHERE display_name = 'Vault'"
        ).fetchone()[0]
        con.execute(
            "UPDATE citadel_rooms SET qr_flags = qr_flags | 8, password = 'letmein' "
            "WHERE room_num = ?",
            [vault],
        )
        con.execute(
            "INSERT INTO citadel_messages (msgnum, author, msgtime, subject, format_type, raw) "
            "VALUES (nextval('citadel_msg_seq'), 'keeper', epoch(now())::BIGINT, "
            "'zorblat in the vault', 0, 'body')"
        )
        con.execute(
            "INSERT INTO citadel_room_msgs (room_num, msgnum) "
            "SELECT ?, max(msgnum) FROM citadel_messages",
            [vault],
        )

        status, _, page = search({"q": "zorblat"})
        assert "in the vault" not in page, "a locked room's subjects leaked into search"

        _, _, vault_page = request(op, f"{BASE}/bbs/room/{vault}")
        status, _, _ = request(
            op,
            f"{BASE}/bbs/room/{vault}/unlock",
            {"_csrf": csrf_of(vault_page), "password": "letmein", "next": f"/bbs/room/{vault}"},
        )
        assert status == 303, f"unlocking the room returned {status}"
        status, _, page = search({"q": "zorblat"})
        assert "in the vault" in page, "an unlocked room is still not searched"

        # Two matches fit one page, so there is no pager to render.
        status, _, page = search({"q": "zorblat", "n": "5"})
        assert "quarterly zorblat report" in page and "in the vault" in page, "both matches on one page"
        assert "Page 1 of" not in page, "a single page rendered a pager"

        # Push past the smallest page size the handler will accept and the pager
        # appears, with different rows on each page.
        for i in range(6):
            deliver(con, "webuser", f"zorblat bulk {i}", "filler", fmt=0)
        status, _, first = search({"q": "zorblat", "n": "5", "p": "1"})
        assert "Page 1 of 2" in first, "eight matches at five per page is not two pages"
        status, _, second = search({"q": "zorblat", "n": "5", "p": "2"})
        assert "Page 2 of 2" in second
        assert "zorblat bulk 0" in second, "the oldest match is not on the last page"
        assert "zorblat bulk 0" not in first, "the same row appeared on both pages"

        # ---- the mail folder view -------------------------------------------
        # Every folder EnsureUserRooms provisions is a VIEW_MAILBOX room, so
        # this is the listing all of them render.
        status, _, page = request(op, f"{BASE}/bbs/room/{mail_room}")
        assert status == 200, f"the mail folder returned {status}"
        assert 'name="msgnum"' in page, "the folder listing has no per-message checkbox"
        assert 'formaction="/mail/flag"' in page, "the bulk bar does not reach /mail/flag"
        assert 'name="folder"' in page, "the bulk bar offers no move target"
        # The select-all only works with script, so it is hidden until qc.js
        # marks the document rather than sitting there doing nothing.
        assert "jsonly" in page, "the select-all is not gated on script"
        tok = csrf_of(page)

        def in_room(room_num, msgnum):
            return con.execute(
                "SELECT count(*) FROM citadel_room_msgs WHERE room_num = ? AND msgnum = ?",
                [room_num, msgnum],
            ).fetchone()[0]

        def folder(name):
            return con.execute(
                "SELECT r.room_num FROM citadel_rooms r JOIN citadel_users u "
                "ON u.usernum = r.mailbox_owner WHERE u.username = 'webuser' "
                "AND r.display_name = ?",
                [name],
            ).fetchone()[0]

        def flag_count(msgnum, flag):
            return con.execute(
                "SELECT count(*) FROM citadel_msg_flags WHERE username = 'webuser' "
                "AND msgnum = ? AND flag = ?",
                [msgnum, flag],
            ).fetchone()[0]

        bulk_a = deliver(con, "webuser", "bulk fixture a", "aaa", fmt=0)
        bulk_b = deliver(con, "webuser", "bulk fixture b", "bbb", fmt=0)

        # One request, two messages: the whole point of the view.
        status, _, _ = request(
            op,
            BASE + "/mail/flag",
            [("_csrf", tok), ("room", str(mail_room)), ("msgnum", str(bulk_a)),
             ("msgnum", str(bulk_b)), ("set", "flagged")],
        )
        assert status == 303, f"bulk flag returned {status}"
        assert flag_count(bulk_a, "\\Flagged") == 1 and flag_count(bulk_b, "\\Flagged") == 1, (
            "bulk flag did not reach both messages"
        )
        status, _, _ = request(
            op,
            BASE + "/mail/flag",
            [("_csrf", tok), ("room", str(mail_room)), ("msgnum", str(bulk_a)),
             ("msgnum", str(bulk_b)), ("set", "unflagged")],
        )
        assert status == 303
        assert flag_count(bulk_a, "\\Flagged") == 0, "the flag did not clear"

        # A selection is not a way past the ownership check. otheruser's message
        # rides along in the list and must simply be dropped.
        status, _, _ = request(
            op,
            BASE + "/mail/flag",
            [("_csrf", tok), ("room", str(mail_room)), ("msgnum", str(bulk_a)),
             ("msgnum", str(secret_num)), ("set", "flagged")],
        )
        assert status == 303
        assert flag_count(secret_num, "\\Flagged") == 0, (
            "a bulk action touched a message the caller may not read"
        )
        assert flag_count(bulk_a, "\\Flagged") == 1, "the rest of the selection was dropped too"

        # Move, and then delete-into-Trash.
        status, _, _ = request(
            op,
            BASE + "/mail/move",
            [("_csrf", tok), ("room", str(mail_room)), ("msgnum", str(bulk_a)),
             ("folder", "Sent Items")],
        )
        assert status == 303, f"bulk move returned {status}"
        assert in_room(folder("Sent Items"), bulk_a) == 1, "the message did not arrive"
        assert in_room(mail_room, bulk_a) == 0, "the message is still in the source folder"

        status, _, _ = request(
            op,
            BASE + "/mail/delete",
            [("_csrf", tok), ("room", str(mail_room)), ("msgnum", str(bulk_b))],
        )
        assert status == 303, f"bulk delete returned {status}"
        assert in_room(folder("Trash"), bulk_b) == 1, "delete did not file into Trash"

        # An empty selection changes nothing and says so.
        status, headers, _ = request(
            op, BASE + "/mail/delete", [("_csrf", tok), ("room", str(mail_room))]
        )
        assert status == 303
        assert "ok=nothing" in headers.get("Location", ""), headers.get("Location", "")

        # `back` is where the listing asks to be returned to, so it is a path we
        # built — never somewhere a form field can point a browser.
        bulk_c = deliver(con, "webuser", "bulk fixture c", "ccc", fmt=0)
        status, headers, _ = request(
            op,
            BASE + "/mail/flag",
            [("_csrf", tok), ("room", str(mail_room)), ("msgnum", str(bulk_c)),
             ("set", "flagged"), ("back", "https://evil.example/")],
        )
        assert status == 303
        loc = headers.get("Location", "")
        assert "evil.example" not in loc, "a mail action redirected to a foreign host"
        assert loc.startswith(f"/bbs/room/{mail_room}"), loc

        # Reading a message in a folder sets \Seen, which is what the listing
        # shows as read — the Citadel last-read pointer is a high-water mark and
        # cannot say "this one, not that one".
        bulk_d = deliver(con, "webuser", "read me", "ddd", fmt=0)
        assert flag_count(bulk_d, "\\Seen") == 0
        status, _, page = request(op, f"{BASE}/bbs/room/{mail_room}/msg/{bulk_d}")
        assert status == 200
        assert flag_count(bulk_d, "\\Seen") == 1, "reading in a mail folder did not set \\Seen"
        # And the read pane reaches both endpoints for a single message.
        assert 'action="/mail/flag"' in page, "the read pane offers no flag control"
        assert 'action="/mail/move"' in page, "the read pane offers no move control"

        # A public room is still a message board, not a mailbox.
        status, _, page = request(op, f"{BASE}/bbs/room/0")
        assert status == 200
        assert 'formaction="/mail/flag"' not in page, "the Lobby rendered as a mail folder"

        # ---- the sidebar ----------------------------------------------------
        # The .count span the stylesheet has always described, and which nothing
        # emitted until now.
        # Something new to count: the sections above read the inbox down.
        deliver(con, "webuser", "sidebar fixture", "unread", fmt=0)
        # A folder's count is \Seen, the same thing its listing bolds a row on —
        # not the Citadel last-read pointer, which cannot skip a message left
        # unread behind one that was opened.
        unread_before = con.execute(
            "SELECT count(*) FROM citadel_room_msgs rm WHERE rm.room_num = ? "
            "AND NOT EXISTS (SELECT 1 FROM citadel_msg_flags f WHERE f.msgnum = rm.msgnum "
            "AND f.username = 'webuser' AND f.flag = '\\Seen')",
            [mail_room],
        ).fetchone()[0]
        assert unread_before > 0, "the fixture left nothing unread to count"

        status, _, page = request(op, f"{BASE}/prefs")
        assert status == 200
        assert 'class="count"' in page, "the sidebar emits no unread count"
        assert ">Inbox<" in page, "the sidebar does not name the inbox"
        assert ">Sent Items<" in page, "the sidebar lists no folder past the inbox"
        assert f'href="/bbs/room/{mail_room}"' in page, "the inbox does not link to its room"

        # The count is the room's unread total, not a decoration.
        m = re.search(
            r'href="/bbs/room/%d"[^>]*><span>Inbox</span><span class="count">(\d+)</span>' % mail_room,
            page,
        )
        assert m, "the inbox link carries no count"
        assert int(m.group(1)) == unread_before, f"count {m.group(1)} != {unread_before} unread"

        # Mark-all-read clears it. In a folder that has to set \Seen as well as
        # the pointer, or the button would move a number the sidebar does not
        # read and appear to do nothing.
        _, _, page = request(
            op, f"{BASE}/bbs/room/{mail_room}/markread", {"_csrf": tok}
        )
        assert con.execute(
            "SELECT count(*) FROM citadel_room_msgs rm WHERE rm.room_num = ? "
            "AND NOT EXISTS (SELECT 1 FROM citadel_msg_flags f WHERE f.msgnum = rm.msgnum "
            "AND f.username = 'webuser' AND f.flag = '\\Seen')",
            [mail_room],
        ).fetchone()[0] == 0, "mark-all-read left messages unseen in a mail folder"
        _, _, page = request(op, f"{BASE}/prefs")
        m = re.search(
            r'href="/bbs/room/%d"[^>]*><span>Inbox</span><span class="count">' % mail_room, page
        )
        assert not m, "the inbox still shows a count after being marked read"

        # A room page marks that room current, and falls back to All rooms for
        # one the sidebar does not list.
        _, _, page = request(op, f"{BASE}/bbs/room/{mail_room}")
        assert f'href="/bbs/room/{mail_room}" aria-current="page"' in page, (
            "the folder being read is not marked current"
        )
        # A room the sidebar does *not* list — it only lists rooms with unread —
        # falls back to marking "All rooms", so no room page is ever unmarked.
        request(op, f"{BASE}/bbs/room/0/markread", {"_csrf": tok})
        _, _, page = request(op, f"{BASE}/bbs/room/0")
        assert 'href="/bbs/room/0" aria-current="page"' not in page, (
            "the Lobby is still listed after being marked read"
        )
        assert '<a href="/bbs/" aria-current="page"' in page, (
            "an unlisted room left the whole sidebar unmarked"
        )

        # 0 turns the room listing, and the query behind it, off.
        con.execute("CALL qm_config_set('qm_web_sidebar_rooms', '0')")
        deliver(con, "webuser", "sidebar cap fixture", "x", fmt=0)
        _, _, page = request(op, f"{BASE}/prefs")
        assert ">All rooms<" in page, "the sidebar lost its rooms link entirely"
        con.execute("CALL qm_config_set('qm_web_sidebar_rooms', '10')")

        # ---- moving between messages ----------------------------------------
        # Its own folder, so the three fixtures below really are the whole room
        # and "oldest" means what the assertions say it does. Cloned from the
        # inbox so it is a personal VIEW_MAILBOX room like any other.
        con.execute(
            "INSERT INTO citadel_rooms (room_num, name, display_name, floor_num, qr_flags, "
            "password, listorder, default_view, info, mailbox_owner, highest_msg) "
            "SELECT nextval('citadel_room_seq'), lpad(mailbox_owner::VARCHAR, 10, '0') || '.NavTest', "
            "'NavTest', floor_num, qr_flags, '', listorder, 1, '', mailbox_owner, 0 "
            "FROM citadel_rooms WHERE room_num = ?",
            [mail_room],
        )
        nav_room = con.execute(
            "SELECT room_num FROM citadel_rooms WHERE display_name = 'NavTest'"
        ).fetchone()[0]
        n1 = deliver(con, "webuser", "nav one", "first", fmt=0)
        n2 = deliver(con, "webuser", "nav two", "second", fmt=0)
        n3 = deliver(con, "webuser", "nav three", "third", fmt=0)
        for num in (n1, n2, n3):
            con.execute("DELETE FROM citadel_room_msgs WHERE msgnum = ?", [num])
            con.execute(
                "INSERT INTO citadel_room_msgs (room_num, msgnum) VALUES (?, ?)", [nav_room, num]
            )

        _, _, page = request(op, f"{BASE}/bbs/room/{nav_room}/msg/{n2}")
        assert f'href="/bbs/room/{nav_room}/msg/{n3}">Newer' in page, "no link to the newer message"
        assert f'href="/bbs/room/{nav_room}/msg/{n1}">Older' in page, "no link to the older message"

        # The ends of the room have only one direction each.
        _, _, page = request(op, f"{BASE}/bbs/room/{nav_room}/msg/{n3}")
        assert ">Newer<" not in page, "the newest message offers a newer one"
        assert f'href="/bbs/room/{nav_room}/msg/{n2}">Older' in page

        _, _, page = request(op, f"{BASE}/bbs/room/{nav_room}/msg/{n1}")
        assert ">Older<" not in page, "the oldest message offers an older one"

        # Next unread, in a mail folder, is the next one without \Seen. All
        # three have been read by now, so clearing the flag on n3 alone makes it
        # the only candidate — and reaching it means n2 was skipped rather than
        # simply being the next message along.
        con.execute("DELETE FROM citadel_msg_flags WHERE username = 'webuser' AND msgnum = ?", [n3])
        _, _, page = request(op, f"{BASE}/bbs/room/{nav_room}/msg/{n1}")
        assert f'href="/bbs/room/{nav_room}/msg/{n3}">Next unread' in page, (
            "next unread did not skip the messages already seen"
        )

        # A room holding one message has nowhere to go and says nothing.
        solo = con.execute("SELECT room_num FROM citadel_rooms WHERE display_name = 'Vault'").fetchone()[0]
        _, _, page = request(op, f"{BASE}/bbs/room/{solo}")
        solo_msg = con.execute(
            "SELECT msgnum FROM citadel_room_msgs WHERE room_num = ?", [solo]
        ).fetchone()[0]
        _, _, page = request(op, f"{BASE}/bbs/room/{solo}/msg/{solo_msg}")
        assert "msgnav" not in page, "a room with one message rendered a navigation row"

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

        # ---- /admin/prefs shows a description for every field --------------
        status, _, page = request(admin_op, BASE + "/admin/prefs")
        assert status == 200
        for muted in ("c_bbs_city", "c_sysadm", "qm_web_force_https", "qm_web_admin_require_tls",
                     "qm_web_origins", "qm_aide_log", "qm_spf_reject", "qm_dkim_reject",
                     "qm_dmarc_enforce", "qm_rbl_reject"):
            # Each field's help immediately follows its <input>/<select>, so a
            # muted paragraph within a short window of the field name is good
            # evidence this specific field has one, not just some other field
            # on the page.
            i = page.index(f'name="v_{muted}"')
            assert '<p class="muted">' in page[i:i + 400], (
                f"{muted} still has no description on /admin/prefs"
            )

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

        # ---- date format preference ----------------------------------------
        _, _, prefs = request(op, BASE + "/prefs")
        status, _, _ = request(
            op, BASE + "/prefs/settings",
            {"_csrf": csrf_of(prefs), "width": "80", "height": "24", "theme": "auto",
             "tz": "", "date_format": "us"},
        )
        assert status == 303, "saving the date format returned " + str(status)
        _, _, prefs = request(op, BASE + "/prefs")
        assert 'value="us" selected' in prefs, "the date format preference did not stick"
        # "Last call" on this same page is FormatTime(ctx, user.last_call) — the
        # most direct proof the preference actually changes rendering, not just
        # what /prefs itself echoes back.
        assert re.search(r"Last call</dt><dd>\d{2}/\d{2}/\d{4} \d{2}:\d{2}", prefs), (
            "the last-call timestamp did not switch to MM/DD/YYYY"
        )
        status, _, _ = request(
            op, BASE + "/prefs/settings",
            {"_csrf": csrf_of(prefs), "width": "80", "height": "24", "theme": "auto",
             "tz": "", "date_format": "iso"},
        )
        assert status == 303
        _, _, prefs = request(op, BASE + "/prefs")
        assert re.search(r"Last call</dt><dd>\d{4}-\d{2}-\d{2} \d{2}:\d{2}", prefs), (
            "switching back to iso did not take"
        )
        # ---- i18n infra -----------------------------------------------------
        # A fresh, signed-out opener: `op` is signed in by this point in the
        # test, and GET /login while authenticated redirects instead of
        # rendering the form.
        _, _, page = request(opener(), BASE + "/login")
        assert '<html lang="en">' in page, "the login page does not declare its language"
        _, _, prefs = request(op, BASE + "/prefs")
        assert '<html lang="en">' in prefs, "a signed-in page does not declare its language"
        assert 'name="locale"' in prefs, "there is no language preference on /prefs"
        status, _, _ = request(
            op, BASE + "/prefs/settings",
            {"_csrf": csrf_of(prefs), "width": "80", "height": "24", "theme": "auto",
             "tz": "", "locale": "en"},
        )
        assert status == 303, "saving the language returned " + str(status)
        _, _, prefs = request(op, BASE + "/prefs")
        assert 'value="en" selected' in prefs, "the language preference did not stick"
        # A locale with no catalog entry must not be storable — the pref must
        # clear rather than pin the visitor to something Tr() cannot serve.
        request(
            op, BASE + "/prefs/settings",
            {"_csrf": csrf_of(prefs), "width": "80", "height": "24", "theme": "auto", "tz": "",
             "locale": "xx"},
        )
        _, _, prefs = request(op, BASE + "/prefs")
        assert 'value="xx" selected' not in prefs, "an unknown locale was stored"

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

        # ---- the DAV verbs reach only /dav/ ---------------------------------
        # The connection loop's method allowlist had to widen for CalDAV. That
        # widening must not have opened the pages: every one of these verbs is
        # now read and routed, and every one of them has to be refused here.
        for verb in (b"PROPFIND", b"PUT", b"DELETE", b"REPORT", b"MKCOL", b"PROPPATCH"):
            out = raw_send(verb + b" /login HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n")
            assert "405" in out, f"{verb.decode()} /login was not refused: {out[:80]}"

        # And a verb outside the allowlist is still refused before routing, so
        # the list is a list rather than a formality.
        out = raw_send(b"TRACE /login HTTP/1.1\r\nHost: x\r\n\r\n")
        assert "405" in out, "TRACE was not refused"

        # An unauthenticated DAV request is answered with a challenge, not with
        # the redirect to /login a browser gets — a client has nothing to do
        # with an HTML form.
        out = raw_send(b"PROPFIND /dav/ HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n")
        assert "401" in out, f"an anonymous PROPFIND /dav/ was not challenged: {out[:120]}"
        assert "WWW-Authenticate: Basic" in out, f"no Basic challenge: {out[:200]}"

        # HEAD gets the headers and no body.
        head = raw_send(b"HEAD /login HTTP/1.1\r\nHost: x\r\n\r\n")
        assert head.startswith("HTTP/1.1 200"), "HEAD /login failed"
        assert head.rstrip().endswith("\r\n") or "\r\n\r\n" in head
        assert "<form" not in head, "HEAD returned a body"

        # ---- persistent connections ----------------------------------------
        # A page is several requests now, so the connection is reused. Each
        # assertion below is a way that reuse can go wrong.
        s = raw_session()
        try:
            s.sendall(b"GET /login HTTP/1.1\r\nHost: x\r\n\r\n")
            head1, body1 = read_response(s)
            assert head1.startswith("HTTP/1.1 200"), head1.split("\r\n")[0]
            assert "keep-alive" in head1.lower(), "the first response did not offer keep-alive"
            assert "<form" in (body1 or ""), "the first response had no body"

            # The second request on the same socket must be answered, which is
            # the whole feature.
            s.sendall(b"GET /robots.txt HTTP/1.1\r\nHost: x\r\n\r\n")
            head2, body2 = read_response(s)
            assert head2.startswith("HTTP/1.1 200"), "a reused connection was not served"
            assert "Disallow" in (body2 or ""), "the second response had no body"
        finally:
            s.close()

        # An explicit `Connection: close` is honoured, and the socket really
        # does end rather than merely saying so.
        s = raw_session()
        try:
            s.sendall(b"GET /login HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            head, _ = read_response(s)
            assert "close" in head.lower(), "Connection: close was not honoured"
            assert s.recv(4096) == b"", "the server kept a connection it said it would close"
        finally:
            s.close()

        # HTTP/1.0 never gets a persistent connection.
        s = raw_session()
        try:
            s.sendall(b"GET /login HTTP/1.0\r\nHost: x\r\n\r\n")
            head, _ = read_response(s)
            assert "keep-alive" not in head.lower(), "HTTP/1.0 was offered keep-alive"
        finally:
            s.close()

        # An idle connection is dropped rather than held. The idle deadline is
        # 5s; allow generous slack for a loaded test box, but far below the 15s
        # first-request deadline so a pass here means the *idle* path ran.
        s = raw_session(timeout=20)
        try:
            s.sendall(b"GET /robots.txt HTTP/1.1\r\nHost: x\r\n\r\n")
            read_response(s)
            started = time.time()
            assert s.recv(4096) == b"", "an idle keep-alive connection was not dropped"
            elapsed = time.time() - started
            assert elapsed < 13, f"an idle connection was held for {elapsed:.1f}s"
        finally:
            s.close()

        # The most important assertion in this block. An oversized
        # Content-Length is refused *without* the body being read, so those
        # bytes are still on the wire; the connection must therefore close.
        # Answering and then reusing it would let the unread body be parsed as
        # the next request — request smuggling, self-inflicted.
        s = raw_session()
        try:
            s.sendall(
                b"POST /login HTTP/1.1\r\nHost: x\r\n"
                b"Content-Length: 99999999\r\n\r\n"
                b"GET /admin/ HTTP/1.1\r\nHost: x\r\n\r\n"
            )
            head, _ = read_response(s)
            assert head.startswith("HTTP/1.1 413"), f"an oversized body was not refused: {head[:40]}"
            assert "close" in head.lower(), "a 413 offered to keep the connection"
            # Nothing more may be served: no second status line, then EOF.
            rest = b""
            while True:
                b = s.recv(4096)
                if not b:
                    break
                rest += b
            assert b"HTTP/1.1" not in rest, "the smuggled request was answered"
        finally:
            s.close()

        # ---- static assets --------------------------------------------------
        # Find the stylesheet the way a browser does, from the page itself. A
        # stale generated web_assets.cpp shows up here as a 404.
        _, _, login_page = request(opener(), BASE + "/login")
        m = re.search(r'href="(/static/qc\.[0-9a-f]+\.css)"', login_page)
        assert m, "the login page did not link a hashed stylesheet"
        css_url = m.group(1)

        # Reachable with no session at all: the login page needs it.
        status, headers, css = request(opener(), BASE + css_url)
        assert status == 200, f"{css_url} returned {status}"
        assert headers["Content-Type"] == "text/css; charset=utf-8"
        assert "immutable" in headers["Cache-Control"], headers["Cache-Control"]
        assert "no-store" not in headers["Cache-Control"], "an asset was marked no-store"
        assert headers["ETag"], "an asset was served with no ETag"
        assert "--accent" in css, "the stylesheet did not contain its own rules"
        etag = headers["ETag"]

        # The conditional GET.
        status, headers, body = request(opener(), BASE + css_url, headers={"If-None-Match": etag})
        assert status == 304, f"If-None-Match returned {status}"
        assert body == "", "a 304 carried a body"

        # A hash that is not ours — a stale bookmark from before a redeploy.
        status, _, _ = request(opener(), BASE + "/static/qc.deadbeef.css")
        assert status == 404, f"an unknown asset hash returned {status}"

        # Traversal out of /static discloses nothing.
        out = raw_send(b"GET /static/%2e%2e%2f%2e%2e%2fetc%2fpasswd HTTP/1.1\r\nHost: x\r\n\r\n")
        assert "400" in out or "404" in out, "a traversal under /static was not refused"
        assert "root:" not in out

        # ---- CSP with assets ------------------------------------------------
        # 'self' is needed for /static and coexists with the nonce.
        _, headers, _ = request(opener(), BASE + "/login")
        csp = headers["Content-Security-Policy"]
        assert "script-src 'self' 'nonce-" in csp, csp
        assert "style-src 'self' 'nonce-" in csp, csp
        assert "'unsafe-inline'" not in csp, csp

        # But a *message body* frame must not gain 'self': it renders markup
        # written by whoever sent the mail, and 'self' would let it pull our
        # scripts. This is the one CSP in the module that must stay closed.
        deliver(
            con,
            "webuser",
            "html csp check",
            "MIME-Version: 1.0\r\nContent-Type: text/html; charset=utf-8\r\n"
            "Subject: html csp check\r\n\r\n<p>hello</p>\r\n",
        )
        mailbox = con.execute(
            "SELECT room_num FROM citadel_rooms WHERE display_name = 'Mail' "
            "AND mailbox_owner = (SELECT usernum FROM citadel_users WHERE username = 'webuser')"
        ).fetchone()[0]
        msgnum = con.execute(
            "SELECT max(msgnum) FROM citadel_room_msgs WHERE room_num = ?", [mailbox]
        ).fetchone()[0]
        # A fresh session: the logout check above revoked the one `op` held.
        frame_op, _ = sign_in(BASE, "webuser", "secret")
        status, headers, _ = request(frame_op, f"{BASE}/bbs/room/{mailbox}/msg/{msgnum}/html")
        assert status == 200, f"the HTML part returned {status}"
        frame_csp = headers["Content-Security-Policy"]
        assert "script-src" not in frame_csp or "'self'" not in frame_csp.split("script-src")[1].split(";")[0], (
            f"the message frame's CSP allows scripts from our origin: {frame_csp}"
        )
        assert "default-src 'none'" in frame_csp, frame_csp

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

    print(
        "PASS: quackmail_http (auth, CSRF, IDOR, XSS, admin gating, framing, "
        "keep-alive, static assets, timeouts)"
    )


if __name__ == "__main__":
    main()
