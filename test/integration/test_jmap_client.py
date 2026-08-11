#!/usr/bin/env python3
"""End-to-end test for JMAP driven by a third-party client library.

Companion to test_jmap.py, which asserts the wire format directly. This one
asserts something test_jmap.py structurally cannot: that a JMAP client written
by somebody with no stake in this server can actually use it. The client is
`jmapc` (https://github.com/smkent/jmapc), and every request here goes through
its typed models, so a response shape it cannot deserialise fails by
construction rather than by an assertion somebody remembered to write.

This test exists because the CalDAV work had the same gap and it cost a design
error: resource names were required to equal the object UID, test_caldav.py
passed the whole time, and vdirsyncer — which names resources with a UUID of its
own — could not sync at all. JMAP had no equivalent probe. Pointing this one at
it immediately found that apiUrl, uploadUrl and downloadUrl were site-relative
paths, so `requests` refused every method call, every upload and every download
with "No scheme supplied": the Session resource parsed, and nothing after it
worked.

The assertions worth naming, because they are the ones that would regress
silently:

  * The Session URLs are absolute and carry the port. A client uses them
    verbatim; it does not resolve them against the request URI the way it
    resolves a DAV href.
  * primaryAccounts names the core capability. jmapc reads that key first, and
    a client is entitled to pick any of the three.
  * A round trip through somebody else's serializer: Email/set writes a
    message and Email/get reads it back into the client's own Email model,
    with the addresses and subject intact.

Requires: pip install duckdb==1.5.4 jmapc
Run after `make release`. Skips itself if jmapc is not installed, so the rest
of the suite stays runnable without it.
"""
import os
import sys

try:
    import jmapc
    from jmapc import methods
except ImportError:  # pragma: no cover - exercised only without the dependency
    print("SKIP: jmapc is not installed (pip install jmapc)")
    sys.exit(0)

import duckdb
import urllib3

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 18101
PORT_TLS = 18102

USER = "jclient"
PASSWORD = "secret"
OTHER = "jclientother"
OTHER_PASSWORD = "secret2"
FQDN = "jmap.example.com"

CORE = "urn:ietf:params:jmap:core"
MAIL = "urn:ietf:params:jmap:mail"
SUBMIT = "urn:ietf:params:jmap:submission"

