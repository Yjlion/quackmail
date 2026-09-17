#!/usr/bin/env python3
"""End-to-end test for the FTP/FTPS listener (quackmail_ftp).

FTP here is a third front door onto the *file areas* -- the same rooms, the same
QR_UPLOAD/QR_DOWNLOAD/QR_VISDIR flags and the same permission questions the
telnet `.RF` family and /dav/files/ ask. So the assertions that matter are less
about FTP than about that: a file stored over FTP has to *be* a message in a
room, and one stored anywhere else has to be fetchable here.

What this pins, beyond the happy path:

  * cleartext credentials are refused by default. Every other protocol in this
    tree has a TLS story, and a password over plain FTP should be the opt-in.
  * PORT/EPRT are refused rather than unimplemented: active mode makes the
    server dial an address the client names.
  * a room without QR_DIRECTORY is not a directory, and one the user cannot see
    is not listed.
  * the storage quota's scope: it counts a user's *own* rooms, so a shared
    file area is charged to nobody -- which is neither a way around a ceiling
    nor a way to exhaust somebody else's.

Requires: pip install duckdb==1.5.4
Run after `make` so the loadable extensions exist under build/release/extension.
"""
import base64
import ftplib
import io
import os
import ssl
import sys
import urllib.request
import time

try:
    import duckdb
