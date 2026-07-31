# TODO.md

Live task list. Context in [MEMORY.md](MEMORY.md), working instructions in
[CLAUDE.md](CLAUDE.md).

## In flight

- [ ] **JMAP Core + Mail** (RFC 8620 + RFC 8621), the second half of the
      groupware-clients work CalDAV/CardDAV started. Blocked on nothing; the
      seams it needs are already in place.
  - [ ] `core/json.{hpp,cpp}` — there is no JSON anywhere in `core/` today. A
        value variant with `Parse`/`Serialize`, strict about UTF-8 and about
        depth and size, because the input is attacker-reachable. Register
        `qm_json_*` scalars so `test/sql/` can assert the codec with no socket,
        the way `qm_dav_name` is asserted now.
  - [ ] `/.well-known/jmap` → the Session resource; `/jmap/api` for the method
        array with back-references; `/jmap/download/...` for blobs, over the
        attachment path `web_mail.cpp` already has.
  - [ ] `Mailbox`, `Email`, `Thread`, `Identity`, `EmailSubmission`, mapped the
        way IMAP already maps them (mailbox = room, id = msgnum, keywords =
        `citadel_msg_flags`). `Email/changes` reuses `RoomChangeToken` and
        `RoomChangesSince` from the DAV work — that is what makes it
        implementable at all.
  - [ ] Unlike IMAP's `APPEND`, ask `citadel::CanPost`. IMAP not doing so is a
        gap worth not copying.
  - The JMAP Calendars and Contacts bindings are deliberately out of scope:
    they are drafts, and no shipping client speaks them. CalDAV/CardDAV is what
    a phone actually connects with.

## Shipped

- [x] **CalDAV and CardDAV** over the groupware rooms, on the existing HTTP and
      HTTPS listeners — not an extension of its own, because it is a second
      projection of the rooms the web UI already renders rather than a new
      service. `/dav/`, with `.well-known` discovery, principal and home sets,
      `PROPFIND`, `PROPPATCH`, `GET`/`PUT`/`DELETE`, `calendar-query`,
      `calendar-multiget`, `addressbook-query`, `addressbook-multiget` and
      `sync-collection`.
  - [x] `core/davxml.{hpp,cpp}` — an XML document reader over the existing
        `xmlstream` tokenizer (resolving prefixes to namespace URIs, so matching
        never depends on a prefix a client chose) and an element writer. The
        first XML *writer* in the tree; XMPP concatenates strings.
  - [x] The resource-name encoding, which is the one real subtlety.
        `http::NormalizePath` percent-decodes *before* it splits a path into
        segments, so percent-encoding cannot help: `%2F` decodes back to a `/`
        and turns one segment into two. Names are encoded to unreserved bytes
        only, with `~HH` for the rest and a leading `.` always escaped so no
        name can be `.` or `..`.
  - [x] `citadel_room_tombstones` plus `RoomChangeToken`/`RoomChangesSince`.
        Additions were already discoverable by msgnum, so the insert path every
        inbound message takes is untouched; removals left nothing behind at all,
        which is why a deletion was invisible to any token derived from the
        store.
  - [x] `Role::Dav`: Basic against the same credential IMAP takes, through the
        same `LoginAllowed` throttle the login form uses; 401 with a challenge
        rather than a redirect to `/login`; no CSRF, which a client that has
        never seen an HTML form cannot satisfy and does not need to.
  - [x] Deliberately absent: `LOCK`/`UNLOCK` (we advertise `DAV: 1` only —
        CalDAV's consistency story is ETags and `If-Match`, which is
        implemented), `MKCALENDAR`/`MKCOL` (creating a calendar means creating a
        room, which has a floor and an access level to decide), and the Notes
        rooms (vNote is not a DAV resource type, and rewriting them as
        `VJOURNAL` would lose the WebCit parity that is the reason they are
        vNote).