# The listener presents the ephemeral self-signed certificate core/src/tls.cpp
# generates. Verifying it is not what this test is about.
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def client(user=USER, password=PASSWORD):
    # jmapc builds its discovery URL as https://<host>/.well-known/jmap, so the
    # test has to reach the implicit-TLS listener; there is no plaintext mode to
    # fall back to. That is itself worth pinning: it is how a real client will
    # arrive.
    c = jmapc.Client.create_with_password(
        host=f"{HOST}:{PORT_TLS}", user=user, password=password
    )
    c.requests_session.verify = False
    return c


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_http"):
        con.execute(f"LOAD '{ext(name)}'")
    con.execute("SELECT count(*) FROM qm_status()").fetchall()

    con.execute(f"CALL qm_user_add('{USER}', '{PASSWORD}')")
    con.execute(f"CALL qm_user_add('{OTHER}', '{OTHER_PASSWORD}')")
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    con.execute(f"CALL qm_config_set('c_fqdn', '{FQDN}')")
    con.execute(f"SELECT ok FROM qm_domain_add('{FQDN}', 'local')").fetchall()
    con.execute(f"CALL qm_http_start('{HOST}', {PORT})")
    con.execute(
        f"SELECT note FROM qm_https_start('{HOST}', {PORT_TLS}, implicit_tls=>true)"
    ).fetchall()

    try:
        c = client()

        # ---- the Session resource ------------------------------------------
        s = c.jmap_session
        assert s.username == USER, f"session names {s.username!r}"

        # Absolute, and carrying the port. c_fqdn cannot supply a port, so a
        # base derived from it would describe every server not on 80/443
        # wrongly -- which is most of them, including this one.
        want = f"https://{HOST}:{PORT_TLS}"
        assert s.api_url == want + "/jmap/api", f"apiUrl is {s.api_url!r}"
        assert s.upload_url == want + "/jmap/upload/{accountId}", \
            f"uploadUrl is {s.upload_url!r}"
        assert s.download_url.startswith(want + "/jmap/download/"), \
            f"downloadUrl is {s.download_url!r}"

        # A client is entitled to read any of the three; jmapc reads core
        # first. Omitting it made account_id depend on the fallback chain.
        assert s.primary_accounts.core == USER, \
            f"primaryAccounts has no core entry: {s.primary_accounts}"
        assert s.primary_accounts.mail == USER
        assert s.primary_accounts.submission == USER
        assert c.account_id == USER, f"account_id resolved to {c.account_id!r}"

        assert s.capabilities.urns == {CORE, MAIL, SUBMIT}, \
            f"advertised capabilities are {sorted(s.capabilities.urns)}"
        assert s.capabilities.core.max_size_upload > 0, \
            "maxSizeUpload is 0, so a client will never try to attach anything"

        # ---- the request envelope, through somebody else's serializer -------
        r = c.request(methods.CoreEcho(data={"hello": "world", "n": 3}))
        assert isinstance(r, methods.CoreEchoResponse), f"echo came back as {r!r}"
        assert r.data == {"hello": "world", "n": 3}, f"echo altered the payload: {r.data}"

        # ---- Mailbox --------------------------------------------------------
        r = c.request(methods.MailboxGet(ids=None))
        assert isinstance(r, methods.MailboxGetResponse), f"Mailbox/get returned {r!r}"
        boxes = {m.name: m for m in r.data}
        for want_box in ("Mail", "Sent Items", "Drafts", "Trash"):
            assert want_box in boxes, f"no {want_box} mailbox: {sorted(boxes)}"
        assert boxes["Mail"].role == "inbox", f"Mail is not the inbox: {boxes['Mail']}"
        assert boxes["Drafts"].role == "drafts"
        assert boxes["Lobby"].role is None, \
            "a public room was given a mailbox role, so a client would file mail into it"
        drafts = boxes["Drafts"].id

        # ---- Email round trip ------------------------------------------------
        subject = "Written by a stranger's client"
        r = c.request(methods.EmailSet(create={"draft": jmapc.Email(
            mailbox_ids={drafts: True},
            keywords={"$draft": True},
            mail_from=[jmapc.EmailAddress(name="J Client", email=f"{USER}@{FQDN}")],
            to=[jmapc.EmailAddress(email=f"{OTHER}@{FQDN}")],
            subject=subject,
            body_values={"b1": jmapc.EmailBodyValue(value="Hello from jmapc.\n")},
            text_body=[jmapc.EmailBodyPart(part_id="b1", type="text/plain")],
        )}))
        assert isinstance(r, methods.EmailSetResponse), f"Email/set returned {r!r}"
        assert not r.not_created, f"Email/set refused the draft: {r.not_created}"
        made = r.created["draft"]

        r = c.request(methods.EmailQuery(limit=10, filter=jmapc.EmailQueryFilterCondition(
            in_mailbox=drafts)))
        assert isinstance(r, methods.EmailQueryResponse), f"Email/query returned {r!r}"
        assert made.id in r.ids, f"the new draft is not in {r.ids}"

        r = c.request(methods.EmailGet(ids=[made.id], fetch_all_body_values=True))
        assert isinstance(r, methods.EmailGetResponse), f"Email/get returned {r!r}"
        assert len(r.data) == 1, f"Email/get returned {len(r.data)} messages"
        got = r.data[0]
        # The point of these four: they came back through the client's own
        # deserializer, not through a dict this test built expectations for.
        assert got.subject == subject, f"subject came back as {got.subject!r}"
        assert got.mail_from[0].email == f"{USER}@{FQDN}", f"From is {got.mail_from}"
        assert got.to[0].email == f"{OTHER}@{FQDN}", f"To is {got.to}"
        assert got.received_at is not None, "no receivedAt, so a client cannot sort"

        # ---- blob upload and download ---------------------------------------
        # Both use a Session URL a client formats and calls verbatim, which is
        # the other half of what the relative paths broke.
        blob_path = os.path.join(
            os.environ.get("TMPDIR", "/tmp"), "quackcit_jmap_client_blob.txt"
        )
        payload = b"uploaded by jmapc\n"
        with open(blob_path, "wb") as f:
            f.write(payload)
        try:
            blob = c.upload_blob(blob_path)
        finally:
            os.unlink(blob_path)
        assert blob.id, f"upload returned no blob id: {blob}"
        assert blob.size == len(payload), f"upload reported size {blob.size}"

        url = s.download_url.format(
            accountId=c.account_id, blobId=blob.id, name="blob.txt", type="text/plain"
        )
        resp = c.requests_session.get(url, timeout=30)
        assert resp.status_code == 200, f"download returned {resp.status_code}"
        assert resp.content == payload, f"download returned {resp.content!r}"

        # ---- one door per account -------------------------------------------
        # The client asks for its own account by name on every call. Another
        # user's blob is not reachable with it, which is the check that keeps a
        # random-looking id from being the access rule.
        other = client(OTHER, OTHER_PASSWORD)
        stolen = s.download_url.format(
            accountId=OTHER, blobId=blob.id, name="blob.txt", type="text/plain"
        )
        resp = other.requests_session.get(stolen, timeout=30)
        assert resp.status_code == 404, \
            f"another user downloaded this blob ({resp.status_code})"

    finally:
        con.execute("CALL qm_https_stop()")
        con.execute("CALL qm_http_stop()")

    print("PASS: JMAP through the jmapc client (session URLs, primaryAccounts, "
          "Mailbox, Email round trip, blob upload/download)")


if __name__ == "__main__":
    main()
