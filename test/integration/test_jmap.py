#!/usr/bin/env python3
"""End-to-end test for JMAP Core (RFC 8620) and Mail (RFC 8621).

Companion to test_imap.py, which drives the same rooms and messages over the
older protocol. The point of most assertions here is that the two agree: a
Mailbox *is* a room and an Email *is* a message, so anything JMAP reports that
IMAP would not is a second source of truth rather than a second view.

The assertions worth naming, because they are the ones that would regress
silently:

  * The parser's refusals are part of the contract. A request body is
    attacker-shaped by definition, so notJSON, notRequest and unknownCapability
    each have to come back as themselves.
  * An emailId is a number out of a request body. Every path that takes one
    checks the message is in a mailbox this user can see first, or it is a
    direct IDOR — and for EmailSubmission it would be an open relay through
    someone else's draft.
  * EmailSubmission/set charges the same rate limit the SMTP submission
    listener charges. A second door onto one mail path that skipped it would
    make the limit advisory.
  * Back-references work, including Email/set -> EmailSubmission/set in one
    request, which is how a client composes and sends in a single round trip.

Requires: pip install duckdb==1.5.4
Run after `make release`.
"""
import base64
import imaplib
import json
import os
import sys
import urllib.error
import urllib.request

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 18098
IMAP_PORT = 11438
BASE = f"http://{HOST}:{PORT}"

USER = "jmapuser"
PASSWORD = "secret"
OTHER = "jmapother"
OTHER_PASSWORD = "secret2"