- [x] **Per-room management and self-serve rooms** (phase 4 of the web overhaul)
  - [x] `citadel::CanAdminister` on the RFC 4314 `a` right, which
        `EffectiveRights` already derives for an aide and for the owner of a
        personal room. An aide delegates a room by granting it — from the web,
        `quackcitadm.sh room acl`, or any IMAP client's `SETACL`. No new column,
        no QuackCit-only bit, nothing only one protocol can see.
  - [x] `http/src/web_rooms.cpp`: `/bbs/room/:n/settings` as a **`Role::User`**
        route — preferences, an ACL editor with the rights legend, the room's
        `room_<name>@` address, mailing-list presentation and membership (each
        field through `listserv::SetField`, so a partial form cannot reset what
        it does not show), the feeds pointed at the room, and deletion behind
        typing the room's name.
  - [x] two things stay aide-only, and the page says why: creating a **feed**
        (`fetch::Feed` stores a plaintext password and `RunFeed` dials an
        arbitrary host — an SSRF primitive with credential storage attached) and
        turning a room into a **list** at all (that mints an inbound address and
        a fan-out engine). A room administrator may poll a feed an aide already
        aimed at their room, and *invite* a subscriber — by confirmation token,
        never outright, so nobody signs up an address they do not read.
  - [x] `/bbs/new` gated on `qm_room_create_axlevel`, default 6 so nothing
        changes until an operator lowers it; an unparseable value reads as the
        default rather than as 0. A restricted flag mask (no `QR_NETWORK`, no
        file-area bits). The creator gets a full stored grant, which is what
        makes them the administrator.
  - [x] **private rooms became joinable.** `ListRooms` and `ResolveRoomNumFor`
        hid every `QR_PRIVATE` room from every non-aide, consulting no ACL — so
        a self-serve private room would have been invisible to the person who
        had just created it. Both now honour a stored `l` grant, which is
        exactly RFC 4314's lookup right.
  - [x] **the mailbox keyspace is defended.** A public room's internal key *is*
        its display name; a personal room's is `<usernum padded to 10>.<room>`.
        A public room called `0000000002.Mail` squats the key user 2's Mail room
        needs, and the loser fails on a unique constraint rather than on
        anything a user could read. `IsReservedRoomName` refuses the shape in
        `CreateRoom` **and** `UpdateRoom`, so a rename cannot reach it either.
  - [x] the room-edit forms carry over flag bits they have no checkbox for. The
        admin console's list omits `QR_UPLOAD`/`QR_DOWNLOAD`/`QR_VISDIR`, so
        saving that page silently cleared them; a checkbox set that is not
        exhaustive always does.
  - [x] `KillRoom` refuses the Aide room, which `PostAideMessage` writes to
        unconditionally — losing it breaks the server's own log channel rather
        than merely removing a room. Personal folders are refused by the
        settings page outright: renaming "Mail" would leave IMAP's INBOX and
        every Sieve `fileinto` pointing at a room that no longer exists.
  - [x] **`KillRoom` no longer leaves rows keyed to a room that is gone.** A
        `citadel_lists` row outliving its room left `ResolveAddress` pointing
        the list's inbound address at nothing; a `quackmail_feeds` row left
        `RunDue` polling a feed with nowhere to put the result. `listserv` and
        `fetch` both depend on `citadel_store`, so it cannot call into them and
        should not learn their table names either — each registers a cleanup
        with `RegisterRoomDeletedHook` from its own `EnsureSchema` and
        `KillRoom` runs the set. A new table keyed by `room_num` means a hook
        beside it, not an edit to `citadel_store.cpp`. `cit_room_kill` exposes
        the whole thing to SQL (and to `quackcitadm.sh room kill`), which is
        what makes it assertable in `test/sql/`.
  - [x] `cit_room_rights(room, user)` — the *derived* view, which
        `cit_room_acl` cannot show, so the predicate the front-ends ask is
        assertable in SQL. `quackcitadm.sh room rights` beside it.
  - [x] `test/sql/roomadmin.test` and `test/integration/test_roomadmin.py`,
        which is mostly negative: a member without `a` cannot save settings, a
        delegate cannot drop their own `a`, `anyone` cannot hold `a`, a stranger
        404s on a private room and its settings alike, a feed for another room
        is not runnable, and a flag the form does not offer survives a save.