except ImportError:
    print("SKIP test_ftp.py: duckdb module not installed")
    sys.exit(0)

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 12121
USER = "ftpuser"
PASSWORD = "secret"
OTHER = "ftpother"


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def connect():
    c = ftplib.FTP()
    c.connect(HOST, PORT, timeout=10)
    return c


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_citadel", "quackmail_ftp", "quackmail_http"):
        con.execute(f"LOAD '{ext(name)}'")
    con.execute(f"CALL qm_user_add('{USER}', '{PASSWORD}')")
    con.execute(f"CALL qm_user_add('{OTHER}', '{PASSWORD}')")

    # QR_DIRECTORY|QR_UPLOAD|QR_DOWNLOAD|QR_VISDIR = 32|64|128|256.
    con.execute("CALL cit_room_add('Drop Box')")
    con.execute("UPDATE citadel_rooms SET qr_flags = qr_flags | 32 | 64 | 128 | 256 "
                "WHERE display_name = 'Drop Box'")
    # A room with no directory flag at all, which must not appear as one.
    con.execute("CALL cit_room_add('Just A Room')")
    room = con.execute(
        "SELECT room_num FROM citadel_rooms WHERE display_name = 'Drop Box'").fetchone()[0]

    note = con.execute(f"SELECT note FROM qm_ftp_start('{HOST}', {PORT})").fetchone()[0]
    assert note == "started", f"listener did not start: {note}"
    time.sleep(0.3)

    try:
        # ---- cleartext is refused by default -----------------------------
        c = connect()
        try:
            c.login(USER, PASSWORD)
            raise AssertionError("a cleartext login was accepted with no TLS and no opt-in")
        except ftplib.error_perm as e:
            assert str(e).startswith("534"), f"the refusal was not 534: {e}"
        c.close()

        # The opt-in exists and works, which is what proves the refusal above
        # was policy rather than a broken login path.
        con.execute("CALL qm_config_set('qm_ftp_allow_cleartext', '1')")

        c = connect()
        c.login(USER, PASSWORD)

        # ---- the filesystem ----------------------------------------------
        assert c.pwd() == "/", f"the root is not /: {c.pwd()}"
        names = c.nlst()
        assert "Drop Box" in names, f"the file area is missing from the root: {names}"
        assert "Just A Room" not in names, "a room with no directory flag was listed as one"

        c.cwd("Drop Box")
        assert c.pwd() == "/Drop Box", c.pwd()
        assert c.nlst() == [], f"a fresh area is not empty: {c.nlst()}"

        # ---- a binary round trip -----------------------------------------
        # Every byte 0-255: a file area holds whatever was put in it, and a
        # text-oriented path would truncate at the NUL.
        blob = bytes(range(256)) * 4
        c.storbinary("STOR blob.bin", io.BytesIO(blob))

        got = io.BytesIO()
        c.retrbinary("RETR blob.bin", got.write)
        assert got.getvalue() == blob, (
            f"the file did not round-trip: {len(got.getvalue())} of {len(blob)}")

        # A file is a message. That is the storage design, and it is what makes
        # quotas, the room ACL and tombstones apply without being told about
        # files.
        rows = con.execute(
            "SELECT count(*) FROM citadel_messages m JOIN citadel_room_msgs rm USING (msgnum) "
            f"WHERE rm.room_num = {room} AND m.euid = 'file/blob.bin'").fetchone()[0]
        assert rows == 1, f"the upload did not land as a message ({rows})"

        assert c.size("blob.bin") == len(blob), f"SIZE is wrong: {c.size('blob.bin')}"
        assert c.sendcmd("MDTM blob.bin").startswith("213 "), "MDTM did not answer"

        # MLSD is the machine-readable listing; LIST is the ls-shaped one for
        # the clients that still parse it.
        entries = {name: facts for name, facts in c.mlsd()}
        assert "blob.bin" in entries, f"MLSD did not list the file: {entries}"
        assert entries["blob.bin"]["type"] == "file", entries["blob.bin"]
        assert int(entries["blob.bin"]["size"]) == len(blob), entries["blob.bin"]

        listing = []
        c.retrlines("LIST", listing.append)
        assert any("blob.bin" in l for l in listing), f"LIST did not show the file: {listing}"
        assert listing[0].startswith("-"), f"a file is listed as a directory: {listing[0]}"

        # ---- rename and delete -------------------------------------------
        c.rename("blob.bin", "renamed.bin")
        got = io.BytesIO()
        c.retrbinary("RETR renamed.bin", got.write)
        assert got.getvalue() == blob, "the rename did not carry the bytes"
        assert "blob.bin" not in c.nlst(), "the old name survived the rename"

        c.delete("renamed.bin")
        assert c.nlst() == [], f"the delete left something behind: {c.nlst()}"

        # ---- refusals -----------------------------------------------------
        # Active mode is a port-scanning primitive and useless behind NAT.
        for verb in ("PORT 127,0,0,1,4,1", "EPRT |1|127.0.0.1|1025|"):
            try:
                c.sendcmd(verb)
                raise AssertionError(f"{verb} was accepted")
            except ftplib.error_perm as e:
                assert str(e).startswith("502"), f"{verb} was refused with {e}"

        # A file area has no subdirectories, because a Citadel room has no
        # sub-rooms.
        try:
            c.cwd("nested/deeper")
            raise AssertionError("a nested path was accepted")
        except ftplib.error_perm:
            pass

        # ---- MKD creates a room ------------------------------------------
        c.cwd("/")
        try:
            c.mkd("Made By Ftp")
            raise AssertionError("a non-aide created a file area")
        except ftplib.error_perm as e:
            assert str(e).startswith("550"), f"MKD was refused with {e}"

        con.execute("CALL qm_config_set('qm_room_create_axlevel', '1')")
        c.mkd("Made By Ftp")
        flags = con.execute(
            "SELECT qr_flags FROM citadel_rooms WHERE display_name = 'Made By Ftp'").fetchone()[0]
        assert flags & 32, f"MKD made a room that is not a directory: {flags}"
        assert "Made By Ftp" in c.nlst(), "the new area is not listed"

        # And the creator can write to it straight away -- the rights grant.
        c.cwd("Made By Ftp")
        c.storbinary("STOR hello.txt", io.BytesIO(b"hi"))
        assert "hello.txt" in c.nlst(), "the creator could not write to their own area"
        c.cwd("/")

        # ---- the storage quota's scope -------------------------------------
        # Worth pinning because it is not the obvious answer. The per-user
        # storage quota counts the messages in rooms that user *owns*
        # (citadel_rooms.mailbox_owner), so a shared file area belongs to nobody
        # and an upload into one is charged to nobody. A drop box is therefore
        # not a way around a ceiling, and it is also not a way to exhaust one
        # somebody else is under.
        #
        # Enforcement itself lives inside InsertMessage rather than at any front
        # door -- that is what stops a new protocol leaking a ceiling -- and is
        # asserted where it bites, in test_caldav.py against an owned room.
        c.cwd("Drop Box")
        con.execute(f"SELECT ok FROM qm_quota_set('{USER}', 1)").fetchall()
        try:
            c.storbinary("STOR shared.bin", io.BytesIO(b"x" * 4096))
            assert "shared.bin" in c.nlst(), "an upload into a shared area was refused"
            owner = con.execute(
                f"SELECT mailbox_owner FROM citadel_rooms WHERE room_num = {room}").fetchone()[0]
            assert not owner, f"the shared area has an owner ({owner}); the test proves nothing"
            c.delete("shared.bin")
        finally:
            con.execute(f"SELECT ok FROM qm_quota_set('{USER}', 0)").fetchall()

        c.quit()

        # ---- another user cannot see a room they have no rights to ---------
        con.execute("UPDATE citadel_rooms SET qr_flags = qr_flags | 4 "
                    "WHERE display_name = 'Drop Box'")  # QR_PRIVATE
        o = connect()
        o.login(OTHER, PASSWORD)
        assert "Drop Box" not in o.nlst(), "a private file area is visible to an outsider"
        o.quit()

        # ---- one store, another front door ----------------------------------
        # The same assertion test_telnet.py makes for telnet->WebDAV, from the
        # third door: bytes stored over FTP come back over WebDAV unchanged.
        # Without it, "three front doors over one store" is a claim.
        con.execute("UPDATE citadel_rooms SET qr_flags = qr_flags & ~4 "
                    "WHERE display_name = 'Drop Box'")
        con.execute("CALL qm_config_set('qm_web_force_https', '0')")
        web = 12122
        note = con.execute(f"SELECT note FROM qm_http_start('{HOST}', {web})").fetchone()[0]
        assert note == "started", f"http listener did not start: {note}"
        time.sleep(0.3)
        try:
            payload = bytes(range(256))
            c = connect()
            c.login(USER, PASSWORD)
            c.cwd("Drop Box")
            c.storbinary("STOR crossover.bin", io.BytesIO(payload))
            c.quit()

            req = urllib.request.Request(
                f"http://{HOST}:{web}/dav/files/{USER}/{room}/crossover.bin", method="GET")
            req.add_header("Authorization", "Basic " +
                           base64.b64encode(f"{USER}:{PASSWORD}".encode()).decode())
            got = urllib.request.build_opener().open(req, timeout=10).read()
            assert got == payload, (
                f"the FTP upload did not come back over WebDAV byte for byte: "
                f"{len(got)} of {len(payload)}")
        finally:
            con.execute("CALL qm_http_stop()").fetchall()

    finally:
        con.execute("CALL qm_ftp_stop()").fetchall()
        con.close()

    print("PASS: FTP (cleartext refusal, binary round trip, MLSD/LIST, rename, MKD, "
          "quota scope, privacy, and the FTP->WebDAV crossover)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
