#!/usr/bin/env python3
"""End-to-end test for the file view in the web interface.

A directory room (QR_DIRECTORY) renders as a file listing, with upload,
download, describe and delete — over the same core/filearea.hpp every other
door uses. So the checks that matter cross doors: a file uploaded in the
browser is the one WebDAV serves, and one PUT over WebDAV shows up in the page.

The permission cases are the file-area flags and the room's own rights:

  * no QR_UPLOAD, no upload form and a 403 on a forged upload;
  * no QR_DOWNLOAD, the name is listed (QR_VISDIR) but not linked, and the
    download route answers 403;
  * a user without the room's write right cannot delete or describe;
  * a download is always an attachment, never rendered on this origin.

Requires: pip install duckdb==1.5.4
Run after `make release` so the loadable extensions exist under
build/release/extension.
"""
import base64
import http.cookiejar
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

try:
    import duckdb
except ImportError:
    print("SKIP test_webfiles.py: duckdb module not installed")
    sys.exit(0)

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 18086
BASE = f"http://{HOST}:{PORT}"


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def send(op, url, data=None, content_type="application/x-www-form-urlencoded", method=None, headers=None):
    req = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", content_type)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        r = op.open(req, timeout=10)
        return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def page(op, path):
    status, headers, body = send(op, BASE + path)
    return status, headers, body.decode("utf-8", "replace")


def csrf_of(text):
    marker = 'name="_csrf" value="'
    i = text.index(marker) + len(marker)
    return text[i : text.index('"', i)]


def sign_in(user, password):
    jar = http.cookiejar.CookieJar()
    op = urllib.request.build_opener(NoRedirect(), urllib.request.HTTPCookieProcessor(jar))
    _, _, login = page(op, "/login")
    body = urllib.parse.urlencode({"_csrf": csrf_of(login), "username": user, "password": password,
                                   "next": "/bbs/"}).encode()
    status, _, _ = send(op, BASE + "/login", body)
    assert status == 303, f"login for {user} returned {status}"
    return op


def post_form(op, path, fields, csrf_from):
    _, _, text = page(op, csrf_from)
    body = dict(fields, _csrf=csrf_of(text))
    return send(op, BASE + path, urllib.parse.urlencode(body).encode())