- [x] **Rich mail and a Sieve rule builder** (phase 3 of the web overhaul)
  - [x] `core/src/mime_build.cpp` — one builder choosing the nesting
        (plain / alternative / related / mixed), replacing the third hand-rolled
        copy in the tree. Boundaries verified absent from every part's bytes;
        transfer encoding chosen rather than fixed.
  - [x] `core/src/html_sanitize.cpp` with two profiles: the existing deny-list
        for *display* (defensible only behind the sandboxed frame) and a true
        **allow-list** for *compose*, applied before the message is built,
        because composed HTML is stored and re-served to other people.
        28 evasions and 14 must-survive constructs asserted.
  - [x] `cid:` inline images end to end: data: URIs from the editor become real
        parts, and the serving route forces the type from a list of four —
        `image/svg+xml` downloads rather than rendering.
  - [x] `sieve::Decompose`/`Compose` over the AST that was already in
        `core/src/sieve.cpp`. The script text stays the single source of truth;
        anything the rule view cannot express is reported and left alone.
  - [x] `http/assets/qc-compose.js` — hand-rolled contenteditable editor, no
        dependency, degrading to the plain textarea when JS is off.
  - [x] `/prefs/sieve` rule cards with add, delete and reorder, all plain form
        posts and no JavaScript.
  - [x] `test/sql/{mime,html_sanitize,sieve}.test` plus
        `test/integration/test_richmail.py` and `test_sieve_rules.py`.

