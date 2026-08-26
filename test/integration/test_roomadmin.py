#!/usr/bin/env python3
"""End-to-end test for per-room management and self-serve rooms.

The thing under test is a *delegation* boundary, so almost every assertion here
is negative: a user who was not granted the RFC 4314 `a` right must not reach a
room's settings, a room administrator must not reach the operator's console, and
an invitation-only room must not be visible to anybody the access list does not
name. The positive cases are cheap by comparison.

The rights derivation itself is asserted offline in test/sql/roomadmin.test —
what needs a live server is the part sqllogictest cannot see: the routes, the
listing, and the flag word surviving a form post.

Requires: pip install duckdb==1.5.4
Run after `make release` so the loadable extensions exist under
build/release/extension.
"""
import http.cookiejar
import imaplib
import os
import re
import time
import urllib.error
import urllib.parse
import urllib.request

import duckdb

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXT_DIR = os.path.join(REPO, "build", "release", "extension")
HOST = "127.0.0.1"
PORT = 18085
PORT_IMAP = 11435
BASE = f"http://{HOST}:{PORT}"


def ext(name):
    return os.path.join(EXT_DIR, name, name + ".duckdb_extension")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """Follow nothing: the status and Location are what is under test."""

    def redirect_request(self, *args, **kwargs):
        return None


def request(op, url, data=None):
    body = urllib.parse.urlencode(data).encode() if data is not None else None
    req = urllib.request.Request(url, data=body)
    req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        r = op.open(req, timeout=10)
        return r.status, dict(r.headers), r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read().decode("utf-8", "replace")


def csrf_of(page):
    marker = 'name="_csrf" value="'
    i = page.index(marker) + len(marker)
    return page[i : page.index('"', i)]


def sign_in(user, password):
    jar = http.cookiejar.CookieJar()
    op = urllib.request.build_opener(NoRedirect(), urllib.request.HTTPCookieProcessor(jar))
    _, _, page = request(op, BASE + "/login")
    status, _, _ = request(
        op,
        BASE + "/login",
        {"_csrf": csrf_of(page), "username": user, "password": password, "next": "/bbs/"},
    )
    assert status == 303, f"login for {user} returned {status}"
    return op


def post(op, path, fields, csrf_from=None):
    """POST `fields` to `path`, taking a fresh CSRF token from a GET first."""
    _, _, page = request(op, BASE + (csrf_from or path))
    body = dict(fields)
    body["_csrf"] = csrf_of(page)
    return request(op, BASE + path, body)


def rights_fields(rights_str):
    """The settings/acl form now posts one checkbox per RFC 4314 letter
    (right_l, right_r, ...) instead of a free-typed "rights" string."""
    return {f"right_{c}": "1" for c in rights_str}


def rights(con, room, user):
    row = con.execute(
        "SELECT rights, can_post, can_administer FROM cit_room_rights(?, ?)", [room, user]
    ).fetchall()
    return row[0] if row else None


def flags_of(con, room_num):
    return con.execute(
        "SELECT qr_flags FROM citadel_rooms WHERE room_num = ?", [room_num]
    ).fetchone()[0]


