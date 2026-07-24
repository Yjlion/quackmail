#!/usr/bin/env python3
"""End-to-end test for the IMAP front-end (quackmail_imap).

Loads the extensions into an in-memory DuckDB, starts the IMAP listener with
STARTTLS, then drives a real imaplib client: STARTTLS + AUTHENTICATE, verify the
default Citadel folder set (INBOX + INBOX/<groupware> + <Floor>/<public>),
NAMESPACE, STATUS, APPEND, SEARCH, and COPY. Mirrors the mailbox layout a stock
Citadel Groupware server presents.

Requires: pip install duckdb==1.5.4
Run after `make` so the loadable extensions exist under build/release/extension.
"""
import imaplib
import os
import time

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 11430


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


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
    con.execute("CALL qm_user_add('imapuser', 'secret')")

    note = con.execute(
        f"SELECT note FROM qm_imap_start('{HOST}', {PORT}, starttls=>true)"
    ).fetchone()[0]
    assert note == "started", f"server did not start: {note}"
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

        M.logout()
    finally:
        con.execute("CALL qm_imap_stop()").fetchall()

    # Verify the append persisted into the Citadel message store.
    n = con.execute("SELECT count(*) FROM citadel_messages").fetchone()[0]
    assert n == 1, f"expected 1 stored message, got {n}"

    print("PASS: IMAP STARTTLS/AUTH, default folder set, STATUS, APPEND, SEARCH, COPY")


if __name__ == "__main__":
    main()
