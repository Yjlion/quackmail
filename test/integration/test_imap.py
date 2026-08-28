#!/usr/bin/env python3
"""End-to-end test for the IMAP front-end (quackmail_imap).

Loads the extensions into an in-memory DuckDB, starts the IMAP and IMAPS
listeners, then drives a real imaplib client: STARTTLS + AUTHENTICATE, verify
the default Citadel folder set (INBOX + INBOX/<groupware> + <Floor>/<public>),
NAMESPACE, STATUS, APPEND, SEARCH, and COPY, then a login over implicit TLS.
Mirrors the mailbox layout a stock Citadel Groupware server presents.

Requires: pip install duckdb==1.5.4
Run after `make` so the loadable extensions exist under build/release/extension.
"""
import imaplib
import os
import re
import ssl
import threading
import time

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 11430
PORT_TLS = 11993


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


# imaplib knows GETACL/SETACL/DELETEACL/MYRIGHTS but not LISTRIGHTS, and refuses
# to send a verb missing from this table.
imaplib.Commands.setdefault("LISTRIGHTS", ("AUTH", "SELECTED"))
# Likewise for RFC 9208 — imaplib knows none of the quota verbs.
imaplib.Commands.setdefault("GETQUOTAROOT", ("AUTH", "SELECTED"))
imaplib.Commands.setdefault("GETQUOTA", ("AUTH", "SELECTED"))
imaplib.Commands.setdefault("SETQUOTA", ("AUTH", "SELECTED"))


# The default groupware mailboxes every Citadel user gets, in IMAP naming.
EXPECTED_PERSONAL = {
    "INBOX",
    "INBOX/Calendar",
    "INBOX/Contacts",
    "INBOX/Drafts",
    "INBOX/Notes",
    "INBOX/Sent Items",
    "INBOX/Tasks",
    "INBOX/Trash",
}
EXPECTED_PUBLIC = {"Main Floor", "Main Floor/Lobby", "Main Floor/Trashcan"}