def main():
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    # IMAP is here for one section: the whole design rests on `a` being one
    # right that every front-end reads the same way, and nothing else in the
    # suite crosses protocols to prove it.
    for name in ("quackmail", "quackmail_citadel", "quackmail_http", "quackmail_imap"):
        con.execute(f"LOAD '{ext(name)}'")

    for user, ax in (("owner", 4), ("member", 4), ("stranger", 4), ("boss", 6)):
        con.execute(f"CALL qm_user_add('{user}', 'secret')")
        con.execute(
            "INSERT INTO citadel_users (username, usernum, axlevel) "
            f"VALUES ('{user}', nextval('citadel_user_seq'), {ax})"
        )
    con.execute("CALL qm_config_set('qm_web_force_https', '0')")
    con.execute("CALL qm_config_set('c_fqdn', 'quackmail.test')")

    note = con.execute(f"SELECT note FROM qm_http_start('{HOST}', {PORT})").fetchone()[0]
    assert note == "started", f"http did not start: {note}"
    note = con.execute(f"SELECT note FROM qm_imap_start('{HOST}', {PORT_IMAP})").fetchone()[0]
    assert note == "started", f"imap did not start: {note}"
    time.sleep(0.5)

    try:
        owner = sign_in("owner", "secret")
        member = sign_in("member", "secret")
        stranger = sign_in("stranger", "secret")
        boss = sign_in("boss", "secret")

        # ---- the creation gate ------------------------------------------
        #
        # Default is the aide level, so an existing server gains nothing until
        # an operator lowers it. That default is the whole reason this feature
        # can ship switched on.
        status, _, _ = request(owner, BASE + "/bbs/new")
        assert status == 403, f"/bbs/new open to a plain user by default: {status}"
        status, _, page = request(boss, BASE + "/bbs/new")
        assert status == 200, f"aide refused /bbs/new: {status}"

        _, _, sidebar = request(owner, BASE + "/bbs/")
        assert "/bbs/new" not in sidebar, "sidebar offers a page the user may not reach"

        con.execute("CALL qm_config_set('qm_room_create_axlevel', '4')")
        status, _, page = request(owner, BASE + "/bbs/new")
        assert status == 200, f"/bbs/new still refused after lowering the level: {status}"
        _, _, sidebar = request(owner, BASE + "/bbs/")
        assert "/bbs/new" in sidebar, "sidebar does not offer room creation once it is allowed"

        # A level that is not a number must not be read as 0 — a typo in a
        # setting that gates a capability has to fail closed.
        con.execute("CALL qm_config_set('qm_room_create_axlevel', 'yes')")
        status, _, _ = request(owner, BASE + "/bbs/new")
        assert status == 403, "a malformed access level opened the gate"
        con.execute("CALL qm_config_set('qm_room_create_axlevel', '4')")

        # ---- creating a room --------------------------------------------
        status, headers, _ = post(
            owner,
            "/bbs/new",
            {"display_name": "Project X", "floor": "0", "view": "0", "private": "1",
             "info": "A private room."},
        )
        assert status == 303, f"room creation returned {status}"
        location = headers["Location"]
        assert location.endswith("?ok=room_created"), location
        room_num = int(re.search(r"/bbs/room/(\d+)/settings", location).group(1))

        # The creator holds every right, which is what makes them the
        # administrator — there is no separate owner column to consult.
        r = rights(con, "Project X", "owner")
        assert r == ("lrswipkxtea", True, True), f"creator rights: {r}"
        assert rights(con, "Project X", "stranger") == ("", False, False), "private room leaks rights"

        # ---- who can see an invitation-only room -------------------------
        _, _, page = request(owner, BASE + "/bbs/")
        assert "Project X" in page, "creator cannot see their own private room"
        _, _, page = request(stranger, BASE + "/bbs/")
        assert "Project X" not in page, "private room listed to somebody with no grant"

        status, _, _ = request(stranger, BASE + f"/bbs/room/{room_num}")
        assert status == 404, f"private room readable by a stranger: {status}"
        status, _, _ = request(stranger, BASE + f"/bbs/room/{room_num}/settings")
        assert status == 404, f"private room's settings reachable by a stranger: {status}"

        status, _, _ = request(owner, BASE + f"/bbs/room/{room_num}/settings")
        assert status == 200, "creator cannot reach their own room's settings"
        # An aide reaches every room without any grant at all.
        status, _, _ = request(boss, BASE + f"/bbs/room/{room_num}/settings")
        assert status == 200, "aide refused a room's settings"

        # ---- names that would collide ------------------------------------
        status, _, body = post(
            owner, "/bbs/new", {"display_name": "0000000012.Mail", "floor": "0", "view": "0"}
        )
        assert status == 400 and "reserved" in body, f"reserved name accepted: {status}"
        status, _, _ = post(
            owner, "/bbs/new", {"display_name": "Project X", "floor": "0", "view": "0"}
        )
        assert status == 400, "duplicate room name accepted"

        # ---- delegating, one right at a time -----------------------------
        settings = f"/bbs/room/{room_num}/settings"
        status, _, _ = post(owner, settings + "/acl", {"identifier": "member", **rights_fields("lrswi")},
                            csrf_from=settings)
        assert status == 303, f"granting access returned {status}"
        assert rights(con, "Project X", "member") == ("lrswi", True, False)

        status, _, _ = request(member, BASE + f"/bbs/room/{room_num}")
        assert status == 200, "an invited member cannot read the room"
        _, _, page = request(member, BASE + "/bbs/")
        assert "Project X" in page, "an invited member does not see the room listed"
        # Reading it is not administering it.
        status, _, _ = request(member, BASE + settings)
        assert status == 403, f"a member without `a` reached the settings: {status}"
        status, _, _ = post(member, settings + "/save",
                            {"display_name": "Hijacked", "floor": "0", "view": "0"},
                            csrf_from=f"/bbs/room/{room_num}")
        assert status == 403, "a member without `a` could save room settings"

        status, _, _ = post(owner, settings + "/acl", {"identifier": "member", **rights_fields("lrswia")},
                            csrf_from=settings)
        assert status == 303
        status, _, _ = request(member, BASE + settings)
        assert status == 200, "a member granted `a` still cannot administer the room"

        # A delegate cannot drop their own administration: only an aide could
        # give it back, and that is a support request rather than a choice.
        status, _, _ = post(member, settings + "/acl", {"identifier": "member", **rights_fields("lrswi")},
                            csrf_from=settings)
        assert status == 403, "a delegate locked themselves out"
        assert rights(con, "Project X", "member")[2] is True

        # "anyone" covers callers who are not signed in, so it can never hold
        # `a`. This is the entry somebody types by accident.
        status, _, body = post(owner, settings + "/acl", {"identifier": "anyone", **rights_fields("lrsa")},
                               csrf_from=settings)
        assert status == 400 and "anyone" in body, f"anyone was granted `a`: {status}"

        # An aide, though, may take a delegate's rights away — no right_X
        # fields at all, same as unchecking every box.
        status, _, _ = post(boss, settings + "/acl", {"identifier": "member"},
                            csrf_from=settings)
        assert status == 303
        assert rights(con, "Project X", "member") == ("", False, False)

        # ---- delegating room *creation* -----------------------------------
        #
        # `k` lets a room administrator hand a member the ability to create
        # rooms on that room's floor, without touching the site-wide
        # qm_room_create_axlevel gate or the delegate's own access level.
        # Raise the gate to aide-only first so the axlevel path cannot be what
        # is actually passing this.
        con.execute("CALL qm_config_set('qm_room_create_axlevel', '6')")
        con.execute("CALL cit_floor_add('Annex')")
        annex = con.execute(
            "SELECT floor_num FROM citadel_floors WHERE name = 'Annex'"
        ).fetchone()[0]

        status, _, _ = request(stranger, BASE + "/bbs/new")
        assert status == 403, f"a plain user reached /bbs/new with the gate raised: {status}"

        status, _, _ = post(owner, settings + "/acl", {"identifier": "stranger", **rights_fields("lk")},
                            csrf_from=settings)
        assert status == 303, f"granting k returned {status}"

        status, _, page = request(stranger, BASE + "/bbs/new")
        assert status == 200, f"a `k` grant on a room's floor did not open /bbs/new: {status}"
        assert "Annex" not in page, "the floor picker offered a floor `k` was never granted on"

        status, _, _ = post(
            stranger, "/bbs/new", {"display_name": "Annex Room", "floor": str(annex), "view": "0"}
        )
        assert status == 403, f"`k` on floor 0 created a room on a different floor: {status}"

        status, headers, _ = post(
            stranger, "/bbs/new", {"display_name": "Delegated Room", "floor": "0", "view": "0"}
        )
        assert status == 303, f"a `k` grant did not allow creating on that room's floor: {status}"
        # The creator still becomes the new room's own administrator, same as
        # any other creation path.
        r = rights(con, "Delegated Room", "stranger")
        assert r[2] is True, f"k-delegated creator did not become the room's administrator: {r}"
        con.execute("CALL cit_room_kill('Delegated Room')")

        # Taking `k` away closes the door again.
        status, _, _ = post(owner, settings + "/acl", {"identifier": "stranger", **rights_fields("l")},
                            csrf_from=settings)
        assert status == 303
        status, _, _ = request(stranger, BASE + "/bbs/new")
        assert status == 403, "revoking k left /bbs/new open"
        assert rights(con, "Project X", "stranger") == ("l", False, False)

        con.execute("CALL qm_config_set('qm_room_create_axlevel', '4')")

        # ---- saving preferences ------------------------------------------
        #
        # QR_UPLOAD is not one of this form's checkboxes. A checkbox set that is
        # not exhaustive silently clears whatever it left out, so the bits the
        # form does not offer have to be carried over rather than rebuilt.
        con.execute("UPDATE citadel_rooms SET qr_flags = qr_flags | 64 WHERE room_num = ?", [room_num])
        before = flags_of(con, room_num)
        status, _, _ = post(
            owner, settings + "/save",
            {"display_name": "Project X", "floor": "0", "view": "3", "info": "Now a calendar.",
             "listorder": "0", "password": "", "private": "1"},
            csrf_from=settings,
        )
        assert status == 303, f"saving room settings returned {status}"
        after = flags_of(con, room_num)
        assert after & 64, f"a flag the form does not offer was cleared: {before} -> {after}"
        assert after & 4, "the private flag was lost on save"
        view = con.execute(
            "SELECT default_view FROM citadel_rooms WHERE room_num = ?", [room_num]
        ).fetchone()[0]
        assert view == 3, f"default_view not saved: {view}"

        # The reserved shape is refused on a rename as well as on creation —
        # UpdateRoom is a second way to reach the same keyspace.
        status, _, body = post(
            owner, settings + "/save",
            {"display_name": "0000000012.Mail", "floor": "0", "view": "0", "listorder": "0",
             "password": ""},
            csrf_from=settings,
        )
        assert status == 400 and "reserved" in body, f"reserved name accepted on rename: {status}"

        # ---- opening a room to e-mail ------------------------------------
        #
        # Only meaningful for a public room, so drop the private flag first.
        status, _, _ = post(
            owner, settings + "/save",
            {"display_name": "Project X", "floor": "0", "view": "0", "listorder": "0",
             "password": ""},
            csrf_from=settings,
        )
        assert status == 303
        status, _, _ = post(owner, settings + "/mail", {"open": "1"}, csrf_from=settings)
        assert status == 303, f"opening the room to mail returned {status}"
        r = rights(con, "Project X", "")
        assert r[1] is True, f"anonymous senders still refused after opening: {r}"
        status, _, _ = post(owner, settings + "/mail", {"open": "0"}, csrf_from=settings)
        assert status == 303
        assert rights(con, "Project X", "")[1] is False, "closing the room to mail did nothing"

        # ---- feeds are read-only here ------------------------------------
        #
        # A feed stores a password and dials whatever host it names, so a room
        # administrator may only run one an aide already pointed at their room.
        con.execute("CALL cit_room_add('Elsewhere')")
        con.execute(
            "CALL qm_feed_add('somefeed', 'rss', 'https://127.0.0.1:9/none.xml', 'Elsewhere')"
        )
        status, _, _ = post(owner, settings + "/feedrun", {"name": "somefeed"}, csrf_from=settings)
        assert status == 404, f"a feed for another room was runnable from here: {status}"

        _, _, page = request(owner, BASE + settings)
        assert "/admin/feeds" in page, "the settings page does not point at the operator's feed console"

        # ---- deleting a room ---------------------------------------------
        status, _, _ = post(owner, settings + "/kill", {"confirm": "wrong"}, csrf_from=settings)
        assert status == 400, "a room was deleted without its name being typed"
        assert flags_of(con, room_num) is not None

        # KillRoom does take the list configuration with it now, so this refusal
        # is about authority rather than about leaving half a list behind: the
        # address a list stops accepting mail for is the site's, so unmaking one
        # is an aide's call and not a room administrator's.
        con.execute("CALL qm_list_create('Project X', 'projectx')")
        status, _, body = post(owner, settings + "/kill", {"confirm": "Project X"},
                               csrf_from=settings)
        assert status == 400 and "projectx@" in body, f"a list's room was deletable: {status}"
        assert con.execute(
            "SELECT count(*) FROM citadel_lists WHERE room_num = ?", [room_num]
        ).fetchone()[0] == 1, "the refused delete removed the list anyway"
        con.execute("CALL qm_list_remove('Project X')")

        # What an aide gets instead: room and list go together, so no address is
        # left accepting mail for a room that is not there.
        con.execute("CALL cit_room_add('Doomed')")
        con.execute("CALL qm_list_create('Doomed', 'doomed')")
        con.execute("CALL qm_list_sub_add('Doomed', 'erin@example.com', 'post')")
        con.execute("CALL cit_room_kill('Doomed')")
        assert con.execute(
            "SELECT count(*) FROM citadel_lists WHERE address = 'doomed'"
        ).fetchone()[0] == 0, "the list survived the room it was configured against"
        assert con.execute(
            "SELECT count(*) FROM citadel_list_subs s "
            "WHERE NOT EXISTS (SELECT 1 FROM citadel_rooms r WHERE r.room_num = s.room_num)"
        ).fetchone()[0] == 0, "subscribers were left pointing at a room that is gone"

        # The cleanup has to happen on the *web* route too, which is the case
        # where hook installation is least obvious: a browser session opens its
        # own Connection inside the http extension, and if store::EnsureSchema
        # has not run there KillRoom finds an empty hook registry and drops the
        # room while these rows survive — silently, because nothing errors.
        #
        # A feed rather than a list, because the route above refuses to delete a
        # list's room and so can never reach the hook. And note what this file
        # does *not* load: quackmail_spool owns the feed worker, but the http
        # extension has to install the hook by itself or this is broken in
        # exactly the deployment a room is most likely to be deleted from.
        con.execute(
            "CALL qm_feed_add('roomfeed', 'rss', 'https://127.0.0.1:9/f.xml', 'Project X')"
        )
        con.execute(
            "INSERT INTO quackmail_feed_seen (feed_id, uid) "
            "SELECT id, 'guid-1' FROM quackmail_feeds WHERE name = 'roomfeed'"
        )
        assert con.execute(
            "SELECT count(*) FROM quackmail_feeds WHERE name = 'roomfeed'"
        ).fetchone()[0] == 1

        status, headers, _ = post(owner, settings + "/kill", {"confirm": "Project X"},
                                  csrf_from=settings)
        assert status == 303, f"deleting the room returned {status}"
        gone = con.execute(
            "SELECT count(*) FROM citadel_rooms WHERE room_num = ?", [room_num]
        ).fetchone()[0]
        assert gone == 0, "the room survived its own deletion"
        assert con.execute(
            "SELECT count(*) FROM quackmail_feeds WHERE name = 'roomfeed'"
        ).fetchone()[0] == 0, "the feed outlived the room it pulls into (hook did not fire on the web route)"
        # RemoveFeed cascades to the seen-uids, which is why the hook goes
        # through it rather than deleting the feed row directly.
        assert con.execute(
            "SELECT count(*) FROM quackmail_feed_seen s "
            "WHERE NOT EXISTS (SELECT 1 FROM quackmail_feeds f WHERE f.id = s.feed_id)"
        ).fetchone()[0] == 0, "feed seen-uids were orphaned"

        # ---- the rooms that are not anybody's to delete -------------------
        #
        # The Lobby has always been refused; the Aide room is where
        # PostAideMessage writes, so losing it would break the server's own log
        # channel rather than just removing a room.
        for reserved in (0, 1):
            status, _, _ = post(
                boss, f"/bbs/room/{reserved}/settings/kill",
                {"confirm": con.execute(
                    "SELECT display_name FROM citadel_rooms WHERE room_num = ?", [reserved]
                ).fetchone()[0]},
                csrf_from=f"/bbs/room/{reserved}/settings",
            )
            assert status == 400, f"room {reserved} was deletable: {status}"

        # ---- delegation crosses protocols ---------------------------------
        #
        # The claim the whole design rests on: `a` is one right, stored once, and
        # an aide delegates a room from whatever client is already open. Nothing
        # else in the suite exercises the crossing.
        con.execute("CALL cit_room_add('Steering')")
        steering = con.execute(
            "SELECT room_num FROM citadel_rooms WHERE display_name = 'Steering'"
        ).fetchone()[0]
        status, _, _ = request(member, BASE + f"/bbs/room/{steering}/settings")
        assert status == 403, "a room was administrable before anyone granted it"

        imap = imaplib.IMAP4(HOST, PORT_IMAP)
        assert "ACL" in imap.capabilities, "the IMAP listener does not advertise ACL"
        imap.login("boss", "secret")
        box = next(
            b.decode().split(' "/" ')[-1].strip('"')
            for b in imap.list()[1]
            if "Steering" in b.decode()
        )
        typ, _ = imap._simple_command("SETACL", f'"{box}"', "member", "lrswia")
        assert typ == "OK", f"SETACL failed: {typ}"
        imap.logout()

        status, _, page = request(member, BASE + f"/bbs/room/{steering}/settings")
        assert status == 200, f"a SETACL grant did not reach the web: {status}"
        # And it is a working grant, not just a visible page.
        status, _, _ = post(
            member, f"/bbs/room/{steering}/settings/save",
            {"display_name": "Steering Group", "floor": "0", "view": "0", "listorder": "0",
             "password": ""},
            csrf_from=f"/bbs/room/{steering}/settings",
        )
        assert status == 303
        assert con.execute(
            "SELECT display_name FROM citadel_rooms WHERE room_num = ?", [steering]
        ).fetchone()[0] == "Steering Group"

        # Revocation crosses back. The mailbox path follows the room's new name,
        # which is itself a check that the rename really moved it.
        imap = imaplib.IMAP4(HOST, PORT_IMAP)
        imap.login("boss", "secret")
        box = next(
            b.decode().split(' "/" ')[-1].strip('"')
            for b in imap.list()[1]
            if "Steering Group" in b.decode()
        )
        typ, _ = imap._simple_command("DELETEACL", f'"{box}"', "member")
        assert typ == "OK", f"DELETEACL failed: {typ}"
        imap.logout()
        status, _, _ = request(member, BASE + f"/bbs/room/{steering}/settings")
        assert status == 403, f"a DELETEACL revocation did not reach the web: {status}"

        # ---- personal folders are not rooms to administer -----------------
        mail_num = con.execute(
            "SELECT r.room_num FROM citadel_rooms r JOIN citadel_users u "
            "ON u.usernum = r.mailbox_owner WHERE u.username = 'owner' AND r.display_name = 'Mail'"
        ).fetchone()
        if mail_num is not None:
            status, _, _ = request(owner, BASE + f"/bbs/room/{mail_num[0]}/settings")
            assert status == 403, f"a personal folder was administrable: {status}"

        print("test_roomadmin: OK")
    finally:
        con.execute("CALL qm_http_stop()")
        con.execute("CALL qm_imap_stop()")
        con.close()


if __name__ == "__main__":
    main()
