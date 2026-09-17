#!/usr/bin/env python3
"""End-to-end test for the NNTP front-end (quackmail_nntp).

Exercises the reader half (CAPABILITIES, AUTHINFO, LIST ACTIVE/NEWSGROUPS/
OVERVIEW.FMT with a wildmat pattern, GROUP, LISTGROUP, ARTICLE/HEAD/BODY/STAT,
NEXT/LAST, OVER, DATE) and the posting half, which a real Citadel server does
not implement. Finally it checks the posted article is visible in the Citadel
room store, i.e. that news and the other front-ends share one store.

Requires: pip install duckdb==1.5.4
Run after `make` so the loadable extensions exist under build/release/extension.
"""
import os
import socket
import ssl
import time

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 11190
PORT_TLS = 11563


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class News:
    """Minimal NNTP client: send a command, collect the reply."""

    def __init__(self, port, use_tls=False):
        s = socket.create_connection((HOST, port), timeout=5)
        if use_tls:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            s = ctx.wrap_socket(s)
        s.settimeout(2)
        self.f = s.makefile("rwb")
        self.s = s
        self.greeting = self.f.readline().decode().strip()

    def cmd(self, text, multiline=False):
        self.f.write(text.encode() + b"\r\n")
        self.f.flush()
        status = self.f.readline().decode().strip()
        if not multiline or status[:1] not in "123":
            return status, []
        body = []
        while True:
            line = self.f.readline().decode()
            if line.rstrip("\r\n") == ".":
                break
            if not line:
                break
            body.append(line.rstrip("\r\n"))
        return status, body

    def post(self, article):
        status, _ = self.cmd("POST")
        assert status.startswith("340"), status
        self.f.write(article.replace("\n", "\r\n").encode() + b"\r\n.\r\n")
        self.f.flush()
        return self.f.readline().decode().strip()

    def send_line(self, text):
        """A command with no reply read.

        TAKETHIS answers only after the article, which is the whole point of
        streaming -- so reading a reply here would block forever.
        """
        self.f.write(text.encode() + b"\r\n")
        self.f.flush()

    def send_body(self, article):
        """The article half of IHAVE or TAKETHIS, dot-terminated."""
        self.f.write(article.replace("\n", "\r\n").encode() + b"\r\n.\r\n")
        self.f.flush()
        return self.f.readline().decode().strip()

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext('quackmail')}'")
    con.execute(f"LOAD '{ext('quackmail_citadel')}'")
    con.execute(f"LOAD '{ext('quackmail_nntp')}'")
    con.execute("CALL qm_user_add('newsuser', 'secret')")

    for call in (
        f"SELECT note FROM qm_nntp_start('{HOST}', {PORT}, starttls=>true)",
        f"SELECT note FROM qm_nntps_start('{HOST}', {PORT_TLS}, implicit_tls=>true)",
    ):
        assert con.execute(call).fetchone()[0] == "started"
    time.sleep(0.3)

    try:
        c = News(PORT)
        assert "posting allowed" in c.greeting, c.greeting

        status, caps = c.cmd("CAPABILITIES", multiline=True)
        assert status.startswith("101"), status
        for want in ("VERSION 2", "READER", "MODE-READER", "OVER", "POST", "STARTTLS", "AUTHINFO USER"):
            assert want in caps, f"{want} missing from {caps}"

        # Reader commands need authentication.
        assert c.cmd("LIST")[0].startswith("480")
        assert c.cmd("AUTHINFO USER nobody__")[0].startswith("481")
        assert c.cmd("AUTHINFO USER newsuser")[0].startswith("381")
        assert c.cmd("AUTHINFO PASS wrong")[0].startswith("481")
        assert c.cmd("AUTHINFO USER newsuser")[0].startswith("381")
        assert c.cmd("AUTHINFO PASS secret")[0].startswith("281")

        assert c.cmd("MODE READER")[0].startswith("200")
        assert c.cmd("DATE")[0].startswith("111")

        # LIST ACTIVE: "<group> <high> <low> <post flag>". The seeded public
        # rooms use Citadel's escaping rules.
        status, groups = c.cmd("LIST", multiline=True)
        assert status.startswith("215"), status
        names = [g.split()[0] for g in groups]
        assert "ctdl.lobby" in names, names
        assert "ctdl.trashcan" in names, names
        # Aide and Global Address Book are aide-private, so a regular user does
        # not see them. Personal rooms keep their "<usernum>.<name>" form when it
        # is already a valid newsgroup name, and get escaped when it is not —
        # the same rule the oracle applies.
        assert any(n.endswith(".Mail") for n in names), names
        assert any(n.startswith("ctdl.") and "sent+20items" in n for n in names), names
        lobby_line = next(g for g in groups if g.startswith("ctdl.lobby "))
        assert lobby_line.split()[-1] == "y", lobby_line  # posting allowed

        # LIST NEWSGROUPS maps back to the room name; wildmat filters.
        status, descr = c.cmd("LIST NEWSGROUPS ctdl.lo*", multiline=True)
        assert descr == ["ctdl.lobby Lobby"], descr
        status, fmt = c.cmd("LIST OVERVIEW.FMT", multiline=True)
        assert fmt[0] == "Subject:" and fmt[-1] == "Lines:", fmt
        assert c.cmd("LIST BOGUS")[0].startswith("501")

        # An empty group.
        status, _ = c.cmd("GROUP ctdl.lobby")
        assert status.startswith("211 0 -1 -1 ctdl.lobby"), status
        assert c.cmd("ARTICLE")[0].startswith("420")
        assert c.cmd("GROUP ctdl.nope")[0].startswith("411")

        # --- POST (which the real Citadel NNTP server cannot do) -----------
        c.cmd("GROUP ctdl.lobby")
        article = (
            "Newsgroups: ctdl.lobby\n"
            "Subject: First post\n"
            "From: newsuser@quackcit\n"
            "\n"
            "hello usenet\n"
            "second line\n"
        )
        assert c.post(article).startswith("240"), "POST failed"

        status, _ = c.cmd("GROUP ctdl.lobby")
        assert status.startswith("211 1 1 1 ctdl.lobby"), status

        status, nums = c.cmd("LISTGROUP ctdl.lobby", multiline=True)
        assert nums == ["1"], nums

        status, body = c.cmd("ARTICLE 1", multiline=True)
        assert status.startswith("220 1 <"), status
        assert "Subject: First post" in body, body
        assert "hello usenet" in body, body

        status, head = c.cmd("HEAD 1", multiline=True)
        assert status.startswith("221"), status
        assert any(h.startswith("Subject:") for h in head), head
        assert "hello usenet" not in head, head

        status, only_body = c.cmd("BODY 1", multiline=True)
        assert status.startswith("222"), status
        assert "hello usenet" in only_body and "second line" in only_body, only_body

        status, _ = c.cmd("STAT 1")
        assert status.startswith("223 1 <"), status
        msgid = status.split(" ", 2)[2]

        # Fetch by Message-ID (Citadel answers 500 here; we resolve it).
        status, _ = c.cmd(f"ARTICLE {msgid}", multiline=True)
        assert status.startswith("220"), status

        # OVER: num, subject, from, date, msgid, refs, bytes, lines.
        status, over = c.cmd("OVER 1-", multiline=True)
        assert status.startswith("224"), status
        fields = over[0].split("\t")
        assert fields[0] == "1" and fields[1] == "First post", fields
        assert fields[4] == msgid, fields
        assert int(fields[6]) > 0 and int(fields[7]) > 0, "bytes/lines should be real values"

        # Only one article, so NEXT/LAST have nowhere to go.
        assert c.cmd("NEXT")[0].startswith("421")
        assert c.cmd("LAST")[0].startswith("422")
        assert c.cmd("ARTICLE 99")[0].startswith("423")

        assert c.cmd("HELP", multiline=True)[0].startswith("100")
        assert c.cmd("FROB")[0].startswith("500")
        assert c.cmd("QUIT")[0].startswith("205")
        c.close()

        # --- the peer feed: IHAVE, CHECK, TAKETHIS -------------------------
        # RFC 3977 §6.3.2 and RFC 4644. Worth stating plainly: Citadel's own
        # NNTP implements none of these, so unlike the reader verbs above there
        # is no parity oracle here -- the RFCs are the spec.
        #
        # All three turn on one question, "do I already have this?", which was
        # unanswerable until citadel_msgids existed: the ARTICLE-by-message-id
        # path scans the selected group, which is hopeless for a peer offering
        # thousands.
        c = News(PORT)
        assert c.cmd("AUTHINFO USER newsuser")[0].startswith("381")
        assert c.cmd("AUTHINFO PASS secret")[0].startswith("281")

        status, caps = c.cmd("CAPABILITIES", multiline=True)
        assert "IHAVE" in caps, f"IHAVE is not advertised to a peer: {caps}"
        assert "STREAMING" in caps, f"STREAMING is not advertised: {caps}"
        assert c.cmd("MODE STREAM")[0].startswith("203"), "MODE STREAM refused"

        transit = (
            "From: someone@peer.example\n"
            "Newsgroups: ctdl.lobby\n"
            "Subject: Fed in over IHAVE\n"
            "Message-ID: <feed-1@peer.example>\n"
            "\n"
            "Body from the peer.\n"
        )
        status, _ = c.cmd("IHAVE <feed-1@peer.example>")
        assert status.startswith("335"), f"IHAVE was not wanted: {status}"
        assert c.send_body(transit).startswith("235"), "IHAVE transfer failed"

        # Offering it again must say "already have it" rather than filing a
        # second copy -- the entire point of the index.
        status, _ = c.cmd("IHAVE <feed-1@peer.example>")
        assert status.startswith("435"), f"a duplicate offer was accepted: {status}"
        # 435 and not 437: "I have it" is not "it is bad", and a peer uses the
        # difference to decide whether to keep offering it.
        dupes = con.execute(
            "SELECT count(*) FROM citadel_messages WHERE subject = 'Fed in over IHAVE'"
        ).fetchone()[0]
        assert dupes == 1, f"the duplicate offer stored {dupes} copies"

        # CHECK is the streaming form of that question.
        assert c.cmd("CHECK <feed-1@peer.example>")[0].startswith("438"), "CHECK on a held article"
        status, _ = c.cmd("CHECK <feed-2@peer.example>")
        assert status.startswith("238"), f"CHECK on a new article: {status}"

        # TAKETHIS sends the article with no intervening reply.
        transit2 = transit.replace("feed-1", "feed-2").replace("over IHAVE", "over TAKETHIS")
        # The reply comes after the article, not after the command.
        c.send_line("TAKETHIS <feed-2@peer.example>")
        assert c.send_body(transit2).startswith("239"), "TAKETHIS transfer failed"

        # A TAKETHIS for something already held still has to read the article
        # off the wire before answering, or the connection desynchronises --
        # which the next command proves it did not.
        c.send_line("TAKETHIS <feed-2@peer.example>")
        assert c.send_body(transit2).startswith("439"), "a duplicate TAKETHIS was accepted"
        assert c.cmd("DATE")[0].startswith("111"), "the stream desynchronised after a refusal"

        # An article for a group this server does not carry is refused
        # permanently (437), not deferred: silently creating a room for every
        # group a peer offers would let one peer fill the room list.
        nocarry = transit.replace("feed-1", "feed-3").replace("ctdl.lobby", "ctdl.nosuchgroup")
        status, _ = c.cmd("IHAVE <feed-3@peer.example>")
        assert status.startswith("335"), status
        assert c.send_body(nocarry).startswith("437"), "an uncarried group was accepted"

        c.cmd("QUIT")
        c.close()

        # A peer must authenticate: these sit after the 480 gate deliberately.
        c = News(PORT)
        assert c.cmd("IHAVE <feed-9@peer.example>")[0].startswith("480"), \
            "IHAVE was allowed without authentication"
        assert c.cmd("TAKETHIS <feed-9@peer.example>")[0].startswith("480"), \
            "TAKETHIS was allowed without authentication"
        c.close()

        # --- implicit TLS listener -----------------------------------------
        c = News(PORT_TLS, use_tls=True)
        assert c.cmd("AUTHINFO USER newsuser")[0].startswith("381")
        assert c.cmd("AUTHINFO PASS secret")[0].startswith("281")
        status, groups = c.cmd("LIST ACTIVE ctdl.lobby", multiline=True)
        # "<group> <high> <low> <posting>" (RFC 3977 §7.6.3). The high-water mark
        # is whatever has accumulated by now -- the peer-feed block above adds
        # two articles -- so what is pinned here is the shape and the posting
        # flag, not a count that any earlier test can move.
        assert len(groups) == 1, groups
        parts = groups[0].split()
        assert parts[0] == "ctdl.lobby" and parts[2] == "1" and parts[3] == "y", groups
        assert int(parts[1]) >= 1, groups
        c.cmd("QUIT")
        c.close()
    finally:
        con.execute("CALL qm_nntp_stop()").fetchall()
        con.execute("CALL qm_nntps_stop()").fetchall()

    # The posted article lives in the Lobby like any other Citadel message.
    rows = con.execute(
        "SELECT m.author, m.subject, m.format_type FROM citadel_messages m "
        "JOIN citadel_room_msgs rm USING (msgnum) "
        "JOIN citadel_rooms r USING (room_num) WHERE r.name = 'Lobby'"
    ).fetchall()
    # The locally posted article, plus the two fed in over the peer verbs. A
    # transit article keeps the From: it arrived with rather than the peer's
    # login -- rewriting it would be forging it.
    assert ("newsuser", "First post", 4) in rows, rows
    assert ("someone@peer.example", "Fed in over IHAVE", 4) in rows, rows
    assert ("someone@peer.example", "Fed in over TAKETHIS", 4) in rows, rows
    assert len(rows) == 3, f"the Lobby holds {len(rows)} articles, not 3: {rows}"

    print("PASS: NNTP reader parity (LIST/GROUP/ARTICLE/OVER/NEXT), POST, nntps,")
    print("      and the peer feed (IHAVE, MODE STREAM, CHECK, TAKETHIS)")


if __name__ == "__main__":
    main()
