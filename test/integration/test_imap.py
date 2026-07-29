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
import ssl
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
    finally:
        con.execute("CALL qm_imap_stop()").fetchall()
        con.execute("CALL qm_imaps_stop()").fetchall()

    # Verify the append persisted into the Citadel message store.
    n = con.execute("SELECT count(*) FROM citadel_messages").fetchone()[0]
    assert n == 1, f"expected 1 stored message, got {n}"

    print("PASS: IMAP STARTTLS/AUTH, IMAPS implicit TLS, default folder set, "
          "STATUS, APPEND, SEARCH, COPY, RFC 4314 ACLs")


if __name__ == "__main__":
    main()