- [x] **The remaining room views** (phase 2b of the web overhaul)
  - [x] `core/contentline.{hpp,cpp}` — the line grammar vCard, iCalendar and
        vNote share, extracted rather than copied a third time.
  - [x] `core/vnote.{hpp,cpp}` — what real Citadel stores a sticky note in
        (`text/vnote`), so WebCit and the Citadel clients can read ours. VJOURNAL
        would have needed no new code but would not have been readable by them.
  - [x] a viewer time zone: `web_tz` per user, `qm_default_tz` for the site, with
        a picker on `/prefs`. `FormatTime` used `localtime_r`, so every timestamp
        rendered in the *server's* zone.
  - [x] `web_calendar.cpp` — month grid and agenda over `ical::Expand`, events
        written with a TZID so a recurrence keeps its local hour across a DST
        change. `VIEW_CALBRIEF` shares the handler.
  - [x] `web_tasks.cpp` — VTODO with due/priority/progress, sorted as a work
        queue, complete toggled by POST.
  - [x] `web_notes.cpp` — a vNote card grid, colours validated before they reach
        a style attribute.
  - [x] `web_blog.cpp` — blog and journal, which hold *ordinary messages*: whole
        entries newest-first, reusing `RenderMessage` and the existing compose
        and read routes rather than growing a second edit path.
  - [x] `test/sql/vnote.test` and `test/integration/test_groupware.py`.
  - [x] deliberately **not** wiki (`VIEW_WIKI`/`WIKIMD` need versioning, a
        name→euid resolver and a markdown renderer — its own PR) or
        `VIEW_QUEUE` (Citadel's internal spool view, not a user view). Drafts
        stay on the mailbox path under `/mail/`.

- [x] **Groupware core and the contacts view** (phase 2a of the web overhaul)
  - [x] a bundled IANA time zone database: `core/tz.{hpp,cpp}` +
        `tools/gen_tzdata.py` → committed `core/src/tzdata.cpp` (598 zones, 341
        distinct after dedup, 252 links, IANA 2026c), on the same terms as the
        PSL. Verified against Python's `zoneinfo` over the same release: 318,136
        offset/DST/abbreviation checks across every zone and 1971–2055, plus
        71,760 wall-clock round trips, zero mismatches.
  - [x] `core/vcard.{hpp,cpp}` — vCard 3.0/4.0, preserving unknown properties,
        folding at 75 octets on a UTF-8 boundary.
  - [x] `core/ical.{hpp,cpp}` — VEVENT/VTODO/VJOURNAL, a Component tree beside
        a flat Item so an edit cannot drop an alarm, `EmitVtimezone` from the
        bundled database, and recurrence that steps in **wall-clock** terms so a
        weekly 09:00 meeting keeps its local hour across a DST change. Capped at
        750 occurrences.
  - [x] `citadel::UpsertByEuid`/`FindByEuid` and an index on
        `citadel_messages.euid` (there were no indexes on that table at all).
  - [x] `enum RoomView` extended with the numbering transcribed from
        `libcitadel.h` on the oracle — 6 WIKI, 7 CALBRIEF, 8 JOURNAL, 9 DRAFTS,
        10 BLOG, 11 QUEUE. A different, plausible-looking ordering was in
        circulation and is wrong.
  - [x] `web_views.cpp` dispatch on `default_view` with `?view=raw` as the
        escape hatch, and `web_contacts.cpp`: browse, view, create, edit in
        place, delete.
  - [x] `test/sql/{tz,vcard,ical}.test` and `test/integration/test_contacts.py`,
        which reads objects created in the browser back over IMAP.

- [x] **Web: persistent connections, `/static` assets, a sidebar** (phase 1 of
      the web overhaul)
  - [x] keep-alive in `core/src/http.cpp`, bounded by 100 requests per
        connection, a 5 s idle deadline and a 60 s connection ceiling. The first
        request keeps the 15 s header budget the slow-loris defence relies on.
  - [x] the invariant that makes it safe: any non-`Ok` read closes the
        connection, because 413 and 411 answer without consuming the announced
        body and those bytes would otherwise be read as the next request.
        Regression test pipelines a request behind an oversized `Content-Length`.
  - [x] `ServerController::SetMaxConnections` (`qm_http_max_connections`,
        default 256; 0 = unlimited, so no other protocol changes). MEMORY.md
        named the absence of a cap as the reason connections were closed, so it
        had to land in the same commit.
  - [x] `/static/*` with content-hashed immutable URLs, ETag/304, and bytes
        compiled in from `tools/gen_assets.py` → committed `web_assets.cpp`.
        CI runs `--check` so a stale generated file fails the build.
  - [x] CSP gains `'self'` for script and style; the message-body frame
        deliberately does not.
  - [x] the stylesheet splits into inlined critical CSS + `/static/qc.css`;
        `NavFor` becomes a grouped sidebar; `AdminNav`'s nineteen buttons become
        six labelled groups; `Render` gains a `PageOpts` overload so no existing
        call site moved.

- [x] **Pull messages in from POP3, IMAP and RSS** (`qm_fetch` in
      `quackmail_spool`, `core/src/fetch.cpp`)
  - [x] the tree spoke all three protocols only as a server, so the clients are
        new: `mail_client.cpp` (POP3 `UIDL`/`RETR`/`DELE`; IMAP `UID SEARCH` /
        `UID FETCH BODY.PEEK[]` with literal handling and `UIDVALIDITY`),
        `http_client.cpp`, `feed.cpp`
  - [x] the HTTP client is separate from `core/http.cpp` on purpose: that one
        refuses chunked transfer encoding, which is right for a server and fatal
        for a client, because feed servers chunk
  - [x] RSS 2.0 / RDF / Atom over the `xmlstream` tokenizer already in core for
        XMPP — which gained CDATA support, without which the tag scanner stops
        at the first `>` inside a feed's HTML payload
  - [x] `quackmail_feed_seen` (POP3 UIDLs, IMAP `<uidvalidity>.<uid>`, RSS
        guids) makes a poll idempotent; ETag/If-Modified-Since turn an unchanged
        feed into a 304 with no body
  - [x] messages are left on the server by default, and one dead source records
        its error on its own row instead of stopping the others
  - [x] `qm_feed_*` admin functions, `quackcitadm.sh feed`, `/admin/feeds`
  - [x] `feed::Parse` is a pure function, so `test/sql/feed.test` pins down
        every shape of feed offline; `test_fetch.py` points the new clients at
        QuackCit's **own** POP3 and IMAP listeners, so there is no fixture
        server to drift and any client/server disagreement surfaces there

- [x] **Mailing list manager** (`quackmail_spool`, `core/src/listserv.cpp`)
  - [x] a list *is* a room: `citadel_lists` / `citadel_list_subs` /
        `citadel_list_held`, which is Citadel's `listrecp`/`digestrecp` model in
        tables
  - [x] distribution is a spooler over the room's message pointers, not a hook
        in the SMTP handler — so a post from telnet, NNTP, webmail or `ENT0`
        reaches subscribers, which a delivery-path hook would silently miss
  - [x] RFC 2369/2919 `List-*` headers, a `<list>-bounces@` envelope, subject
        tags and footers; inbound `DKIM-Signature`/`Authentication-Results`/
        `Return-Path`/`List-*` stripped (the rewriting invalidates a signature,
        and a sender-supplied `List-Unsubscribe` is a hijack)
  - [x] self-service by mail (`-subscribe`, `-unsubscribe`, `-request`,
        `-confirm-<token>`) and at `/lists`, both gated on a token mailed to the
        address claimed; `multipart/digest` batching; a moderation queue where
        approval posts into the room and the spooler does the sending
  - [x] `qm_list_*` admin functions, `quackcitadm.sh list`, `/admin/lists`
  - [x] `core/worker.hpp` — `PeriodicWorker`, lifted out of `smtp_out`'s relay
        drainer and now shared by it. `util::RfcDate` and `net::Connect`
        (with a connect timeout the old private copy lacked) lifted likewise
  - [x] **`deploy/` starts background workers at all.** `qm_smtp_relay_start`
        appeared nowhere in `deploy/`, so on a real install the outbound queue
        was drained by nothing: submitted mail, alias forwards and Sieve
        redirects all queued and never left. `quackcit_workers` now covers the
        relay drainer and the listserv spooler.

- [x] **Misc fixes and features.**
  - [x] IMAPS: the implicit-TLS twin of the IMAP listener (993; dev 1993), so
        every protocol now has one.
  - [x] Deploy: `QUACKCIT_PORT_SMTP_IN` → `QUACKCIT_PORT_SMTP` and
        `QUACKCIT_PORT_SMTPS` → `QUACKCIT_PORT_SUBMISSIONS`, named after the
        service rather than the module. The old names still work for one release.
  - [x] ANSI colour in the BBS shell, finally reading the long-plumbed
        `US_COLOR` bit. Gated on TERMINAL-TYPE so a dumb terminal never sees an
        escape, and the pager counts printable columns rather than bytes.
  - [x] `citadel_sessions.host` is written at last: RWHO, the telnet who-list
        and both web who-lists show where a session came from. The native
        extension's duplicate session helpers are gone, which also fixed its
        blank Client column.
  - [x] System messages to the Aide room (`qm_aide_log`): new users, aide
        actions, and optionally refused inbound mail (`qm_aide_log_rejects`,
        off by default).
  - [x] RFC 4314 IMAP ACLs — `SETACL`/`DELETEACL`/`GETACL`/`LISTRIGHTS`/
        `MYRIGHTS` and the `ACL` capability, closing a parity gap with the
        oracle. Rights are derived from the room and unioned with stored grants.
  - [x] `citadel::CanPost`, one post predicate for the web front-end, NNTP,
        telnet and `ENT0` — which had no check at all.
  - [x] Public room mail at `room_<name>@<fqdn>`, opted in by granting `anyone`
        the `p` right, plus RFC 5233 subaddressing (`user+detail@`) with
        `envelope :detail`/`:user` in Sieve.
  - [x] A bundled Public Suffix List behind DMARC's organizational domain,
        with `qm_psl_org_domain`/`qm_psl_suffix` and `test/sql/psl.test`.
  - [x] Web colour themes (per-user, with a site default) and a typed
        `/admin/prefs` settings page beside the raw config list.
- [x] **Phase 0 — GitHub sync.** Merged branches pruned; v0.3.0 re-cut as
      **v0.3.1** (PR #9).
- [x] **Phase 1 — POP3 parity + agent docs** (PR #10)
  - [x] Citadel command set, wording and UPDATE-state semantics
        (`STLS`, `LAST`, `TOP <n> <lines>`, CAPA, exact `-ERR` strings)
  - [x] `qm_pop3s` implicit-TLS listener (Citadel's 995)
  - [x] `citadel::RenderRfc822`/`MessageId` in core — the RFC822 view POP3 serves
        for native messages (NNTP reuses it)
  - [x] `test/integration/test_pop3.py`, sqllogictest rows, launcher, README
  - [x] `CLAUDE.md` / `MEMORY.md` / `TODO.md`; `HANDOFF.md` removed
  - [x] built and diffed against the oracle on `debian.lan`
- [x] **Phase 2 — telnet + telnets BBS shell** (PR #11; `quackmail_telnet`,
      dev 2300 / 2992, real 23/992 — the oracle has no telnet listener)
  - [x] IAC option negotiation + line editing in `core/src/telnet.cpp`
  - [x] presence/instant messages moved into core so telnet and native Citadel
        sessions share the who-list and paging
  - [x] commands from `citadel.rc`: login/new user, `G`oto, `K`nown rooms,
        `U`ngoto, `M`ail, read `N`ew/`O`ld/`F`orward/`R`everse/`L`ast five,
        `E`nter message, `W`ho is online, `P`age a user, `X` expert mode,
        `?`Help, `T`erminate, and the `.` dot-command dispatcher
  - [x] verified with the real `telnet` client; message posted over telnet reads
        back over the native Citadel protocol
- [x] **Phase 3 — NNTP + NNTPS** (PR #12; `quackmail_nntp`, dev 1119 / 1563)
  - [x] reader parity with the oracle (CAPABILITIES, AUTHINFO, LIST ACTIVE/
        NEWSGROUPS/OVERVIEW.FMT, GROUP/LISTGROUP, ARTICLE/HEAD/BODY/STAT,
        NEXT/LAST, OVER/XOVER, NEWGROUPS, DATE, HELP, STARTTLS)
  - [x] `RoomToNewsgroup`/`NewsgroupToRoom` + wildmat in core; personal room keys
        padded to Citadel's `0000000002.Mail` form so group names match exactly
  - [x] **POST**, plus `<message-id>` fetches and real `:bytes`/`:lines`
  - known divergences from the oracle, all deliberate: posting-related
    capabilities/flags, and RFC-correct `205` for QUIT (Citadel sends `221`).
    Room ordering was also on this list until Phase 6 traced it to `cmd_lkra`
    walking the room database in key order; `ListRooms` now matches.
- [x] **Phase 4 — XMPP** (PR #13; `quackmail_xmpp`, 5222/5223; dev 15222/15223
      because the oracle owns 5222 on the test box)
  - [x] incremental XML stream tokenizer in core (no expat in the extension
        build) + `net::ClientStream::WaitReadable`/`ReadAvailable` so a session
        can push unsolicited stanzas
  - [x] STARTTLS, SASL PLAIN, legacy `jabber:iq:auth`, bind, session, roster and
        presence from `citadel_sessions`, `<message>` ↔ `citadel_express`,
        vcard-temp, ping, disco, `503` fallback
  - [x] verified against the oracle stanza by stanza; XMPP → native `GEXP`
        confirmed live

- [x] **Phase 5 — SMTP authentication, policy and operations** (PR #14)
  - [x] `core/`: SPF (RFC 7208), DKIM sign+verify+keygen (6376, plus ed25519
        from 8463), DMARC (7489), DNSBL lookups, and the DNS TXT/A/AAAA/PTR
        queries they all need
  - [x] `policy::` site policy in `core/src/mailpolicy.cpp` — hosted domains,
        aliases and catch-alls, allow/block ACLs, DNSBL zones, DKIM keys,
        per-user quotas, and the inbound audit log
  - [x] inbound: checks run at the protocol stage they belong to, with
        `Received:`/`Authentication-Results:`/`Received-SPF:` stamped on every
        accepted message; rejects only on the sender's own `p=reject` or a
        DNSBL listing, each overridable through `citadel_config`
  - [x] outbound: DKIM signing at submission (one signature covers the local
        copy and every queued copy) and per-recipient rate limiting, default
        100/300 s and 500/24 h, refused with a transient `451`
  - [x] LMTP listener (RFC 2033) in `smtp_in` — `LHLO`, per-recipient replies,
        all spam checking bypassed; addressing and Sieve still apply
  - [x] real RFC 5228 Sieve engine (lexer → parser → evaluator, multiple
        actions with implicit keep) replacing the regex placeholder, and a real
        RFC 5804 ManageSieve server replacing the stub
  - [x] `deploy/quackcit.sh` + `quackcit.conf` (config-driven run/stop/status)
        and `deploy/quackcitadm.sh` (CLI administration, over the launcher's
        admin socket while the server holds the database file)
  - [x] `test/sql/mailpolicy.test`, `test/sql/sieve.test`, and integration tests
        for policy, LMTP and ManageSieve; README refreshed
  - [x] built on `debian.lan`; `make test` (211 assertions) and all 12
        integration tests green, including the 8 pre-existing ones
  - [x] probed against real DNS: SPF follows `redirect=` and matches ip4 CIDRs
        (gmail.com → pass/softfail/none), DMARC reads live policies
        (google.com `p=reject`, gmail.com `p=none; sp=quarantine`), DKIM
        fetches a real two-segment TXT key (github.com/s1), DNSBL detects
        Spamhaus's 127.0.0.2 test entry with its code and reason
  - [x] `deploy/` scripts exercised end to end: admin CLI works with the server
        both up (0600 socket) and down, every listener binds its configured port
  - [x] cross-read against the Citadel oracle on port 25 — fixed the SMTP
        banner to lead with the configured FQDN, as Citadel requires
  - [x] PR #14 opened
  - remaining oracle divergences, all cosmetic and deliberate:
        Citadel's EHLO reply is `250-Hello <helo> (<rdns> [<ip>])` where ours is
        `250-quackmail greets <helo>` (matching would need a reverse-DNS lookup
        per connection); Citadel offers `HELP` and advertises `AUTH` on port 25,
        where our MX deliberately offers neither

- [x] **Phase 6 — BBS shell depth + a web front-end (HTTP/HTTPS)** (PR #15)
  - [x] `core/`: zapped rooms and room passwords in the previously dead
        `citadel_room_state.flags`; `DeleteMessage`/`MoveMessage` lifted out of
        the native extension; `ListUsers`/`GetUser`/`SetAxLevel`/`SetUserFlags`
        with Citadel's canonical `US_*` bits in the equally dead
        `citadel_users.flags`; `citadel_user_reg` (the eight `REGI` fields plus
        a biography); `UpdateRoom`, floor rename/kill, and a batched
        `RoomStatsBulk` so a room listing is one query instead of ~4N
  - [x] `core/http.{hpp,cpp}`: an HTTP/1.1 server codec — request reader with
        limits and deadlines, response writer, percent/urlencoded/multipart
        codecs, HTML escaping, path normalization, cookies and a route matcher;
        `net::ClientStream::ReadN` and opt-in `SetTimeouts` behind it
  - [x] `quackmail_http`: server-rendered webmail, the BBS over the web, a
        preferences area, and an admin console covering everything
        `quackcitadm.sh` does
  - [x] telnet: floors and the `;` submenu, zap and the `.Known` filters,
        `S`kip/`A`bandon distinct from `G`oto, registration/bio/configuration,
        the user listing, message delete/move and the `.Admin` family, NAWS
        terminal sizing and a real pager — plus the four known bugs fixed
  - [x] native `REGI`/`GREG`/`EBIO`/`RBIO`/`LIST`, so the official text client
        sees the same registration data
  - [x] `test/sql/http.test` and `test/sql/citadel_store.test`;
        `quackcitadm.sh websession`; README, `quackcit.conf` and deploy wiring
  - [x] `test/integration/test_http.py` — auth and session handling (the stored
        token is the SHA-256, never the cookie), CSRF including the per-session
        binding, account-enumeration resistance, IDOR from three angles, XSS
        from both a raw and an RFC 2047-encoded hostile subject, the sandboxed
        HTML part and its CSP, attachment framing, admin gating with
        re-authentication, the DKIM private key staying off the page, the
        HTTPS redirect, malformed-request handling, and a slow-loris regression
  - [x] `test_telnet.py` extended: expert/floor mode persisting as `US_*` bits,
        floor-grouped listings, `S`kip leaving a room unread where `G`oto does
        not, zap (Lobby refused, `.KZ` listing, restored on visit), `.EG`
        registration setting `US_REGIS`, `.RU` user listing, message deletion
        unlinking the pointer but keeping the row, and aide gating
  - [x] built clean on `debian.lan`; **243 sqllogictest assertions and all 13
        integration tests green**
  - [x] manual pass through the real `deploy/run_quackcit.py` launcher: every
        listener bound, `quackcitadm.sh` driven over the launcher's admin
        socket while the server held the database, each web page read as text,
        and the BBS driven with the actual `telnet` client. Two things it
        caught that no assertion would have: room badges read "Lobby of 0" when
        nothing was unread (now "empty" / "N messages" / "3 new of 10"), and
        the toggle wording did not match the text client. Confirmed end to end
        that a message posted in the browser reads back over the native
        Citadel protocol, and that `/admin/dkim` shows only the public key.
  - [x] byte-level parity diff: the official `citadel` text client driven
        against QuackCit (through the `LD_PRELOAD` port shim) and against the
        oracle, same script, screens compared. It found three real divergences,
        all fixed and re-verified:
        - `PASS`/`NEWU` returned `0` for the `US_*` field, so the official
          client never saw expert or floor mode — silently defeating the whole
          point of persisting those bits. The reply now carries the real user
          record, including the e-mail field and `last_call`.
        - room listings were ordered by floor/listorder; the real server walks
          the room database in key order, which puts every mailbox first
        - `100` listings all said "listing follows"; the real server has
          per-verb text ("Known rooms:", "Server info:", "msg:", ...)
  - known divergences that remain, all deliberate: branding (node name, banner
    text) and the NNTP posting support QuackCit adds on purpose
- [x] **GitHub sync + TLS out of the box.**
  - [x] every branch from PRs #9–#17 was fully merged into `main` with no open
        PRs; the eight merged local branches and the three `origin/claude/*`
        remote branches are gone, leaving `main`. (`xmpp` survives only because
        it is checked out in a local worktree.)
  - [x] `deploy/quackcit.sh` generates a self-signed certificate on first start
        when none is configured, under `$QUACKCIT_TLS_DIR`. Until now
        `start_call` skipped **every implicit-TLS listener** without one, so a
        fresh install had no SMTPS/POP3S/TELNETS/NNTPS/XMPPS and — because
        `qm_web_force_https` defaults to 1 — a web interface that redirected to
        a port nothing was listening on. `core/src/tls.cpp`'s in-memory cert
        did not help: ephemeral, per-listener, `CN=localhost`.
  - [x] issued to `hostname -f` with `localhost`/`127.0.0.1`/`::1` SANs, ten
        years, written through `.tmp` files so an interrupt cannot leave a
        mismatched pair, key 0600 in a 0700 directory, and reused by every
        restart. `QUACKCIT_TLS_CERT`/`_KEY` still win; a path that does not
        resolve is now a startup error rather than a silent downgrade.
        `QUACKCIT_TLS_AUTOGEN=0` restores the old behaviour.

- [x] **Deploy scripts for the binary release.**
  - [x] `deploy/` is POSIX shell end to end; `run_quackcit.py` and
        `quackcit_admin.py` deleted. The server is the bundled DuckDB CLI
        idling on a FIFO it holds open read-write, and that FIFO is the admin
        channel `quackcitadm.sh` uses in place of the old JSON Unix socket
  - [x] `quackcit_common.sh` — layout detection (checkout vs unpacked bundle,
        nested vs flat extension tree), the listener table, SQL quoting, the
        control channel; sourced by both entry points
  - [x] `.github/workflows/release.yml` packages the scripts, `quackcit.conf`
        and a `quackcit.service` template, so a release untars into
        `/opt/quackmail` and runs

## Backlog

- Web: the wiki view (`VIEW_WIKI`/`WIKIMD`), which needs versioning, a
  name→euid resolver and a markdown renderer — deferred out of phase 2b and
  still the largest single gap against WebCit.
- Room administration, left out of phase 4 deliberately: `k` ("create rooms
  under this one") is in the rights legend but nothing reads it, so a room
  administrator cannot yet delegate *creation* the way they delegate access.
- Telnet BBS, still to fill in from `citadel.rc`: file transfer (the
  `QR_UPLOAD`/`QR_DOWNLOAD`/`QR_VISDIR` room flags and the `.Read file` /
  `.Admin File` family), `C`hat, and help files.
- XMPP, not implemented (Citadel does not have them either): MUC, offline
  storage, stored rosters/subscriptions, s2s.
- IMAP depth: `IDLE`, `CONDSTORE`/`QRESYNC`, server-side sort/thread.
- IMAP still serves `msg.raw` directly for native (format 0) messages; it should
  use `citadel::RenderRfc822` like POP3 and NNTP now do.
- Citadel breadth: `CONF`/config verbs, `EXPI` message expiry, address books /
  vCard rooms, the Citadel network mesh (inter-node replication, and with it the
  NNTP peer-feed verbs `IHAVE`/`CHECK`/`TAKETHIS`).
- SMTP: PIPELINING, CHUNKING/BDAT, DSN.
- Mail authentication depth: DMARC aggregate (`rua`) reports; ARC, so forwarded
  mail keeps an authenticated chain; MTA-STS / DANE for outbound transport.
- Sieve extensions beyond the current core + `reject`/`envelope`/`body`/`copy`:
  variables, regex, vacation, imap4flags.
- Hardening: SCRAM-SHA-256, bcrypt/argon2 password hashing, charset transcoding
  beyond UTF-8/Latin-1.
