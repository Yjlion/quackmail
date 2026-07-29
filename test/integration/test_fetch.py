#!/usr/bin/env python3
"""End-to-end test for pulling messages in from a POP3, IMAP or RSS source.

The RSS half is asserted offline in test/sql/feed.test, because feed::Parse is a
pure function. What needs a socket is everything around it: the POP3 and IMAP
*clients*, the seen-set that makes a poll idempotent, and posting into a room.

For the two mail protocols the far end is **QuackCit's own POP3 and IMAP
listeners**. That is not a shortcut — it exercises the new clients against a
real server speaking the real protocol, with no fixture server to drift out of
date, and any disagreement between our client and our server shows up here
rather than in production. RSS gets a throwaway http.server, which also lets the
conditional-request path (ETag, 304) be driven deliberately.

Requires: pip install duckdb==1.5.4. Run after `make`.
"""
import http.server
import os
import poplib
import threading
import time

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
POP_PORT = 3710
IMAP_PORT = 3743
HTTP_PORT = 3780

FEED_V1 = b"""<?xml version="1.0"?>
<rss version="2.0">
  <channel>
    <title>Test Feed</title>
    <link>https://example.invalid/</link>
    <item>
      <title>Item one</title>
      <link>https://example.invalid/1</link>
      <guid>guid-1</guid>
      <pubDate>Mon, 27 Jul 2026 10:00:00 +0000</pubDate>
      <description><![CDATA[<p>The <b>first</b> item.</p>]]></description>
    </item>
    <item>
      <title>Item two</title>
      <link>https://example.invalid/2</link>
      <guid>guid-2</guid>
      <description>The second item.</description>
    </item>
  </channel>
</rss>
"""

FEED_V2 = FEED_V1.replace(
    b"    <item>\n      <title>Item one</title>",
    b"""    <item>
      <title>Item three</title>
      <link>https://example.invalid/3</link>
      <guid>guid-3</guid>
      <description>A newer item.</description>
    </item>
    <item>
      <title>Item one</title>""",
)