def list_names(M):
    typ, data = M.list()
    assert typ == "OK", data
    names = set()
    for d in data:
        s = d.decode() if isinstance(d, bytes) else d
        # ... "/" "<name>"  -> take the quoted name at the end
        names.add(s.rsplit('"/"', 1)[1].strip().strip('"'))
    return names


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    con.execute(f"LOAD '{ext('quackmail_citadel')}'")
    con.execute(f"LOAD '{ext('quackmail_imap')}'")
    # imapuser is an aide (axlevel 6), which is what grants the ACL "a" right;
    # otheruser stays an ordinary account. citadel_users is populated lazily on
    # first use, so the row has to be inserted rather than updated.
    for user, ax in (("imapuser", 6), ("otheruser", 4)):
        con.execute(f"CALL qm_user_add('{user}', 'secret')")
        con.execute(
            "INSERT INTO citadel_users (username, usernum, axlevel) "
            f"VALUES ('{user}', nextval('citadel_user_seq'), {ax})"
        )
    con.execute("CALL cit_room_add('Announcements')")

    note = con.execute(
        f"SELECT note FROM qm_imap_start('{HOST}', {PORT}, starttls=>true)"
    ).fetchone()[0]
    assert note == "started", f"server did not start: {note}"
    note = con.execute(
        f"SELECT note FROM qm_imaps_start('{HOST}', {PORT_TLS}, implicit_tls=>true)"
    ).fetchone()[0]
    assert note == "started", f"imaps did not start: {note}"
    time.sleep(0.3)

    try:
        M = imaplib.IMAP4(HOST, PORT)
        # CAPABILITY advertises STARTTLS before TLS, and SASL mechanisms.
        assert "STARTTLS" in M.capabilities, M.capabilities
        M.starttls()
        assert "AUTH=PLAIN" in M.capabilities, M.capabilities
        M.login("imapuser", "secret")

        # NAMESPACE: personal under INBOX/, shared under a floor path.
        typ, ns = M.namespace()
        assert typ == "OK" and b"INBOX/" in ns[0], ns

        # The default folder set matches a stock Citadel install.
        names = list_names(M)
        missing = (EXPECTED_PERSONAL | EXPECTED_PUBLIC) - names
        assert not missing, f"missing default mailboxes: {missing} (got {names})"

        # STATUS on INBOX.
        typ, data = M.status("INBOX", "(MESSAGES UIDNEXT UIDVALIDITY UNSEEN)")
        assert typ == "OK" and b"UIDVALIDITY" in data[0], data

        # APPEND a message, then find it by SEARCH (subject + body + flag).
        msg = (
            b"From: alice@example.test\r\n"
            b"To: imapuser@quackcit\r\n"
            b"Subject: Parity Probe\r\n\r\n"
            b"the body contains xyzzy\r\n"
        )
        typ, r = M.append("INBOX", r"(\Seen)", None, msg)
        assert typ == "OK", r

        M.select("INBOX")
        assert M.search(None, "ALL")[1] == [b"1"], "APPEND not visible"
        assert M.search(None, "SUBJECT", "Parity")[1] == [b"1"], "SUBJECT search"
        assert M.search(None, "BODY", "xyzzy")[1] == [b"1"], "BODY search"
        assert M.search(None, "SEEN")[1] == [b"1"], "SEEN search"
        assert M.search(None, "UNSEEN")[1] == [b""], "UNSEEN search"

        # A native (format 0) message has no header block at all in msg.raw —
        # SEARCH has to match against the same RenderRfc822 view FETCH already
        # renders, not the stored bytes, or FROM/SUBJECT can never match and
        # LARGER/SMALLER disagree with the RFC822.SIZE FETCH reports.
        con.execute(
            """
            INSERT INTO citadel_messages (msgnum, author, recipient, msgtime, subject, format_type, raw)
            VALUES (nextval('citadel_msg_seq'), 'alice', 'imapuser', epoch(now())::BIGINT,
                    'Native Probe', 0, 'the native body')
            """
        )
        con.execute(
            """
            INSERT INTO citadel_room_msgs (room_num, msgnum)
            SELECT r.room_num, (SELECT max(msgnum) FROM citadel_messages)
            FROM citadel_rooms r
            JOIN citadel_users u ON u.usernum = r.mailbox_owner
            WHERE u.username = 'imapuser' AND r.display_name = 'Mail'
            """
        )
        M.select("INBOX")
        typ, data = M.search(None, "SUBJECT", "Native")
        assert typ == "OK" and data[0], f"SUBJECT search missed a native message: {data}"
        native_seq = data[0].split()[0]
        typ, data = M.search(None, "FROM", "alice")
        assert typ == "OK" and native_seq in data[0].split(), f"FROM search missed a native message: {data}"

        typ, data = M.fetch(native_seq, "(RFC822.SIZE)")
        assert typ == "OK", data
        m = re.search(rb"RFC822\.SIZE (\d+)", data[0])
        assert m, data
        size = int(m.group(1))
        assert native_seq in M.search(None, "LARGER", str(size - 1))[1][0].split(), "LARGER missed the rendered size"
        assert native_seq not in M.search(None, "LARGER", str(size))[1][0].split(), "LARGER matched its own size"
        assert native_seq in M.search(None, "SMALLER", str(size + 1))[1][0].split(), "SMALLER missed the rendered size"

        # CREATE a folder and COPY the message into it.
        assert M.create("INBOX/Archive")[0] == "OK"
        assert M.copy("1", "INBOX/Archive")[0] == "OK"
        M.select("INBOX/Archive")
        assert M.search(None, "ALL")[1] == [b"1"], "COPY did not land"

        # ---- RFC 4314 access control -----------------------------------
        # CAPABILITY advertises ACL, as the real Citadel server does.
        assert "ACL" in M.capabilities, M.capabilities

        # An aide holds every right on a public room, including "a".
        typ, data = M._simple_command("MYRIGHTS", '"Main Floor/Announcements"')
        typ, data = M._untagged_response(typ, data, "MYRIGHTS")
        assert data and data[0].decode().endswith("lrswipkxtea"), data

        # Nothing is granted to anyone by default: the room is not mail-reachable.
        typ, data = M._simple_command("GETACL", '"Main Floor/Announcements"')
        assert typ == "OK", data
        typ, data = M._untagged_response(typ, data, "ACL")
        assert data and b"anyone" not in data[0], data

        # Grant "anyone" the post right — the opt-in for room_<name>@ mail.
        assert M._simple_command("SETACL", '"Main Floor/Announcements"', "anyone", "lrsp")[0] == "OK"
        typ, data = M._simple_command("GETACL", '"Main Floor/Announcements"')
        typ, data = M._untagged_response(typ, data, "ACL")
        assert data and b"anyone" in data[0] and b"lrsp" in data[0], data

        # The "+" and "-" forms edit the existing grant rather than replacing it.
        assert M._simple_command("SETACL", '"Main Floor/Announcements"', "anyone", "-p")[0] == "OK"
        typ, data = M._simple_command("GETACL", '"Main Floor/Announcements"')
        typ, data = M._untagged_response(typ, data, "ACL")
        assert data and b"lrs" in data[0] and b"p" not in data[0].split(b"lrs")[-1], data

        # DELETEACL removes the entry outright.
        assert M._simple_command("DELETEACL", '"Main Floor/Announcements"', "anyone")[0] == "OK"

        # LISTRIGHTS reports what may be granted.
        typ, data = M._simple_command("LISTRIGHTS", '"Main Floor/Announcements"', "anyone")
        assert typ == "OK", data

        # A mailbox nobody named does not exist.
        assert M._simple_command("MYRIGHTS", '"No Such Room"')[0] == "NO"

        # ---- IDLE (RFC 2177) -------------------------------------------------
        #
        # The whole reason IDLE exists is that a *second* session's write has to
        # reach a parked client without it asking. Extensions share no C++
        # state, so what the other connection wrote comes back out of the store
        # or not at all — the untagged response below is the only proof that
        # path works. Driven through imaplib's own IDLE support, so the wire
        # framing is checked by a client rather than by this file.
        assert "IDLE" in M.capabilities, M.capabilities
        M.select("INBOX")

        def in_background(fn, delay=0.5):
            def go():
                time.sleep(delay)
                W = imaplib.IMAP4(HOST, PORT)
                W.starttls()
                W.login("imapuser", "secret")
                W.select("INBOX")
                fn(W)
                W.logout()

            t = threading.Thread(target=go, daemon=True)
            t.start()
            return t

        def append_one(W):
            W.append("INBOX", None, None,
                     b"From: sender@example.invalid\r\nSubject: while idling\r\n"
                     b"\r\nA message that arrived without being asked for.\r\n")

        worker = in_background(append_one)
        saw = []
        with M.idle(duration=20) as idler:
            for typ, data in idler:
                saw.append((typ, data))
                if typ == "EXISTS":
                    break
        worker.join(15)
        assert any(t == "EXISTS" for t, _ in saw), \
            f"a message delivered by another session never reached the idler: {saw}"

        # Exiting the block sent DONE, and imaplib would have raised if the
        # tagged reply had not come back under the IDLE command's own tag.
        assert M.state == "SELECTED", M.state

        # An expunge from another session reaches the idler as well. The
        # positions matter: each EXPUNGE renumbers everything after it, which is
        # why they go out highest-first.
        def expunge_first(W):
            W.store("1", "+FLAGS", r"(\Deleted)")
            W.expunge()

        worker = in_background(expunge_first)
        saw = []
        with M.idle(duration=20) as idler:
            for typ, data in idler:
                saw.append((typ, data))
                if typ == "EXPUNGE":
                    break
        worker.join(15)
        assert any(t == "EXPUNGE" for t, _ in saw), \
            f"an expunge by another session never reached the idler: {saw}"

        # And the reason it could: EXPUNGE goes through citadel::DeleteMessage,
        # which records a tombstone. It used to unlink the pointer with SQL of
        # its own and leave nothing behind — so a message deleted in a mail
        # client was invisible to JMAP's Email/changes and DAV's
        # sync-collection, which is the exact hole citadel_room_tombstones was
        # added to close. IDLE only noticed because it reads the same token.
        tombs = con.execute("SELECT count(*) FROM citadel_room_tombstones").fetchone()[0]
        assert tombs >= 1, "EXPUNGE left no tombstone, so no synchronizing client can see it"

        # A client that cannot IDLE polls with NOOP, and RFC 3501 names that as
        # the point of the command. Reporting nothing there is how such a client
        # misses new mail until it re-selects.
        M.untagged_responses.pop("EXISTS", None)
        W = imaplib.IMAP4(HOST, PORT)
        W.starttls()
        W.login("imapuser", "secret")
        W.select("INBOX")
        append_one(W)
        W.logout()
        assert M.noop()[0] == "OK"
        assert "EXISTS" in M.untagged_responses, \
            f"NOOP reported no change: {M.untagged_responses}"

        M.logout()

        # An ordinary user gets read/post rights on a public room but not "a",
        # and so cannot read or rewrite the access list.
        O = imaplib.IMAP4(HOST, PORT)
        O.starttls()
        O.login("otheruser", "secret")
        typ, data = O._simple_command("MYRIGHTS", '"Main Floor/Announcements"')
        typ, data = O._untagged_response(typ, data, "MYRIGHTS")
        rights = data[0].decode().rsplit(" ", 1)[-1]
        assert "r" in rights and "a" not in rights, rights
        assert O._simple_command("GETACL", '"Main Floor/Announcements"')[0] == "NO"
        assert O._simple_command("SETACL", '"Main Floor/Announcements"', "anyone", "lrsp")[0] == "NO"
        # Someone else's personal folder does not even resolve, so this reports
        # "no such mailbox" rather than an empty rights set — which would have
        # confirmed the folder exists.
        assert O._simple_command("MYRIGHTS", '"INBOX/Archive"')[0] == "NO"
        O.logout()

        # The implicit-TLS twin (993 in production): TLS from the first byte,
        # so STARTTLS must not be advertised and LOGIN works without upgrading.
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        S = imaplib.IMAP4_SSL(HOST, PORT_TLS, ssl_context=ctx)
        assert "STARTTLS" not in S.capabilities, S.capabilities
        S.login("imapuser", "secret")
        assert S.select("INBOX")[0] == "OK"
        S.logout()
        check_quota(con)

    finally:
        con.execute("CALL qm_imap_stop()").fetchall()
        con.execute("CALL qm_imaps_stop()").fetchall()

    # Four rows: the first APPEND, the seeded native message, and the two the
    # IDLE section appended. The expunge above removed a *pointer*, not a row —
    # a message can be pointed into several rooms, so unlinking one of them
    # never deletes the bytes.
    n = con.execute("SELECT count(*) FROM citadel_messages").fetchone()[0]
    assert n == 4, f"expected 4 stored messages, got {n}"

    print("PASS: IMAP STARTTLS/AUTH, IMAPS implicit TLS, default folder set, "
          "STATUS, APPEND, SEARCH (incl. native-message rendering), COPY, "
          "RFC 4314 ACLs, QUOTA, IDLE (new mail and expunges from another session)")