CORE = "urn:ietf:params:jmap:core"
MAIL = "urn:ietf:params:jmap:mail"
SUBMIT = "urn:ietf:params:jmap:submission"


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class Jmap:
    def __init__(self, user=USER, password=PASSWORD):
        self.auth = base64.b64encode(f"{user}:{password}".encode()).decode()
        self.op = urllib.request.build_opener()

    def raw(self, method, path, body=None, headers=None, auth=True):
        req = urllib.request.Request(BASE + path, data=body, method=method)
        if auth:
            req.add_header("Authorization", "Basic " + self.auth)
        for k, v in (headers or {}).items():
            req.add_header(k, v)
        try:
            r = self.op.open(req, timeout=20)
            return r.status, dict(r.headers), r.read()
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), e.read()

    def session(self):
        status, _, body = self.raw("GET", "/.well-known/jmap")
        assert status == 200, f"session returned {status}: {body[:200]}"
        return json.loads(body)

    def call(self, calls, using=(CORE, MAIL, SUBMIT)):
        """Post a method-call array; return the methodResponses."""
        payload = json.dumps({"using": list(using), "methodCalls": calls}).encode()
        status, _, body = self.raw("POST", "/jmap/api", payload,
                                   {"Content-Type": "application/json"})
        assert status == 200, f"/jmap/api returned {status}: {body[:400]}"
        return json.loads(body)["methodResponses"]

    def one(self, name, args, using=(CORE, MAIL, SUBMIT)):
        """A single call; asserts it was not an error and returns the result."""
        r = self.call([[name, args, "c0"]], using)[0]
        assert r[0] != "error", f"{name} failed: {r[1]}"
        return r[1]


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_http", "quackmail_imap"):
        con.execute(f"LOAD '{ext(name)}'")
    con.execute("SELECT count(*) FROM qm_status()").fetchall()

    con.execute(f"CALL qm_user_add('{USER}', '{PASSWORD}')")
    con.execute(f"CALL qm_user_add('{OTHER}', '{OTHER_PASSWORD}')")
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    con.execute("CALL qm_config_set('c_fqdn', 'jmap.example.com')")
    con.execute("SELECT ok FROM qm_domain_add('jmap.example.com', 'local')").fetchall()
    con.execute(f"CALL qm_http_start('{HOST}', {PORT})")
    con.execute(f"CALL qm_imap_start('{HOST}', {IMAP_PORT})")

    try:
        j = Jmap()

        # ---- the Session resource -----------------------------------------
        # A client is handed a URL and a credential and nothing else, so
        # anything absent here is a feature it will never look for.
        status, headers, _ = j.raw("GET", "/.well-known/jmap", auth=False)
        assert status == 401, f"the session resource was served anonymously ({status})"
        assert headers.get("WWW-Authenticate", "").startswith("Basic ")

        s = j.session()
        assert CORE in s["capabilities"], f"no core capability: {list(s['capabilities'])}"
        assert MAIL in s["capabilities"]
        assert SUBMIT in s["capabilities"]
        assert s["username"] == USER
        assert USER in s["accounts"], f"accounts does not name this user: {list(s['accounts'])}"
        assert s["primaryAccounts"][MAIL] == USER
        assert s["apiUrl"] == "/jmap/api"
        assert s["capabilities"][CORE]["maxSizeUpload"] > 0, \
            "maxSizeUpload is 0, so a client will never try to attach anything"
        assert s["uploadUrl"] == "/jmap/upload/{accountId}", s["uploadUrl"]
        # There is no push, and saying so stops a client opening a stream that
        # would never carry anything.
        assert s["eventSourceUrl"] == ""

        # ---- the request envelope ------------------------------------------
        r = j.call([["Core/echo", {"hello": "world", "n": 3}, "c0"]])
        assert r[0][0] == "Core/echo", f"echo came back as {r[0][0]}"
        assert r[0][1] == {"hello": "world", "n": 3}, f"echo altered the payload: {r[0][1]}"
        assert r[0][2] == "c0", "the call id was not echoed"

        # An unknown method is a method-level error, not a failed request: the
        # other calls in the same array still have to run.
        r = j.call([["No/such", {}, "a"], ["Core/echo", {"x": 1}, "b"]])
        assert r[0][0] == "error" and r[0][1]["type"] == "unknownMethod", r[0]
        assert r[1][0] == "Core/echo", "one bad call took the whole request down"

        # Request-level problems, which are a different thing and reported as
        # RFC 7807 rather than in methodResponses.
        status, _, body = j.raw("POST", "/jmap/api", b"{not json",
                                {"Content-Type": "application/json"})
        assert status == 400 and "notJSON" in body.decode(), body[:200]

        status, _, body = j.raw("POST", "/jmap/api", b'{"using":["' + CORE.encode() + b'"]}',
                                {"Content-Type": "application/json"})
        assert status == 400 and "notRequest" in body.decode(), body[:200]

        # `using` is enforced rather than decoration: a client that omits it is
        # told so, or every client learns to skip it.
        payload = json.dumps({"methodCalls": [["Core/echo", {}, "c0"]]}).encode()
        status, _, body = j.raw("POST", "/jmap/api", payload,
                                {"Content-Type": "application/json"})
        assert status == 400 and "unknownCapability" in body.decode(), body[:200]

        # Raw bytes that are not UTF-8 never become a string. This is the half
        # of json::ValidUtf8 a SQL literal cannot reach: C0 80 is an overlong
        # NUL, the classic way past a naive filter.
        for bad in (b"\xc0\x80", b"\xed\xa0\x80", b"\xf5\x90\x80\x80", b"\xff"):
            payload = b'{"using":["' + CORE.encode() + b'"],"methodCalls":[["Core/echo",{"s":"' \
                      + bad + b'"},"c0"]]}'
            status, _, body = j.raw("POST", "/jmap/api", payload,
                                    {"Content-Type": "application/json"})
            assert status == 400, f"invalid UTF-8 {bad!r} was accepted ({status})"

        # ---- Mailbox --------------------------------------------------------
        res = j.one("Mailbox/get", {"accountId": USER})
        boxes = {m["name"]: m for m in res["list"]}
        for want in ("Mail", "Sent Items", "Drafts", "Trash"):
            assert want in boxes, f"no {want} mailbox: {sorted(boxes)}"
        assert boxes["Mail"]["role"] == "inbox", f"Mail is not the inbox: {boxes['Mail']}"
        assert boxes["Sent Items"]["role"] == "sent"
        assert boxes["Drafts"]["role"] == "drafts"
        assert boxes["Trash"]["role"] == "trash"
        assert boxes["Mail"]["parentId"] is None, "rooms do not nest"
        assert boxes["Mail"]["myRights"]["mayAddItems"] is True
        inbox = boxes["Mail"]["id"]
        drafts = boxes["Drafts"]["id"]
        sent_box = boxes["Sent Items"]["id"]

        # A public BBS room is a mailbox too — that is what IMAP does, and
        # hiding it here would make the two front-ends disagree about what the
        # account contains. But it must not carry a role.
        assert "Lobby" in boxes, f"the Lobby is not listed: {sorted(boxes)}"
        assert boxes["Lobby"]["role"] is None, \
            "a public room was given a mailbox role, so a client would file mail into it"

        res = j.one("Mailbox/query", {"accountId": USER, "filter": {"role": "inbox"}})
        assert res["ids"] == [inbox], f"the role filter returned {res['ids']}"

        # Nothing journals room creation, so the honest answer is that changes
        # cannot be calculated — a client re-fetches rather than quietly missing
        # a new folder.
        r = j.call([["Mailbox/changes", {"accountId": USER, "sinceState": "0"}, "c0"]])[0]
        assert r[0] == "error" and r[1]["type"] == "cannotCalculateChanges", r[1]

        # ---- Email/set: compose a draft --------------------------------------
        state = j.one("Mailbox/get", {"accountId": USER})["state"]
        res = j.one("Email/set", {
            "accountId": USER,
            "create": {"draft1": {
                "mailboxIds": {drafts: True},
                "keywords": {"$draft": True},
                "from": [{"name": "J User", "email": f"{USER}@jmap.example.com"}],
                "to": [{"email": f"{OTHER}@jmap.example.com"}],
                "subject": "First JMAP message",
                "bodyValues": {"b1": {"value": "Hello from JMAP.\n"}},
                "textBody": [{"partId": "b1", "type": "text/plain"}],
            }},
        })
        assert not res["notCreated"], f"the draft was refused: {res['notCreated']}"
        made = res["created"]["draft1"]
        email_id = made["id"]
        assert res["newState"] != state, "creating a message did not move the account state"

        # ---- Email/get -------------------------------------------------------
        res = j.one("Email/get", {
            "accountId": USER, "ids": [email_id], "fetchTextBodyValues": True,
        })
        assert not res["notFound"], f"the message we just made is missing: {res['notFound']}"
        e = res["list"][0]
        assert e["subject"] == "First JMAP message", e
        assert e["from"][0]["email"] == f"{USER}@jmap.example.com", e["from"]
        assert e["to"][0]["email"] == f"{OTHER}@jmap.example.com"
        assert e["keywords"].get("$draft") is True, f"keywords: {e['keywords']}"
        assert e["mailboxIds"].get(drafts) is True, f"mailboxIds: {e['mailboxIds']}"
        assert "Hello from JMAP." in list(e["bodyValues"].values())[0]["value"]
        assert e["preview"].startswith("Hello from JMAP"), e["preview"]
        assert e["hasAttachment"] is False
        # RFC 3339 in UTC, not the RFC 5322 form the header holds — a client
        # will simply fail to parse the latter.
        assert e["receivedAt"].endswith("Z") and e["receivedAt"][4] == "-", e["receivedAt"]
        assert e["sentAt"].endswith("Z"), e["sentAt"]

        # `ids` is required: an account's whole message store is not something
        # to serialize because a client forgot a filter.
        r = j.call([["Email/get", {"accountId": USER}, "c0"]])[0]
        assert r[0] == "error" and r[1]["type"] == "invalidArguments", r[1]

        # An id that is not ours, and one that is not a number at all. Neither
        # may become a lookup: "12abc" reading as 12 is an IDOR.
        res = j.one("Email/get", {"accountId": USER, "ids": ["999999", "12abc", "../etc"]})
        assert sorted(res["notFound"]) == sorted(["999999", "12abc", "../etc"]), res
        assert res["list"] == []

        # ---- Email/query and back-references ----------------------------------
        res = j.one("Email/query", {"accountId": USER, "filter": {"inMailbox": drafts},
                                    "calculateTotal": True})
        assert res["ids"] == [email_id], f"query returned {res['ids']}"
        assert res["total"] == 1

        # The whole point of the envelope: one round trip, the second call
        # taking its ids from the first.
        r = j.call([
            ["Email/query", {"accountId": USER, "filter": {"inMailbox": drafts}}, "q"],
            ["Email/get", {"accountId": USER, "#ids": {
                "resultOf": "q", "name": "Email/query", "path": "/ids"}}, "g"],
        ])
        assert r[1][0] == "Email/get", f"the back-reference failed: {r[1]}"
        assert [x["id"] for x in r[1][1]["list"]] == [email_id], r[1][1]

        # A reference to a call that did not happen is invalidResultReference,
        # not an empty id list — which would look to a client like an empty
        # mailbox rather than a bug.
        r = j.call([["Email/get", {"accountId": USER, "#ids": {
            "resultOf": "nope", "name": "Email/query", "path": "/ids"}}, "g"]])[0]
        assert r[0] == "error" and r[1]["type"] == "invalidResultReference", r[1]

        # Filters.
        res = j.one("Email/query", {"accountId": USER, "filter": {"subject": "first jmap"}})
        assert res["ids"] == [email_id], "the subject filter is case sensitive"
        res = j.one("Email/query", {"accountId": USER, "filter": {"text": "Hello from JMAP"}})
        assert res["ids"] == [email_id], "the body filter missed"
        res = j.one("Email/query", {"accountId": USER, "filter": {"hasKeyword": "$draft"}})
        assert res["ids"] == [email_id]
        res = j.one("Email/query", {"accountId": USER, "filter": {"notKeyword": "$draft"}})
        assert email_id not in res["ids"]
        res = j.one("Email/query", {"accountId": USER,
                                    "filter": {"after": "2099-01-01T00:00:00Z"}})
        assert res["ids"] == [], "a date filter in the far future matched"

        # ---- keywords, and the IMAP agreement ---------------------------------
        j.one("Email/set", {"accountId": USER,
                            "update": {email_id: {"keywords/$seen": True}}})
        res = j.one("Email/get", {"accountId": USER, "ids": [email_id],
                                  "properties": ["keywords"]})
        assert res["list"][0]["keywords"].get("$seen") is True

        # The store keeps the IMAP spelling; JMAP translates. If it stored both
        # they would drift, and a message read in one client would still be
        # unread in the other.
        flags = con.execute(
            "SELECT flag FROM citadel_msg_flags WHERE username = ? ORDER BY flag",
            [USER]).fetchall()
        assert ("\\Seen",) in flags, f"the JMAP keyword was not stored as an IMAP flag: {flags}"
        assert ("$seen",) not in flags, "the keyword was stored twice, in two spellings"

        # Setting it twice must not accumulate rows: citadel_msg_flags has no
        # unique constraint.
        j.one("Email/set", {"accountId": USER,
                            "update": {email_id: {"keywords/$seen": True}}})
        n = con.execute(
            "SELECT count(*) FROM citadel_msg_flags WHERE username = ? AND flag = '\\Seen'",
            [USER]).fetchone()[0]
        assert n == 1, f"the flag was stored {n} times"

        # ---- Email/changes -----------------------------------------------------
        before = j.one("Mailbox/get", {"accountId": USER})["state"]
        res = j.one("Email/set", {
            "accountId": USER,
            "create": {"d2": {
                "mailboxIds": {drafts: True},
                "subject": "Second",
                "bodyValues": {"b": {"value": "two"}},
                "textBody": [{"partId": "b", "type": "text/plain"}],
            }},
        })
        second_id = res["created"]["d2"]["id"]
        res = j.one("Email/changes", {"accountId": USER, "sinceState": before})
        assert second_id in res["created"], f"the new message is not in created: {res}"
        assert email_id not in res["created"], "an unchanged message was reported as new"

        # A state string we never minted has to say so, or a client quietly
        # misses everything that happened while it was away.
        r = j.call([["Email/changes", {"accountId": USER, "sinceState": "nonsense"}, "c"]])[0]
        assert r[0] == "error" and r[1]["type"] == "cannotCalculateChanges", r[1]

        # Destroy shows up as destroyed.
        before = j.one("Mailbox/get", {"accountId": USER})["state"]
        res = j.one("Email/set", {"accountId": USER, "destroy": [second_id]})
        assert res["destroyed"] == [second_id], res
        res = j.one("Email/changes", {"accountId": USER, "sinceState": before})
        assert second_id in res["destroyed"], f"the deletion is not in destroyed: {res}"

        # ---- Thread -------------------------------------------------------------
        res = j.one("Email/get", {"accountId": USER, "ids": [email_id],
                                  "properties": ["threadId"]})
        thread = res["list"][0]["threadId"]
        res = j.one("Thread/get", {"accountId": USER, "ids": [thread]})
        assert res["list"][0]["emailIds"] == [email_id], res

        # ---- Identity -----------------------------------------------------------
        res = j.one("Identity/get", {"accountId": USER})
        assert res["list"][0]["email"] == f"{USER}@jmap.example.com", res["list"]

        # ---- EmailSubmission ------------------------------------------------------
        res = j.one("EmailSubmission/set", {
            "accountId": USER,
            "create": {"send1": {"emailId": email_id, "identityId": "0"}},
            "onSuccessUpdateEmail": {"#send1": {
                "mailboxIds/" + drafts: False,
                "mailboxIds/" + sent_box: True,
                "keywords/$draft": None,
            }},
        })
        assert not res["notCreated"], f"the send was refused: {res['notCreated']}"
        sub = res["created"]["send1"]
        rcpt = f"{OTHER}@jmap.example.com"
        assert rcpt in sub["deliveryStatus"], f"no delivery status for {rcpt}: {sub}"
        # Local, so it really was stored rather than queued.
        assert sub["deliveryStatus"][rcpt]["delivered"] == "yes", sub["deliveryStatus"]

        # onSuccessUpdateEmail moved it out of Drafts and into Sent.
        res = j.one("Email/get", {"accountId": USER, "ids": [email_id],
                                  "properties": ["mailboxIds"]})
        boxes_now = res["list"][0]["mailboxIds"]
        assert boxes_now.get(sent_box) is True, f"not filed into Sent: {boxes_now}"
        assert drafts not in boxes_now, f"still in Drafts: {boxes_now}"

        # It really was delivered: the other account can see it.
        other = Jmap(OTHER, OTHER_PASSWORD)
        res = other.one("Mailbox/query", {"accountId": OTHER, "filter": {"role": "inbox"}})
        their_inbox = res["ids"][0]
        res = other.one("Email/query", {"accountId": OTHER,
                                        "filter": {"inMailbox": their_inbox}})
        assert res["ids"], "the recipient's inbox is empty"
        res = other.one("Email/get", {"accountId": OTHER, "ids": res["ids"],
                                      "properties": ["subject"]})
        assert any(x["subject"] == "First JMAP message" for x in res["list"]), res["list"]

        # A remote recipient is queued rather than claimed as delivered: the
        # relay worker has not run, and telling a client otherwise is a lie it
        # would show to a user.
        res = j.one("Email/set", {"accountId": USER, "create": {"d3": {
            "mailboxIds": {drafts: True},
            "to": [{"email": "someone@remote.example"}],
            "subject": "Outbound",
            "bodyValues": {"b": {"value": "out"}},
            "textBody": [{"partId": "b", "type": "text/plain"}],
        }}})
        remote_id = res["created"]["d3"]["id"]
        res = j.one("EmailSubmission/set", {
            "accountId": USER, "create": {"s2": {"emailId": remote_id}}})
        assert not res["notCreated"], res["notCreated"]
        st = res["created"]["s2"]["deliveryStatus"]["someone@remote.example"]
        assert st["delivered"] == "queued", f"a remote recipient was claimed delivered: {st}"
        queued = con.execute(
            "SELECT count(*) FROM quackmail_outbound WHERE rcpt = 'someone@remote.example'"
        ).fetchone()[0]
        assert queued == 1, f"the message is not on the relay queue ({queued})"

        # Compose and send in one round trip, which is the flow the envelope
        # and back-references exist for.
        r = j.call([
            ["Email/set", {"accountId": USER, "create": {"c1": {
                "mailboxIds": {drafts: True},
                "to": [{"email": f"{OTHER}@jmap.example.com"}],
                "subject": "One round trip",
                "bodyValues": {"b": {"value": "composed and sent together"}},
                "textBody": [{"partId": "b", "type": "text/plain"}],
            }}}, "set"],
            ["EmailSubmission/set", {"accountId": USER, "create": {"sub1": {
                "#emailId": {"resultOf": "set", "name": "Email/set", "path": "/created/c1/id"},
            }}}, "send"],
        ])
        assert r[1][0] == "EmailSubmission/set", f"the send failed: {r[1]}"
        assert not r[1][1]["notCreated"], f"the chained send was refused: {r[1][1]['notCreated']}"

        # ---- authorization ---------------------------------------------------------
        # Another account's id must not be usable, even with valid credentials.
        r = j.call([["Mailbox/get", {"accountId": OTHER}, "c0"]])[0]
        assert r[0] == "error" and r[1]["type"] == "accountNotFound", r[1]

        # And an emailId belonging to someone else is not readable, not
        # submittable, and not destroyable. The submission case is the sharp
        # one: it would be an open relay through another user's draft.
        their = other.one("Email/query", {"accountId": OTHER, "filter": {}})["ids"][0]
        res = j.one("Email/get", {"accountId": USER, "ids": [their]})
        assert res["notFound"] == [their], f"another account's message was readable: {res}"

        res = j.one("EmailSubmission/set", {
            "accountId": USER, "create": {"evil": {"emailId": their}}})
        assert "evil" in res["notCreated"], "another account's message was submittable"
        assert res["notCreated"]["evil"]["type"] == "notFound", res["notCreated"]

        res = j.one("Email/set", {"accountId": USER, "destroy": [their]})
        assert res["destroyed"] == [], "another account's message was destroyable"

        # ---- blob download -----------------------------------------------------------
        res = j.one("Email/get", {"accountId": USER, "ids": [email_id],
                                  "properties": ["blobId"]})
        blob = res["list"][0]["blobId"]
        status, headers, body = j.raw("GET", f"/jmap/download/{USER}/{blob}/message.eml")
        assert status == 200, f"blob download returned {status}"
        assert b"Subject:" in body, body[:200]
        # Never the sender's type and never inline: a blob served as text/html
        # from our own origin is a stored XSS with a download link.
        assert headers.get("Content-Type") == "application/octet-stream", headers.get("Content-Type")
        assert headers.get("Content-Disposition", "").startswith("attachment"), headers

        # Another account's blob, and another account's download path.
        status, _, _ = j.raw("GET", f"/jmap/download/{USER}/{their}/x.eml")
        assert status == 404, f"another account's blob was downloadable ({status})"
        status, _, _ = j.raw("GET", f"/jmap/download/{OTHER}/{blob}/x.eml")
        assert status == 404, f"the account id in the path was not checked ({status})"

        # ---- upload, and attachments -------------------------------------------------
        # The one JMAP endpoint that is not a method call. Without it a client
        # can compose text and nothing else, which is the first thing a real one
        # notices.
        blob_bytes = b"\x89PNG\r\n\x1a\n" + b"not really a png, but bytes are bytes"
        status, _, body = j.raw("POST", f"/jmap/upload/{USER}", blob_bytes,
                                {"Content-Type": "image/png"})
        assert status == 201, f"upload returned {status}: {body[:200]}"
        up = json.loads(body)
        assert up["accountId"] == USER and up["size"] == len(blob_bytes), up
        blob_id = up["blobId"]

        # Another account cannot upload into this one's namespace, and the
        # account id in the path is checked rather than decorative.
        status, _, _ = other.raw("POST", f"/jmap/upload/{USER}", b"x",
                                 {"Content-Type": "text/plain"})
        assert status == 404, f"upload accepted another account's path ({status})"

        # A random blob id is not an access rule: the row is scoped to whoever
        # uploaded it.
        status, _, _ = other.raw("GET", f"/jmap/download/{OTHER}/{blob_id}/x.png")
        assert status == 404, "another account downloaded this account's blob"

        res = j.one("Email/set", {"accountId": USER, "create": {"att": {
            "mailboxIds": {drafts: True},
            "to": [{"email": f"{OTHER}@jmap.example.com"}],
            "subject": "With an attachment",
            "bodyValues": {"b": {"value": "see attached"}},
            "textBody": [{"partId": "b", "type": "text/plain"}],
            "attachments": [{"blobId": blob_id, "type": "image/png", "name": "logo.png"}],
        }}})
        assert not res["notCreated"], f"the attachment was refused: {res['notCreated']}"
        att_id = res["created"]["att"]["id"]

        res = j.one("Email/get", {"accountId": USER, "ids": [att_id],
                                  "properties": ["hasAttachment", "attachments"]})
        e = res["list"][0]
        assert e["hasAttachment"] is True, e
        assert len(e["attachments"]) == 1, e["attachments"]
        assert e["attachments"][0]["name"] == "logo.png", e["attachments"][0]
        assert e["attachments"][0]["type"].startswith("image/png"), e["attachments"][0]

        # The bytes are *copied* into the message, not referenced. A blob is
        # temporary by JMAP's own definition, so a message that pointed at one
        # would lose its attachment the moment the staging row aged out.
        part_blob = e["attachments"][0]["blobId"]
        status, headers, got = j.raw("GET", f"/jmap/download/{USER}/{part_blob}/logo.png")
        assert status == 200, f"the attachment part did not download ({status})"
        assert got == blob_bytes, "the attachment did not survive the round trip byte for byte"
        assert headers.get("Content-Type") == "application/octet-stream", \
            "a blob was served with a sender-supplied type"

        # A blobId that is not this account's is blobNotFound, which is what
        # tells a client to re-upload rather than retry the same id forever.
        res = j.one("Email/set", {"accountId": USER, "create": {"bad": {
            "mailboxIds": {drafts: True},
            "subject": "Bad blob",
            "attachments": [{"blobId": "Udeadbeefdeadbeefdeadbeefdeadbeef", "type": "text/plain"}],
        }}})
        assert res["notCreated"]["bad"]["type"] == "blobNotFound", res["notCreated"]

        # ---- rate limiting ---------------------------------------------------------
        # The same quota the SMTP submission listener charges. A second door
        # onto one mail path that skipped it would make the limit advisory.
        con.execute(f"SELECT ok FROM qm_ratelimit_set('{USER}', 1, 3600, 100)").fetchall()
        res = j.one("Email/set", {"accountId": USER, "create": {"d9": {
            "mailboxIds": {drafts: True},
            "to": [{"email": f"{OTHER}@jmap.example.com"}],
            "subject": "Over quota",
            "bodyValues": {"b": {"value": "x"}},
            "textBody": [{"partId": "b", "type": "text/plain"}],
        }}})
        over_id = res["created"]["d9"]["id"]
        res = j.one("EmailSubmission/set", {
            "accountId": USER, "create": {"s9": {"emailId": over_id}}})
        assert "s9" in res["notCreated"], "the rate limit did not apply to JMAP"
        assert res["notCreated"]["s9"]["type"] == "overQuota", res["notCreated"]

        # ---- cross-protocol ----------------------------------------------------------
        # A Mailbox is a room and an Email is a message, so IMAP has to be
        # looking at the same things — including the read state, which is the
        # one a user notices immediately if the two disagree.
        im = imaplib.IMAP4(HOST, IMAP_PORT)
        im.login(USER, PASSWORD)
        typ, data = im.select("Sent Items")
        assert typ == "OK", f"IMAP cannot select Sent Items: {data}"
        typ, nums = im.search(None, "ALL")
        ids = nums[0].split()
        assert ids, "IMAP sees nothing in Sent Items, but JMAP filed a message there"
        typ, raw = im.fetch(ids[-1], "(FLAGS RFC822)")
        blob_txt = b"".join(p[1] for p in raw if isinstance(p, tuple)).decode("utf-8", "replace")
        assert "First JMAP message" in blob_txt, blob_txt[:300]
        flags_line = b" ".join(p[0] for p in raw if isinstance(p, tuple)).decode()
        assert "\\Seen" in flags_line, \
            f"JMAP marked it read and IMAP disagrees: {flags_line}"
        im.logout()

    finally:
        con.execute("CALL qm_http_stop()")
        con.execute("CALL qm_imap_stop()")
        con.close()

    print("PASS: JMAP Core + Mail (session, envelope, back-references, Mailbox, Email, "
          "Thread, Identity, EmailSubmission, blobs, authorization, IMAP parity)")


if __name__ == "__main__":
    sys.exit(main())