class FeedHandler(http.server.BaseHTTPRequestHandler):
    """Serves the feed with an ETag, and honours If-None-Match."""

    body = FEED_V1
    etag = '"v1"'
    hits = []

    def do_GET(self):
        FeedHandler.hits.append(self.headers.get("If-None-Match"))
        if self.headers.get("If-None-Match") == FeedHandler.etag:
            self.send_response(304)
            self.send_header("ETag", FeedHandler.etag)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/rss+xml")
        self.send_header("ETag", FeedHandler.etag)
        # Chunked on purpose: real feed servers do it, and the server codec in
        # core/http.cpp deliberately refuses chunking, so the client's own
        # decoder is the only thing that can read this.
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        body = FeedHandler.body
        for i in range(0, len(body), 64):
            chunk = body[i:i + 64]
            self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
        self.wfile.write(b"0\r\n\r\n")

    def log_message(self, *args):
        pass


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    for name in ("quackmail", "quackmail_pop3", "quackmail_imap", "quackmail_spool"):
        con.execute(f"LOAD '{ext(name)}'")

    assert con.execute("SELECT ok FROM qm_user_add('sender', 'secret')").fetchone()[0]

    def rows_in(room):
        return con.execute(
            "SELECT m.subject FROM citadel_messages m "
            "JOIN citadel_room_msgs rm ON rm.msgnum = m.msgnum "
            "JOIN citadel_rooms r ON r.room_num = rm.room_num "
            f"WHERE r.display_name = '{room}' ORDER BY m.msgnum"
        ).fetchall()

    def run(feed):
        return con.execute(
            "SELECT fetched, stored, skipped, status, error "
            f"FROM qm_fetch_run(feed => '{feed}')"
        ).fetchone()

    assert con.execute("SELECT ok FROM cit_room_add('Pulled')").fetchone()[0]
    assert con.execute("SELECT ok FROM cit_room_add('Newsroom')").fetchone()[0]

    for call in (
        f"SELECT note FROM qm_pop3_start('{HOST}', {POP_PORT})",
        f"SELECT note FROM qm_imap_start('{HOST}', {IMAP_PORT})",
    ):
        note = con.execute(call).fetchone()[0]
        assert note == "started", f"listener did not start: {note}"

    httpd = http.server.ThreadingHTTPServer((HOST, HTTP_PORT), FeedHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    time.sleep(0.4)

    # The Mail room is provisioned on login (EnsureUserRooms), so sign in once
    # before seeding it — exactly as test_pop3.py does.
    m = poplib.POP3(HOST, POP_PORT, timeout=5)
    m.user("sender")
    m.pass_("secret")
    m.quit()

    mail_room = con.execute(
        "SELECT r.room_num FROM citadel_rooms r JOIN citadel_users u ON u.usernum = r.mailbox_owner "
        "WHERE u.username = 'sender' AND r.display_name = 'Mail'"
    ).fetchone()
    assert mail_room is not None, "sender has no Mail room"

    # Seed the source mailbox: three messages in sender's Mail room, which is
    # what our own POP3 and IMAP listeners serve.
    for n in (1, 2, 3):
        msgnum = con.execute("SELECT nextval('citadel_msg_seq')").fetchone()[0]
        con.execute(
            "INSERT INTO citadel_messages (msgnum, author, recipient, msgtime, subject, "
            f"format_type, origin_room, raw) VALUES ({msgnum}, 'someone', 'sender', 1753660800, "
            f"'Upstream {n}', 0, 'Mail', 'Body of message {n}.')"
        )
        con.execute(f"INSERT INTO citadel_room_msgs VALUES ({mail_room[0]}, {msgnum})")
        con.execute(
            f"UPDATE citadel_rooms SET highest_msg = {msgnum} WHERE room_num = {mail_room[0]}"
        )

    try:
        # ---- POP3 -------------------------------------------------------

        ok, note = con.execute(
            "SELECT ok, note FROM qm_feed_add('selfpop', 'pop3', "
            f"'sender:secret@{HOST}:{POP_PORT}', 'Pulled')"
        ).fetchone()
        assert ok, note
        assert con.execute(
            "SELECT ok FROM qm_feed_set('selfpop', 'tls', 'none')"
        ).fetchone()[0]

        ok, note = con.execute("SELECT ok, note FROM qm_feed_test('selfpop')").fetchone()
        assert ok, note
        assert "3 message" in note, note

        fetched, stored, skipped, status, error = run("selfpop")
        assert status == "ok", (status, error)
        assert (fetched, stored) == (3, 3), (fetched, stored, skipped)
        assert [r[0] for r in rows_in("Pulled")] == ["Upstream 1", "Upstream 2", "Upstream 3"]

        # The seen set makes a second poll a no-op. Without it, leave-on-server
        # would re-post the whole mailbox every interval.
        fetched, stored, skipped, status, error = run("selfpop")
        assert status == "ok", (status, error)
        assert (fetched, stored) == (0, 0), (fetched, stored)
        assert skipped == 3, skipped
        assert len(rows_in("Pulled")) == 3

        # Messages were left where they were, because leave_on_server is the
        # default: pulling from somebody's mailbox must not empty it by accident.
        assert con.execute(
            f"SELECT count(*) FROM citadel_room_msgs WHERE room_num = {mail_room[0]}"
        ).fetchone()[0] == 3

        # ---- IMAP -------------------------------------------------------

        ok, note = con.execute(
            "SELECT ok, note FROM qm_feed_add('selfimap', 'imap', "
            f"'sender:secret@{HOST}:{IMAP_PORT}', 'Newsroom')"
        ).fetchone()
        assert ok, note
        for key, value in (("tls", "none"), ("mailbox", "INBOX")):
            assert con.execute(
                f"SELECT ok FROM qm_feed_set('selfimap', '{key}', '{value}')"
            ).fetchone()[0]

        ok, note = con.execute("SELECT ok, note FROM qm_feed_test('selfimap')").fetchone()
        assert ok, note
        assert "UIDVALIDITY" in note, note

        fetched, stored, skipped, status, error = run("selfimap")
        assert status == "ok", (status, error)
        assert stored == 3, (fetched, stored, skipped, error)
        assert [r[0] for r in rows_in("Newsroom")] == ["Upstream 1", "Upstream 2", "Upstream 3"]

        # The UID watermark holds. "UID n:*" matches the highest UID even when
        # it is below n, so without filtering the newest message would come back
        # on every single poll — this is the assertion that catches that.
        fetched, stored, skipped, status, error = run("selfimap")
        assert status == "ok", (status, error)
        assert stored == 0, (fetched, stored, error)
        assert len(rows_in("Newsroom")) == 3

        # A new upstream message is picked up, and only that one.
        msgnum = con.execute("SELECT nextval('citadel_msg_seq')").fetchone()[0]
        con.execute(
            "INSERT INTO citadel_messages (msgnum, author, msgtime, subject, format_type, "
            f"origin_room, raw) VALUES ({msgnum}, 'someone', 1753660800, 'Upstream 4', 0, "
            "'Mail', 'Body of message 4.')"
        )
        con.execute(f"INSERT INTO citadel_room_msgs VALUES ({mail_room[0]}, {msgnum})")
        con.execute(
            f"UPDATE citadel_rooms SET highest_msg = {msgnum} WHERE room_num = {mail_room[0]}"
        )
        fetched, stored, skipped, status, error = run("selfimap")
        assert status == "ok", (status, error)
        assert stored == 1, (fetched, stored, error)
        assert [r[0] for r in rows_in("Newsroom")][-1] == "Upstream 4"

        # ---- RSS --------------------------------------------------------

        assert con.execute("SELECT ok FROM cit_room_add('Feeds')").fetchone()[0]
        ok, note = con.execute(
            "SELECT ok, note FROM qm_feed_add('news', 'rss', "
            f"'http://{HOST}:{HTTP_PORT}/feed.xml', 'Feeds')"
        ).fetchone()
        assert ok, note

        fetched, stored, skipped, status, error = run("news")
        assert status == "ok", (status, error)
        assert stored == 2, (fetched, stored, error)
        # Oldest first, so message numbers follow publication order in the room.
        assert [r[0] for r in rows_in("Feeds")] == ["Item two", "Item one"]

        # The CDATA-wrapped HTML survived the chunked transfer and became a
        # multipart/alternative.
        raw = con.execute(
            "SELECT CAST(raw AS VARCHAR) FROM citadel_messages WHERE subject = 'Item one'"
        ).fetchone()[0]
        assert "multipart/alternative" in raw, raw[:400]
        assert "<b>first</b>" in raw, raw[:600]
        assert "https://example.invalid/1" in raw

        # A second poll sends the stored ETag and gets a 304, so nothing is
        # parsed and nothing is posted.
        FeedHandler.hits.clear()
        fetched, stored, skipped, status, error = run("news")
        assert status == "unchanged", (status, error)
        assert stored == 0
        assert FeedHandler.hits == ['"v1"'], FeedHandler.hits

        # When the feed does change, only the new item is posted — the two we
        # have already seen are recognised by guid even though they are served
        # again.
        FeedHandler.body = FEED_V2
        FeedHandler.etag = '"v2"'
        fetched, stored, skipped, status, error = run("news")
        assert status == "ok", (status, error)
        assert stored == 1, (fetched, stored, skipped, error)
        assert skipped == 2, skipped
        assert [r[0] for r in rows_in("Feeds")][-1] == "Item three"

        # ---- failures are recorded, not raised --------------------------

        ok, note = con.execute(
            "SELECT ok, note FROM qm_feed_add('dead', 'rss', "
            "'http://127.0.0.1:9/nothing.xml', 'Feeds')"
        ).fetchone()
        assert ok, note
        fetched, stored, skipped, status, error = run("dead")
        assert status == "error", (status, error)
        assert error, "a failure must say what went wrong"
        # And it is on the row, where the admin console shows it.
        assert con.execute(
            "SELECT last_status FROM qm_feeds() WHERE name = 'dead'"
        ).fetchone()[0] == "error"

        # One dead feed does not stop the others: a run over everything still
        # reports a row per feed.
        results = con.execute("SELECT feed, status FROM qm_fetch_run()").fetchall()
        assert len(results) == 4, results
        assert any(s == "error" for _, s in results)
        assert any(s in ("ok", "unchanged") for _, s in results)

        ok, note = con.execute("SELECT ok, note FROM qm_feed_test('dead')").fetchone()
        assert not ok, note

    finally:
        httpd.shutdown()
        con.execute("CALL qm_pop3_stop()").fetchall()
        con.execute("CALL qm_imap_stop()").fetchall()

    print("test_fetch: ok")


if __name__ == "__main__":
    main()
