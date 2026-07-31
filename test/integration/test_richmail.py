#!/usr/bin/env python3
"""End-to-end test for rich (HTML) mail composition.

The assertions that matter here are all about the bytes that end up **stored**,
not about what a page renders. A display-side check would pass with the hole wide
open: composed HTML is kept and then re-served from our origin to other people —
the recipient, everyone in a public room, every subscriber to a mailing list — so
if a pasted `<script>` reaches the database, the sanitizer has already failed no
matter what any page shows.

Covers:
  * an HTML send produces multipart/alternative with *both* halves, so a
    recipient with no HTML still gets readable text;
  * a script, a handler and a javascript: URL are absent from the stored bytes;
  * a data: image becomes a real `cid:` part and the HTML refers to it;
  * the cid: route serves a real image inline but forces anything else —
    including SVG, which is scriptable — to download;
  * the message frame's CSP gains `img-src 'self'` (so cid: images load) and
    still has no `script-src 'self'` (so a hostile message cannot pull ours).

Requires: pip install duckdb==1.5.4
Run after `make` so the loadable extensions exist under build/release/extension.
"""
import base64
import http.cookiejar
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 18093
BASE = f"http://{HOST}:{PORT}"

USER = "richmail"
PASSWORD = "secret"

# A one-pixel PNG, so the inline-image path has real bytes to carry.
PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFAAH/q842iQAAAABJRU5ErkJggg==")
# An SVG is an image by content type and a script host in practice.
SVG = b'<svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script></svg>'


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