def check_quota(con):
    """RFC 9208 QUOTA, including the unit every implementation gets wrong."""
    M = imaplib.IMAP4(HOST, PORT)
    M.starttls()
    M.login("imapuser", "secret")
    try:
        _check_quota(con, M)
    finally:
        M.logout()


def _check_quota(con, M):
    assert "QUOTA" in M.capabilities, M.capabilities
    assert "QUOTA=RES-STORAGE" in M.capabilities, M.capabilities

    # No ceiling: the mailbox is in no quota root, so GETQUOTAROOT names none
    # and there is no QUOTA line to follow. hardLimit has no "unlimited"
    # spelling, and absence is the accurate statement.
    typ, data = M._simple_command("GETQUOTAROOT", '"INBOX"')
    assert typ == "OK", data
    roots = M._untagged_response(typ, [None], "QUOTAROOT")[1]
    assert roots and roots[0].strip() == b'"INBOX"', f"unlimited user got a root: {roots}"

    # 10 MiB exactly, so the kibibyte conversion has an unambiguous answer.
    assert con.execute("SELECT ok FROM qm_quota_set('imapuser', 10485760)").fetchone()[0]

    typ, data = M._simple_command("GETQUOTAROOT", '"INBOX"')
    assert typ == "OK", data
    roots = M._untagged_response(typ, [None], "QUOTAROOT")[1]
    assert roots and roots[0].strip() == b'"INBOX" ""', f"wrong quota root: {roots}"
    quota = M._untagged_response(typ, [None], "QUOTA")[1]
    assert quota, "no QUOTA line followed the root"
    # STORAGE is in units of 1024 octets (RFC 9208 §5.1). 10485760 bytes is
    # 10240 KiB — reporting 10485760 here would tell every client the quota is
    # a thousand times larger than it is.
    assert b"(STORAGE " in quota[0], quota
    limit = int(quota[0].split(b"(STORAGE ")[1].split(b")")[0].split()[1])
    assert limit == 10240, f"STORAGE limit reported as {limit}, wanted 10240 KiB"

    # A public room belongs to nobody, so it is in no root whatever the user's
    # own ceiling is.
    typ, data = M._simple_command("GETQUOTAROOT", '"Announcements"')
    assert typ == "OK", data
    roots = M._untagged_response(typ, [None], "QUOTAROOT")[1]
    assert roots and roots[0].strip() == b'"Announcements"', f"public room got a root: {roots}"

    # GETQUOTA on the one root we have, and NO on anything else.
    typ, data = M._simple_command("GETQUOTA", '""')
    assert typ == "OK", data
    assert M._untagged_response(typ, [None], "QUOTA")[1], "GETQUOTA returned no line"
    typ, data = M._simple_command("GETQUOTA", '"nosuchroot"')
    assert typ == "NO", f"an unknown quota root returned {typ}"

    # SETQUOTA is an administrator's command; imapuser is an aide.
    typ, data = M._simple_command("SETQUOTA", '"" (STORAGE 20480)')
    assert typ == "OK", data
    stored = con.execute(
        "SELECT limit_bytes FROM qm_quotas() WHERE username = 'imapuser'").fetchone()[0]
    assert stored == 20971520, f"SETQUOTA stored {stored}, wanted 20480 KiB in bytes"

    N = imaplib.IMAP4(HOST, PORT)
    N.starttls()
    N.login("otheruser", "secret")
    try:
        typ, data = N._simple_command("SETQUOTA", '"" (STORAGE 1)')
        assert typ == "NO", f"an ordinary user set a quota: {typ}"
        assert b"NOPERM" in data[0], f"wrong response code: {data}"
    finally:
        N.logout()

    # An APPEND that would cross the ceiling is refused *before* the
    # continuation, so the literal never leaves the client and the connection
    # stays in sync for the next command.
    assert con.execute("SELECT ok FROM qm_quota_set('imapuser', 1)").fetchone()[0]
    typ, data = M.append("INBOX", "", None, b"From: a@b\r\nSubject: too big\r\n\r\nx\r\n")
    assert typ == "NO", f"an over-quota APPEND returned {typ}"
    assert b"OVERQUOTA" in data[0], f"wrong response code: {data}"
    # The session is still usable — a desync here would strand the client.
    assert M.noop()[0] == "OK", "the connection desynced after a refused APPEND"

    assert con.execute("SELECT ok FROM qm_quota_set('imapuser', 0)").fetchone()[0]


if __name__ == "__main__":
    main()
