#!/usr/bin/env python3
"""End-to-end test for the webmail composer.

Everything here is about the state the *form* is seeded with and the bytes the
send produces — the two places where compose was quietly wrong rather than
visibly broken:

  * a reply carried only the parent's Message-ID as References, so threading
    broke for every recipient from the third message on;
  * reply-all copied the original Cc and dropped the original To, which made it
    a reply to one person with extra steps;
  * a forward was inline text, so the original's attachments were silently lost;
  * `Re: ` was matched literally, so "AW:" and "Re[2]:" stacked;
  * a draft could be saved but never resumed, and Bcc did not exist.

Requires: pip install duckdb==1.5.4
Run after `make` so the loadable extensions exist under build/release/extension.
"""
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
PORT = 18099
BASE = f"http://{HOST}:{PORT}"

USER = "alice"
PASSWORD = "secret"

# The message every reply/forward case is seeded from. It has a References
# chain, a second To recipient, a Cc, and an attachment.
ORIGINAL = (
    "Message-ID: <parent@example.org>\r\n"
    "References: <root@example.org> <mid@example.org>\r\n"
    "From: Bob Barker <bob@example.org>\r\n"
    "To: alice@quackmail.test, \"Carol, Ann\" <carol@example.org>\r\n"
    "Cc: dave@example.org\r\n"
    "Subject: AW: lunch\r\n"
    "MIME-Version: 1.0\r\n"
    'Content-Type: multipart/mixed; boundary="BOUND"\r\n'
    "\r\n"
    "--BOUND\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n\r\n"
    "Where are we going?\r\n"
    "--BOUND\r\n"
    'Content-Type: application/octet-stream; name="menu.bin"\r\n'
    'Content-Disposition: attachment; filename="menu.bin"\r\n'
    "Content-Transfer-Encoding: base64\r\n\r\n"
    "AAECAwQF\r\n"
    "--BOUND--\r\n"
)


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

    def get(self, path, headers=None):
        req = urllib.request.Request(BASE + path)
        for k, v in (headers or {}).items():
            req.add_header(k, v)
        try:
            r = self.op.open(req, timeout=20)
            return r.status, r.read().decode("utf-8", "replace"), dict(r.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace"), dict(e.headers)

    def post(self, path, fields):
        body = urllib.parse.urlencode(fields).encode()
        return self._send(path, body, "application/x-www-form-urlencoded")

    def post_multipart(self, path, fields, files=()):
        """A multipart body, which is what /mail/send requires."""
        boundary = "----qcComposeBoundary41b"
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
            return r.status, r.read().decode("utf-8", "replace"), dict(r.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace"), dict(e.headers)


def csrf(page):
    m = re.search(r'name="_csrf" value="([^"]+)"', page)
    assert m, "no CSRF token on that page"
    return m.group(1)


def field(page, name):
    """The value of a form control, hidden or text."""
    m = re.search(r'name="%s"[^>]*\svalue="([^"]*)"' % re.escape(name), page)
    if not m:
        m = re.search(r'value="([^"]*)"[^>]*\sname="%s"' % re.escape(name), page)
    assert m, f"no field named {name!r} on that page"
    return m.group(1)


def unescape(v):
    return (v.replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", '"')
             .replace("&#39;", "'").replace("&amp;", "&"))


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_http"):
        con.execute(f"LOAD '{ext(name)}'")
    con.execute("SELECT count(*) FROM qm_status()").fetchall()

    con.execute(f"CALL qm_user_add('{USER}', '{PASSWORD}')")
    con.execute("CALL qm_user_add('bob', 'secret')")
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    con.execute(f"CALL qm_http_start('{HOST}', {PORT})")

    def stored_raw(subject):
        row = con.execute(
            "SELECT decode(raw) FROM citadel_messages WHERE subject = ? "
            "ORDER BY msgnum DESC LIMIT 1", [subject]).fetchone()
        assert row, f"no stored message with subject {subject!r}"
        return row[0]

    try:
        c = Client()
        _, page, _ = c.get("/login")
        status, _, _ = c.post("/login", {"username": USER, "password": PASSWORD,
                                         "_csrf": csrf(page)})
        assert status == 303, f"sign-in returned {status}"

        # The original, filed into alice's inbox.
        con.execute(
            "INSERT INTO citadel_messages (msgnum, author, recipient, msgtime, subject, "
            "format_type, raw) VALUES (nextval('citadel_msg_seq'), 'bob@example.org', ?, "
            "1700000000, 'AW: lunch', 4, ?)", [USER, ORIGINAL.encode()])
        parent = con.execute("SELECT max(msgnum) FROM citadel_messages").fetchone()[0]
        mailbox = con.execute(
            "SELECT r.room_num FROM citadel_rooms r JOIN citadel_users u "
            "ON u.usernum = r.mailbox_owner WHERE u.username = ? AND r.display_name = 'Mail'",
            [USER]).fetchone()[0]
        con.execute("INSERT INTO citadel_room_msgs (room_num, msgnum) VALUES (?, ?)",
                    [mailbox, parent])

        # ---- reply: the whole References chain -------------------------------
        status, form, _ = c.get(f"/mail/compose?room={mailbox}&reply={parent}")
        assert status == 200, f"the reply form returned {status}"
        refs = unescape(field(form, "references"))
        for one in ("<root@example.org>", "<mid@example.org>", "<parent@example.org>"):
            assert one in refs, f"{one} is missing from References: {refs!r}"
        assert refs.index("<root@example.org>") < refs.index("<parent@example.org>"), \
            "the chain is out of order — oldest first is what threading reads"
        assert unescape(field(form, "in_reply_to")) == "<parent@example.org>", \
            "In-Reply-To is not the parent"

        # A subject that already carries a reply prefix — in German — does not
        # get another one stacked on it.
        assert 'value="AW: lunch"' in form, \
            f"the subject was re-prefixed: {field(form, 'subject')!r}"

        # A plain reply goes to the sender alone.
        to = unescape(field(form, "to"))
        assert "bob@example.org" in to, f"the reply is not addressed to the sender: {to!r}"
        assert "carol@example.org" not in to, "a plain reply went to everybody"

        # ---- reply-all: the original To, minus yourself -----------------------
        status, form, _ = c.get(f"/mail/compose?room={mailbox}&reply={parent}&all=1")
        assert status == 200
        to = unescape(field(form, "to"))
        cc = unescape(field(form, "cc"))
        assert "bob@example.org" in to, f"reply-all dropped the sender: {to!r}"
        assert "carol@example.org" in to, f"reply-all dropped the original To: {to!r}"
        assert "alice@quackmail.test" not in to + cc, \
            f"reply-all addressed the message back to its author: {to!r} / {cc!r}"
        assert "dave@example.org" in cc, f"reply-all dropped the Cc: {cc!r}"
        # A display name holding a comma stays one recipient.
        assert '"Carol, Ann"' in to, f"a quoted display name was split: {to!r}"

        # ---- forward keeps the attachments ------------------------------------
        status, form, _ = c.get(f"/mail/compose?room={mailbox}&forward={parent}")
        assert status == 200
        assert field(form, "fwd_msg") == str(parent), "the forward does not name the original"
        assert 'value="Fwd: AW: lunch"' in form, "the forward subject is wrong"

        status, _, _ = c.post_multipart("/mail/send", {
            "_csrf": csrf(form), "to": "bob@example.org", "cc": "", "bcc": "",
            "subject": "Fwd: AW: lunch", "body": "See below.", "html_body": "",
            "fwd_room": str(mailbox), "fwd_msg": str(parent),
            "in_reply_to": "", "references": "", "draft_of": "0",
        })
        assert status == 303, f"the forward send returned {status}"
        raw = stored_raw("Fwd: AW: lunch")
        assert "message/rfc822" in raw, f"the original was not attached: {raw[:600]}"
        # RFC 2046 §5.2.1: an embedded message may not be base64'd. Getting this
        # wrong produces something clients offer to download rather than open.
        embedded = raw[raw.index("message/rfc822"):]
        assert "base64" not in embedded.split("\r\n\r\n")[0], \
            f"the embedded message was transfer-encoded: {embedded[:300]}"
        assert "menu.bin" in raw, "the original's own attachment did not survive"

        # ---- Bcc is envelope-only --------------------------------------------
        _, form, _ = c.get("/mail/compose")
        status, _, _ = c.post_multipart("/mail/send", {
            "_csrf": csrf(form), "to": "bob", "cc": "", "bcc": "alice",
            "subject": "quiet copy", "body": "hello", "html_body": "",
            "in_reply_to": "", "references": "", "draft_of": "0",
        })
        assert status == 303, f"the bcc send returned {status}"
        delivered = con.execute(
            "SELECT decode(m.raw) FROM citadel_messages m JOIN citadel_room_msgs rm "
            "ON rm.msgnum = m.msgnum JOIN citadel_rooms r ON r.room_num = rm.room_num "
            "JOIN citadel_users u ON u.usernum = r.mailbox_owner "
            "WHERE u.username = 'bob' AND m.subject = 'quiet copy'").fetchone()
        assert delivered, "the message never reached the To: recipient"
        assert "Bcc:" not in delivered[0], \
            "the delivered copy names the blind recipients — that is the whole point of Bcc"
        sent = con.execute(
            "SELECT decode(m.raw) FROM citadel_messages m JOIN citadel_room_msgs rm "
            "ON rm.msgnum = m.msgnum JOIN citadel_rooms r ON r.room_num = rm.room_num "
            "WHERE r.display_name = 'Sent Items' AND m.subject = 'quiet copy'").fetchone()
        assert sent and "Bcc: alice" in sent[0], \
            "the sender's own copy does not record who the blind copies went to"
        # And the blind recipient did receive it.
        blind = con.execute(
            "SELECT count(*) FROM citadel_messages m JOIN citadel_room_msgs rm "
            "ON rm.msgnum = m.msgnum JOIN citadel_rooms r ON r.room_num = rm.room_num "
            "JOIN citadel_users u ON u.usernum = r.mailbox_owner "
            "WHERE u.username = 'alice' AND r.display_name = 'Mail' "
            "AND m.subject = 'quiet copy'").fetchone()[0]
        assert blind == 1, f"the blind recipient got {blind} copies"

        # ---- drafts: save, resume, and replace --------------------------------
        _, form, _ = c.get("/mail/compose")
        token = csrf(form)
        status, _, _ = c.post_multipart("/mail/send", {
            "_csrf": token, "to": "bob@example.org", "cc": "", "bcc": "eve@example.org",
            "subject": "half written", "body": "The first half.", "html_body": "",
            "draft": "1", "in_reply_to": "<parent@example.org>",
            "references": "<root@example.org> <parent@example.org>", "draft_of": "0",
        })
        assert status == 303, f"saving a draft returned {status}"
        drafts_room = con.execute(
            "SELECT r.room_num FROM citadel_rooms r JOIN citadel_users u "
            "ON u.usernum = r.mailbox_owner WHERE u.username = ? AND r.display_name = 'Drafts'",
            [USER]).fetchone()[0]

        def draft_nums():
            return [r[0] for r in con.execute(
                "SELECT msgnum FROM citadel_room_msgs WHERE room_num = ? ORDER BY msgnum",
                [drafts_room]).fetchall()]

        nums = draft_nums()
        assert len(nums) == 1, f"saving one draft left {len(nums)}"

        status, form, _ = c.get(f"/mail/compose?draft={nums[0]}")
        assert status == 200, f"resuming a draft returned {status}"
        assert 'value="half written"' in form, "the draft's subject did not come back"
        assert "The first half." in form, "the draft's body did not come back"
        assert "bob@example.org" in unescape(field(form, "to")), "the draft's To did not come back"
        assert "eve@example.org" in unescape(field(form, "bcc")), \
            "the draft lost its blind recipients — they are only ever stored here"
        assert field(form, "draft_of") == str(nums[0]), \
            "a resumed draft does not know which draft it replaces"
        # Threading survives being saved and reopened.
        assert "<parent@example.org>" in unescape(field(form, "references")), \
            "a resumed reply lost its References chain"

        # Autosave replaces rather than accumulates, and answers with the number
        # so the form can keep replacing the same one.
        status, body, _ = c.post_multipart("/mail/draft", {
            "_csrf": csrf(form), "to": "bob@example.org", "cc": "", "bcc": "eve@example.org",
            "subject": "half written", "body": "The first half, and the second.",
            "html_body": "", "in_reply_to": "", "references": "",
            "draft_of": str(nums[0]),
        })
        assert status == 200, f"autosave returned {status}"
        assert body.strip().isdigit() and int(body) > 0, f"autosave answered {body!r}"
        after = draft_nums()
        assert len(after) == 1, f"autosaving left {len(after)} drafts instead of replacing"
        assert after[0] == int(body), "autosave did not report the draft it kept"

        # Sending a resumed draft takes it out of Drafts.
        _, form, _ = c.get(f"/mail/compose?draft={after[0]}")
        status, _, _ = c.post_multipart("/mail/send", {
            "_csrf": csrf(form), "to": "bob", "cc": "", "bcc": "",
            "subject": "half written", "body": "Finished.", "html_body": "",
            "in_reply_to": "", "references": "", "draft_of": str(after[0]),
        })
        assert status == 303, f"sending a resumed draft returned {status}"
        assert draft_nums() == [], "a sent draft is still sitting in Drafts"

        # ---- the docked form --------------------------------------------------
        # An htmx request gets the form alone, so it can be swapped into the
        # reading pane; everything else gets the whole page. Both are the same URL.
        status, frag, _ = c.get("/mail/compose", {"HX-Request": "true"})
        assert status == 200, f"the docked compose returned {status}"
        assert "data-compose" in frag, "the docked fragment is not the compose form"
        assert "<html" not in frag.lower(), "the docked fragment is a whole page"
        status, whole, _ = c.get("/mail/compose")
        assert "<html" in whole.lower(), "a plain request no longer gets a page"
        assert "qc-compose." in whole, "the compose page does not load the editor"
        assert 'name="bcc"' in whole, "there is no Bcc field"

        # ---- the address book is searched, not inlined ------------------------
        status, rows, _ = c.get("/mail/addressbook?q=nobodyatall")
        assert status == 200, f"the address book returned {status}"
        assert "nobodyatall" not in rows, "the search echoed its own query back"

    finally:
        con.execute("CALL qm_http_stop()")

    print("PASS: compose (References chain, reply-all, forward as attachment, "
          "Bcc, drafts, docked form)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