def upload(op, room, name, content, description="", csrf_from=None):
    _, _, text = page(op, csrf_from or room)
    boundary = "----qctestboundary"
    parts = []
    for k, v in (("_csrf", csrf_of(text)), ("description", description)):
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n'.encode())
    parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{name}"\r\n'
                 f"Content-Type: application/octet-stream\r\n\r\n".encode() + content + b"\r\n")
    parts.append(f"--{boundary}--\r\n".encode())
    return send(op, BASE + room + "/upload", b"".join(parts), f"multipart/form-data; boundary={boundary}")


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_citadel", "quackmail_http"):
        con.execute(f"LOAD '{ext(name)}'")
    con.execute("SELECT count(*) FROM qm_status()").fetchall()
    for user in ("uploader", "visitor"):
        con.execute(f"CALL qm_user_add('{user}', 'secret')")
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    con.execute("CALL qm_config_set('c_fqdn', 'quackmail.test')")

    con.execute("CALL cit_room_add('Downloads')")
    con.execute("UPDATE citadel_rooms SET qr_flags = qr_flags | 32 | 64 | 128 | 256 "
                "WHERE display_name = 'Downloads'")
    # Listed but not downloadable, and no uploads: QR_DIRECTORY | QR_VISDIR.
    con.execute("CALL cit_room_add('Catalogue')")
    con.execute("UPDATE citadel_rooms SET qr_flags = qr_flags | 32 | 256 WHERE display_name = 'Catalogue'")
    con.execute("CALL cit_room_add('Plain Room')")
    num = {r: con.execute("SELECT room_num FROM citadel_rooms WHERE display_name = ?", [r]).fetchone()[0]
           for r in ("Downloads", "Catalogue", "Plain Room")}
    room = {r: f"/bbs/room/{n}" for r, n in num.items()}

    note = con.execute(f"SELECT note FROM qm_http_start('{HOST}', {PORT})").fetchone()[0]
    assert note == "started", f"http did not start: {note}"
    time.sleep(0.4)

    try:
        up = sign_in("uploader", "secret")
        vis = sign_in("visitor", "secret")

        # ---- the room page is a file listing ----------------------------------
        status, _, text = page(up, room["Downloads"])
        assert status == 200, status
        assert "This directory is empty." in text, "an empty file area did not say so"
        assert 'enctype="multipart/form-data"' in text, "no upload form in an uploadable area"
        assert f"/dav/files/uploader/{num['Downloads']}/" in text, "the WebDAV address is missing"
        status, _, text = page(up, room["Plain Room"])
        assert "This directory is empty." not in text, "a room with no directory flag rendered as files"
        # ?view=raw is the way back to the messages underneath.
        status, _, text = page(up, room["Downloads"] + "?view=raw")
        assert status == 200 and "This directory is empty." not in text

        # ---- /bbs/files and the sidebar ---------------------------------------
        status, _, text = page(up, "/bbs/files")
        assert status == 200 and "Downloads" in text and "Catalogue" in text, text[:500]
        assert "Plain Room" not in text, "a non-directory room was listed as a file area"
        assert 'href="/bbs/files"' in text, "the sidebar has no Files entry"

        # ---- upload, list, download ------------------------------------------
        blob = bytes(range(256)) * 50
        status, headers, _ = upload(up, room["Downloads"], "data.bin", blob, "Every byte value")
        assert status == 303 and "uploaded" in headers.get("Location", ""), (status, headers)
        status, _, text = page(vis, room["Downloads"])
        assert "data.bin" in text and "Every byte value" in text and "uploader" in text, text[:800]
        status, headers, body = send(vis, BASE + room["Downloads"] + "/download?name=data.bin")
        assert status == 200 and body == blob, f"download returned {status}, {len(body)} bytes"
        assert headers.get("Content-Disposition", "").startswith("attachment"), headers
        assert "sandbox" in headers.get("Content-Security-Policy", ""), headers

        # The same file over WebDAV: the page and the drive are one store.
        auth = "Basic " + base64.b64encode(b"uploader:secret").decode()
        plain = urllib.request.build_opener()
        status, _, body = send(plain, f"{BASE}/dav/files/uploader/{num['Downloads']}/data.bin",
                               headers={"Authorization": auth})
        assert status == 200 and body == blob, "the web upload is not what WebDAV serves"
        status, _, _ = send(plain, f"{BASE}/dav/files/uploader/{num['Downloads']}/from-dav.txt",
                            b"put over webdav\n", "text/plain", "PUT", {"Authorization": auth})
        assert status in (201, 204), status
        status, _, text = page(up, room["Downloads"])
        assert "from-dav.txt" in text, "a WebDAV upload is missing from the file view"

        # HTML is served as an attachment, never as a page on this origin.
        upload(up, room["Downloads"], "evil.html", b"<script>alert(1)</script>")
        status, headers, _ = send(vis, BASE + room["Downloads"] + "/download?name=evil.html")
        assert headers.get("Content-Disposition", "").startswith("attachment"), headers

        # ---- describe and delete ----------------------------------------------
        status, _, _ = post_form(up, room["Downloads"] + "/file/describe",
                                 {"name": "from-dav.txt", "description": "Put there by a drive"},
                                 room["Downloads"])
        assert status == 303, status
        _, _, text = page(up, room["Downloads"])
        assert "Put there by a drive" in text

        # In a public room everyone may post, and an ACL entry only ever adds
        # rights — so the refusal is tested with the room made read-only, which
        # is what takes the write right away from a non-aide.
        con.execute("UPDATE citadel_rooms SET qr_flags = qr_flags | 8192 WHERE display_name = 'Downloads'")
        _, _, text = page(vis, room["Downloads"])
        assert "/file/delete" not in text, "delete buttons shown to a user who cannot write"
        status, _, _ = post_form(vis, room["Downloads"] + "/file/delete", {"name": "data.bin"}, room["Downloads"])
        assert status == 403, f"a user without the write right deleted a file: {status}"
        status, _, _ = upload(vis, room["Downloads"], "nope.txt", b"x")
        assert status == 403, f"a user without the write right uploaded: {status}"

        con.execute("UPDATE citadel_rooms SET qr_flags = qr_flags & ~8192 WHERE display_name = 'Downloads'")
        status, headers, _ = post_form(up, room["Downloads"] + "/file/delete", {"name": "data.bin"},
                                       room["Downloads"])
        assert status == 303, status
        status, _, _ = send(up, BASE + room["Downloads"] + "/download?name=data.bin")
        assert status == 404, f"a deleted file still downloads: {status}"

        # ---- the flags ---------------------------------------------------------
        # Seed a file in the listed-only area through the admin path (WebDAV
        # would refuse: no QR_UPLOAD), then check it is named but not served.
        con.execute("UPDATE citadel_rooms SET qr_flags = qr_flags | 64 | 128 WHERE display_name = 'Catalogue'")
        upload(up, room["Catalogue"], "listing.txt", b"catalogue entry\n")
        con.execute("UPDATE citadel_rooms SET qr_flags = qr_flags & ~(64 | 128) WHERE display_name = 'Catalogue'")
        _, _, text = page(vis, room["Catalogue"])
        assert "listing.txt" in text, "QR_VISDIR did not list the name"
        assert "download?name=listing.txt" not in text, "a file in a no-download area was linked"
        assert 'enctype="multipart/form-data"' not in text, "an upload form in a no-upload area"
        status, _, _ = send(vis, BASE + room["Catalogue"] + "/download?name=listing.txt")
        assert status == 403, f"a no-download area served a file: {status}"
        status, _, _ = upload(up, room["Catalogue"], "forged.txt", b"x")
        assert status == 403, f"a forged upload into a no-upload area returned {status}"
        status, _, _ = send(up, BASE + room["Plain Room"] + "/download?name=anything")
        assert status == 404, f"a non-directory room answered a download: {status}"
    finally:
        con.execute("CALL qm_http_stop()")

    print("OK test_webfiles.py")


if __name__ == "__main__":
    main()