class Client:
    def __init__(self):
        self.jar = http.cookiejar.CookieJar()
        self.op = urllib.request.build_opener(
            NoRedirect(), urllib.request.HTTPCookieProcessor(self.jar))

    def get(self, path):
        try:
            r = self.op.open(BASE + path, None, timeout=20)
            return r.status, r.read(), dict(r.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read(), dict(e.headers)

    def post(self, path, fields):
        body = urllib.parse.urlencode(fields).encode()
        return self._send(path, body, "application/x-www-form-urlencoded")

    def post_multipart(self, path, fields, files=()):
        """A multipart body, which is what /mail/send requires."""
        boundary = "----qcTestBoundary9f3a"
        out = b""
        for name, value in fields.items():
            out += f"--{boundary}\r\n".encode()
            out += f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode()
            out += str(value).encode("utf-8") + b"\r\n"
        for name, filename, ctype, content in files:
            out += f"--{boundary}\r\n".encode()
            out += (f'Content-Disposition: form-data; name="{name}"; '
                    f'filename="{filename}"\r\n').encode()
            out += f"Content-Type: {ctype}\r\n\r\n".encode()
            out += content + b"\r\n"
        out += f"--{boundary}--\r\n".encode()
        return self._send(path, out, f"multipart/form-data; boundary={boundary}")

    def _send(self, path, body, ctype):
        req = urllib.request.Request(BASE + path, data=body, method="POST")
        req.add_header("Content-Type", ctype)
        try:
            r = self.op.open(req, timeout=20)
            return r.status, r.read(), dict(r.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read(), dict(e.headers)


def csrf(page):
    m = re.search(rb'name="_csrf" value="([^"]+)"', page)
    assert m, "no CSRF token on that page"
    return m.group(1).decode()


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_http"):
        con.execute(f"LOAD '{ext(name)}'")
    con.execute("SELECT count(*) FROM qm_status()").fetchall()

    con.execute(f"CALL qm_user_add('{USER}', '{PASSWORD}')")
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    con.execute(f"CALL qm_http_start('{HOST}', {PORT})")

    def stored_raw(subject):
        """The raw bytes of the message with this subject, as stored."""
        row = con.execute(
            "SELECT decode(raw) FROM citadel_messages WHERE subject = ? ORDER BY msgnum DESC LIMIT 1",
            [subject]).fetchone()
        assert row, f"no stored message with subject {subject!r}"
        return row[0]

    try:
        c = Client()
        _, page, _ = c.get("/login")
        status, _, _ = c.post("/login", {"username": USER, "password": PASSWORD,
                                        "_csrf": csrf(page)})
        assert status == 303, f"sign-in returned {status}"

        status, form, _ = c.get("/mail/compose")
        assert status == 200, f"the compose form returned {status}"
        # The form must still be a plain-text form: the editor is an enhancement,
        # so everything it needs is present but nothing depends on it running.
        assert b"data-compose" in form, "the compose form is not marked for the editor"
        assert b'name="html_body"' in form, "the compose form has no HTML field"
        assert b'name="body"' in form, "the compose form lost its textarea"
        assert b"qc-compose." in form, "the compose page does not load the editor script"
        token = csrf(form)

        # ---- a plain send stays a single part --------------------------------
        status, _, _ = c.post_multipart("/mail/send", {
            "_csrf": token, "to": f"{USER}@localhost", "cc": "", "subject": "plain only",
            "body": "Just text.", "html_body": "",
        })
        assert status == 303, f"a plain send returned {status}"
        raw = stored_raw("plain only")
        assert "Content-Type: text/plain" in raw, raw[:300]
        assert "multipart" not in raw, "a plain message was wrapped in a multipart"

        # ---- an HTML send produces both halves ------------------------------
        _, form, _ = c.get("/mail/compose")
        token = csrf(form)
        status, _, _ = c.post_multipart("/mail/send", {
            "_csrf": token, "to": f"{USER}@localhost", "cc": "", "subject": "rich",
            "body": "Plain fallback.", "rich": "1",
            "html_body": "<p>Rich <b>body</b> with a <a href=\"https://example.org/\">link</a>.</p>",
        })
        assert status == 303, f"an HTML send returned {status}"
        raw = stored_raw("rich")
        assert "multipart/alternative" in raw, f"no alternative part: {raw[:400]}"
        assert "Content-Type: text/plain" in raw, "the plain half is missing"
        assert "Content-Type: text/html" in raw, "the HTML half is missing"
        assert "Plain fallback." in raw, "the plain half lost its text"
        assert "<b>body</b>" in raw, "the HTML half lost its formatting"
        # text/plain must come first: the alternative list is least-preferred
        # first, and a client shows the last part it understands.
        assert raw.index("text/plain") < raw.index("text/html"), \
            "text/html precedes text/plain — every client would show the plain text"

        # ---- the assertion this file exists for ------------------------------
        # A pasted script must be absent from the STORED bytes. Checking a page
        # instead would pass while the database held the payload.
        _, form, _ = c.get("/mail/compose")
        token = csrf(form)
        hostile = (
            '<p>Hello</p>'
            '<script>alert(1)</script>'
            '<img src=x onerror="alert(2)">'
            '<a href="javascript:alert(3)">click</a>'
            '<iframe src="//evil"></iframe>'
            '<p style="background:url(//evil/track)">styled</p>'
            '<img src="http://evil/track.gif">'
        )
        status, _, _ = c.post_multipart("/mail/send", {
            "_csrf": token, "to": f"{USER}@localhost", "cc": "", "subject": "hostile",
            "body": "", "rich": "1", "html_body": hostile,
        })
        assert status == 303, f"a hostile send returned {status}"
        raw = stored_raw("hostile")
        for needle in ("<script", "onerror", "javascript:", "<iframe", "url(//evil",
                       "http://evil"):
            assert needle not in raw, f"{needle!r} reached the stored message:\n{raw[:600]}"
        # And the harmless parts survived, so the sanitizer is not simply eating
        # the message.
        assert "Hello" in raw and "styled" in raw and "click" in raw, \
            f"the sanitizer removed legitimate content:\n{raw[:600]}"
        # With no plain half supplied, one is derived rather than sent empty.
        assert "Hello" in raw.split("text/plain")[1][:400], \
            "the plain-text half was not derived from the HTML"

        # ---- a data: image becomes a cid: part -------------------------------
        _, form, _ = c.get("/mail/compose")
        token = csrf(form)
        data_uri = "data:image/png;base64," + base64.b64encode(PNG).decode()
        status, _, _ = c.post_multipart("/mail/send", {
            "_csrf": token, "to": f"{USER}@localhost", "cc": "", "subject": "inline",
            "body": "See image.", "rich": "1",
            "html_body": f'<p>Look: <img src="{data_uri}" alt="dot"></p>',
        })
        assert status == 303, f"an inline-image send returned {status}"
        raw = stored_raw("inline")
        assert "multipart/related" in raw, f"no related wrapper: {raw[:400]}"
        assert "Content-ID: <" in raw, "the image has no Content-ID"
        assert "Content-Type: image/png" in raw, "the image part is missing"
        assert "data:image/png;base64" not in raw, \
            "the data: URI was left in the HTML instead of becoming a part"
        m = re.search(r'src="cid:([^"]+)"', raw)
        assert m, f"the HTML does not refer to the image by cid: {raw[:600]}"
        cid = m.group(1)

        # ---- the cid: route -------------------------------------------------
        mailbox, msgnum = con.execute(
            """
            SELECT r.room_num, max(m.msgnum) FROM citadel_messages m
              JOIN citadel_room_msgs r ON r.msgnum = m.msgnum
             WHERE m.subject = 'inline' GROUP BY r.room_num LIMIT 1
            """).fetchone()

        status, body, headers = c.get(f"/bbs/room/{mailbox}/msg/{msgnum}/cid/"
                                      + urllib.parse.quote(cid, safe=""))
        assert status == 200, f"the cid route returned {status}"
        assert headers["Content-Type"] == "image/png", headers["Content-Type"]
        assert headers.get("Content-Disposition") == "inline"
        assert body == PNG, "the served bytes are not the image that was sent"

        status, _, _ = c.get(f"/bbs/room/{mailbox}/msg/{msgnum}/cid/nosuchid")
        assert status == 404, f"an unknown cid returned {status}"

        # ---- the HTML frame -------------------------------------------------
        status, page, headers = c.get(f"/bbs/room/{mailbox}/msg/{msgnum}/html")
        assert status == 200, f"the HTML part returned {status}"
        assert b"/cid/" in page, "cid: was not rewritten to a servable URL"
        assert b"cid:" not in page.replace(b"/cid/", b""), "a raw cid: reference survived"
        csp = headers["Content-Security-Policy"]
        # 'self' in img-src is what lets the rewritten cid: URL load. The frame is
        # sandboxed with no allow-same-origin, so this grants no origin access.
        assert "img-src 'self'" in csp, csp
        # And the frame must still not be able to pull our scripts.
        script_dir = csp.split("script-src")[1].split(";")[0] if "script-src" in csp else ""
        assert "'self'" not in script_dir, f"the message frame can load our scripts: {csp}"
        assert "default-src 'none'" in csp, csp

        # ---- an SVG inline part is forced to download ------------------------
        # Reached through the attachment path, since the compose allow-list
        # already refuses a data:image/svg+xml. A message from elsewhere can
        # still carry one, and it must never render from our origin.
        con.execute(
            """
            INSERT INTO citadel_messages
                (msgnum, author, recipient, msgtime, subject, format_type, raw)
            VALUES (nextval('citadel_msg_seq'), 'sender@example.org', ?, 0, 'svg test', 4, ?)
            """,
            [USER,
             ("MIME-Version: 1.0\r\n"
              'Content-Type: multipart/related; boundary="B"\r\n\r\n'
              "--B\r\n"
              "Content-Type: text/html; charset=utf-8\r\n\r\n"
              '<p><img src="cid:evil@x"></p>\r\n'
              "--B\r\n"
              "Content-Type: image/svg+xml\r\n"
              "Content-ID: <evil@x>\r\n\r\n"
              + SVG.decode() + "\r\n"
              "--B--\r\n").encode()])
        svg_msg = con.execute(
            "SELECT max(msgnum) FROM citadel_messages WHERE subject = 'svg test'").fetchone()[0]
        con.execute("INSERT INTO citadel_room_msgs (room_num, msgnum) VALUES (?, ?)",
                    [mailbox, svg_msg])

        status, body, headers = c.get(f"/bbs/room/{mailbox}/msg/{svg_msg}/cid/evil%40x")
        assert status == 200, f"the SVG cid returned {status}"
        assert headers["Content-Type"] == "application/octet-stream", \
            f"an SVG was served as {headers['Content-Type']} — it is scriptable"
        assert "attachment" in headers.get("Content-Disposition", ""), \
            "an SVG was served inline"

    finally:
        con.execute("CALL qm_http_stop()")

    print("PASS: rich mail (alternative parts, stored-side sanitizing, cid: images, frame CSP)")


if __name__ == "__main__":
    sys.exit(main())
