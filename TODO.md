# TODO.md

Live task list. Context in [MEMORY.md](MEMORY.md), working instructions in
[CLAUDE.md](CLAUDE.md).

## In flight

Nothing.

**v1.0.1** shipped seven improvements on top of the interface rebuild: notes
colours, an out-of-office front door, per-user storage quotas, a chat view at
`/chat`, web presence in *Who is online*, the compose rewrite, and sortable,
movable, resizable listings.

**v1.0.0** rebuilt the web interface on Pico CSS and htmx — a two-pane mailbox,
a real theme mechanism, conversation grouping, keyboard shortcuts, a phone
layout, and multilingual support past the scaffolding.

**v0.9.0** shipped ten web UI pull requests (#40–#50): the i18n scaffolding,
rich text in Notes/Calendar/Blog, ACL checkboxes, note swatches, mail list
density, the date-format preference, the address-book picker, the centred login,
the admin config descriptions and the live Sieve builder. None of them was ever
written up here — they are in `git log` and nowhere else, which is the gap this
file's archive split is meant to stop recurring.

**v0.8.0** shipped an ACME client, the wiki room view, `tools/screenshots.py`
and the `docs/` split.

**v0.7.0** shipped DAV scheduling (CalDAV free/busy and iTIP/iMIP), IMAP
`IDLE`, scrypt password hashing, the `k` right, a real JMAP client's worth of
fixes, webmail search and folder views, and Sieve past its core — `imap4flags`,
`variables` and `vacation`.

## Shipped

Unreleased, on top of v1.0.1.

- [x] **The composer's editor is Squire.** The hand-rolled `contenteditable`
      editor was built on `document.execCommand`, which is deprecated in every
      browser that implements it, cannot report caret state well enough to light
      a toolbar button, and had nothing to sanitize markup with — so every paste
      from a web page was flattened to bare text on the way in. Squire is
      Fastmail's editor, MIT, 60 KB minified, and its `dist/` is committed
      upstream, so vendoring it is a download and a `tools/gen_assets.py` run.
      The file's own header used to argue against exactly this ("TinyMCE or
      Quill: 200 KB to 1 MB … needing a build step this repo does not have");
      that comment is rewritten rather than left contradicting the code, because
      Squire is the case it did not consider. It loads only on the two pages
      that can open a composer, so no other page's cold load changed.
  - [x] **Squire's default sanitizer calls a global `DOMPurify`**, which this
        tree does not vendor — `setHTML` and every paste would have thrown.
        Supplying `sanitizeToDOMFragment` is therefore mandatory, and since it
        had to exist it implements the *server's* allow-list from
        `SanitizeForCompose`. The two must change together: a mismatch is not a
        hole, because the server sanitizes regardless, but it is formatting the
        editor shows and the message silently drops — which is the same shape as
        the `style=` bug that cost a release.
  - [x] **Clicking the message body focused a toolbar button.** The editor
        mounted inside the `<label>` wrapping the textarea, and a click anywhere
        in a label is forwarded to that label's own control — with the textarea
        hidden under the editor, that was the first button in the toolbar, so
        the caret never landed and typing went nowhere. Present in the
        execCommand editor too and never noticed: nothing outside a browser can
        see it, and the editor had no browser test. The body field is now an
        explicit `<label for>` beside a `<div>`, which keeps the association for
        a browser with no script and stops swallowing the click for one with it.
  - [x] `qcSyncBody` strips U+200B out of the plain-text half. Squire parks a
        zero-width space in an inline node with no text yet — click Bold on an
        empty line and it sits there until you type. `getHTML()` strips them;
        `innerText` does not, so every message's text half carried an invisible
        character its HTML half did not.
  - [x] `PageOpts::script` became `PageOpts::scripts`, an ordered list. A page
        that needs a library before its own code has to say so, and `defer` runs
        scripts in document order; a single slot left only the option of
        concatenating a vendored file into one of ours.
  - [x] **`test_web_ui.py` covers the editor**, which nothing did before —
        `test_richmail.py` posts `html_body` over urllib and proves the server's
        half without ever running the editor. It now types, formats, pastes
        hostile markup, and asserts what reaches the two form fields. Both bugs
        above were found by writing it.

- [x] **DAV depth: `MKCOL`/`MKCALENDAR`, the whole `calendar-query` filter tree,
      and `expand`.** All three were one backlog bullet; they turned out to
      share a prerequisite, which is that the filter evaluator had to become a
      *tree* before anything else was worth doing.
  - [x] **The filter evaluator is recursive and lives in `core/`.** What it
        replaced was two flat extractors that looked exactly two `comp-filter`
        levels deep — `FilterComponent` and `FilterTimeRange` — evaluated by two
        `if`s, and which re-parsed each object's iCalendar twice to do it. Now:
        nested `comp-filter`, `prop-filter`, `param-filter`, `text-match` with
        all four match types, `is-not-defined`, and `test="allof"|"anyof"` at
        every level. `addressbook-query` had the same gaps and closes with it.
        It is in `core/` because it is pure — a filter and a body in, a boolean
        out — which is what lets `qm_caldav_filter`/`qm_carddav_filter` expose
        it to `test/sql/dav.test`. Filter combinatorics are what sqllogictest is
        good at and what a socket-driven Python test is bad at.
  - [x] `ical::ItemFromComponent` was extracted out of `ParseItems`, so a caller
        that needs the component tree *and* the flattened items — which is what
        a filter with both a `prop-filter` and a `time-range` needs — gets both
        from one parse rather than parsing the same body twice.
  - [x] **`expand` reuses `ical::Expand`**, the engine already behind the web
        calendar grid, free/busy and the time-range filter, cap included. What
        it adds is turning each instant back into a real VEVENT — and
        `RECURRENCE-ID` was modelled nowhere in the tree before this.
  - [x] **A collection a client creates keeps the client's URL.** A collection
        segment is a room *number* (a Citadel room name may contain `/`), so a
        `MKCALENDAR /dav/calendars/ann/work-trips/` had nowhere to go. A
        client-chosen segment is now bound in `citadel_dav_collections`,
        resolved after the numeric form fails — every URL that worked before
        still works, and a 201 no longer promises a URL the server then serves
        something else at. The binding is dropped with the room, or a reused
        room number would point an old URL at a new room.
  - [x] Creating a collection goes through the *same* gate as creating a room
        from the web (`MayCreateRoomOnFloor`, now shared rather than copied) and
        the same rights grant afterwards — without which the creator holds
        derived rights only and cannot write to what they just made.
  - [x] **`DELETE` on a collection**, gated on the `a` right. It was a flat 405
        on the reasoning that the consequences exceeded what a DELETE could
        express; that held until a client could *create* one, and a client that
        can make a calendar and not remove it leaves litter it cannot clean up.
  - [x] `calendar-description` is writable. PROPFIND had always read it from
        `room.info` and PROPPATCH could never set it — `MKCALENDAR` carries one,
        so the write path had to exist, and it belongs on both verbs.
        `ApplyCollectionProp` is now shared, so creating a calendar with a
        colour and PROPPATCHing one on afterwards cannot disagree.
  - [x] `extended-mkcol` joins the `DAV:` header, and the two verbs join
        `Allow`. Neither is advertised until it works: a class we claim and do
        not honour is one a client keeps trying to use.

- [x] **File areas, `C`hat and help files** — the rest of what `citadel.rc` has
      and this server did not.
  - [x] **The `QR_UPLOAD`/`QR_DOWNLOAD`/`QR_VISDIR` flags are read.** All three
        have been in `citadel_store.hpp` since the beginning and nothing had ever
        looked at them; the only file-area behaviour in the tree was the telnet
        prompt printing `]` for a `QR_DIRECTORY` room.
  - [x] **A file is a message.** A directory room's files are euid-keyed
        messages carrying one attachment part — the same shape as a mail
        attachment and as the wiki's binary payloads. That is the whole design
        decision, and it is what makes storage quotas, the room ACL, DAV
        tombstones and `KillRoom`'s cleanup all apply to files without any of
        them being taught about files. A files table would have had to re-earn
        every one of those.
  - [x] **One store, three front doors.** `core/filearea.hpp` owns the
        permission questions, so telnet, WebDAV and (next) FTP ask rather than
        each re-deriving "a file is an attachment part" — the same rule that has
        every front-end ask `CanPost`. `test_telnet.py` uploads over telnet and
        fetches over WebDAV byte for byte, including `0x00` and `0xFF`, because
        otherwise "three front doors over one store" is a claim rather than a
        fact.
  - [x] Telnet: `.RF`/`.RFG`/`.EF` and `.AFD`/`.AFE`, with **Xmodem-1K and
        CRC-16** in both directions, a plain type-out through the pager, and
        base64 for a terminal with no transfer protocol. Zmodem is deliberately
        out — much more protocol for the same result over a link TCP has already
        made reliable.
  - [x] `telnet::Session` gained raw byte I/O for that, which meant splitting
        `NextPayloadByte` out of `GetChar`: a binary transfer needs the IAC
        handling and must **not** have the CR/LF line discipline on top, because
        a `0x0D` inside a file is data. `WriteRaw` doubles `0xFF` — without that
        a payload byte of `0xFF` reads at the far end as the start of a telnet
        command, which is the classic way binary transfer over telnet corrupts a
        file.
  - [x] **WebDAV at `/dav/files/`**, a third `DavKind` — and the first selected
        by a room *flag* rather than by a view. A file carries its own media type
        and size, so `DavObject` gained both: a PROPFIND over a directory of
        large files must report sizes without reading all of them.
  - [x] **`LOCK`/`UNLOCK`, for file areas and nothing else.** This reverses the
        "deliberately absent" note in the old backlog, and only here: ETags and
        `If-Match` remain the entire consistency story for calendars and
        contacts, which is what every CalDAV client speaks. A file share is the
        case where that is not enough — Explorer and Finder refuse to mount a
        class-1 WebDAV share read-write. `DAV: 2` is therefore per-path, and
        `test_caldav.py` asserts both halves: claimed under `/dav/files/`, not
        claimed on a calendar.
  - [x] **`C`hat on `citadel_express`**, not on a channel of its own, so telnet
        chat, `P`age, the web `/chat` view, XMPP and native `SEXP`/`GEXP` are one
        conversation instead of five. Talking to "everyone" fans out one row per
        online user rather than inventing a table only one front-end understands.
  - [x] **Help files**: compiled-in topics, overridable from a `Help` room by
        euid. Real Citadel reads help off disk; this server is a single loadable
        extension with no data directory to install, which is the same reason
        `http/assets/` is compiled in. A site that never touches it still ships
        working help rather than an empty screen.

- [x] **FTP and FTPS** (`quackmail_ftp`), the third front door onto the file
      areas. The same rooms, flags and permission questions; nothing about a
      file is stored twice.
  - [x] **`core/net.cpp` gained `ListenEphemeral` and `AcceptOnce`.** The only
        `socket`/`bind`/`listen` in the tree was inside `ServerController`, which
        owns one long-lived listener and an accept thread per protocol. A
        passive data channel is the opposite: a socket that exists for one
        transfer. `qm_ftp_pasv_low`/`_high` bound the range, because no
        firewalled deployment can work without that, and the range is tried a
        port at a time since the kernel has no way to be told "anything in
        50000-50100".
  - [x] **`citadel::MayCreateRoom` moved into core.** Three front doors ask it
        now — the web's "new room", DAV's `MKCOL` and FTP's `MKD` — and the
        site-axlevel rule was living in `web_rooms.cpp` where only one of them
        could see it. Found by `MKD` refusing a user the web would have allowed.
  - [x] Three deliberate refusals, each stated in `docs/protocols.md` rather
        than left looking unimplemented: **cleartext credentials are refused by
        default** (`534` unless `qm_ftp_allow_cleartext`), because every other
        protocol here has a TLS story; **`PORT`/`EPRT` answer `502`**, because
        active mode makes the server dial an address the client names; and no
        `APPE`/`REST`/`STOU`, because a file is one message written whole.
  - [x] `RNFR`/`RNTO` is a store under the new name and a remove of the old: the
        name *is* the message's euid, not a column to update.
  - [x] The `add-module` checklist end to end, including the step that fails
        silently — `release.yml`'s hardcoded extension list, which would have
        built and tested fine locally while shipping no artifact.

- [x] **Citadel breadth, part 1: `CONF` and message expiry.** Both were read out
      of Citadel's own source before a line was written, and both premises in the
      backlog turned out to be wrong in ways that would have shipped a protocol
      interoperating with nothing.
  - [x] **`CONF` is not the positional verb the backlog assumed.** The modern
        form is key/value — `GETVAL`/`PUTVAL`/`LISTVAL` — which maps straight
        onto `citadel_config` with no translation at all. The positional
        `GET`/`SET` still exists, is marked *"deprecated; please do not add
        fields or change their order"* in `control.c`, and is still sent by
        Citadel's own text client — so all **73** positions are answered,
        retired ones included as blank lines, because renumbering to close a gap
        shifts every field after it. `GETSYS`/`PUTSYS` store stanzas as
        euid-keyed messages in *Local System Configuration*, which is
        `UpsertByEuid` and needed no new mechanism.
  - [x] **There is no `EXPI` verb.** Citadel's is `TDAP`, "manually initiate
        auto-purger". And the modern Citadel *server* implements neither `GPEX`
        nor `SPEX`, though its own client still sends them — that client's
        expiry editor talks to nothing against a real Citadel. It works here.
  - [x] The policy model is Citadel's, numbers included: next-level, manual, by
        count, by age, at room / floor / site / mailboxes. With nothing set
        anywhere the answer is **manual** — deleting mail on the strength of an
        empty configuration would be the worst available reading of it. And
        `QR_PERMANENT` finally means something: it has been set on seeded and
        personal rooms since the beginning and nothing has ever read it.
  - [x] The sweep is a `PeriodicWorker` with a `qm_expire_run()` one-shot, not
        another passenger on the 1-in-16 coin flip in `web_router.cpp`. Every
        other sweep in this tree rides that, which means a site with no web
        traffic sweeps nothing — tolerable for tombstones, not for the sweep
        that deletes messages.
  - [x] A second pass collects `citadel_messages` rows no `citadel_room_msgs`
        entry points at. `DeleteMessage` deliberately leaves the row when
        another room still holds a copy, which is right; without the second pass
        the unreferenced remainder would accumulate forever.
  - [x] `citadel::SetConfig` and `ListConfig` in core. The read side has been
        there since the beginning; the write side was the same hand-rolled
        upsert in four places.

Released work lives in [TODO-archive.md](TODO-archive.md), newest first.

## Backlog

The first came out of building 0.6.0 and is the one most likely to bite.

- **RFC 6638 auto-scheduling**, the last of DAV scheduling: the
  `schedule-inbox-URL` / `schedule-outbox-URL` collections, a `POST` to the
  outbox (which is how a client asks for somebody else's free/busy over iTIP
  rather than through the `free-busy-query` REPORT), `schedule-default-
  calendar-URL`, the `CALDAV:schedule-send` / `schedule-deliver` privileges,
  and the `calendar-auto-schedule` compliance token — which stays off the
  `DAV:` header until all of that exists, because advertising it is a promise
  clients act on. `ParseDavPath` has no room for the two new collections and
  `DavHandler` has no `POST` branch, so both are the first work. iMIP already
  ships (above), which is the half that interoperates with servers other than
  this one; this is the half that is a convenience for clients already talking
  to us.

- DAV depth beyond scheduling, what is left of it: `COPY`/`MOVE`, and
  `LOCK`/`UNLOCK` — the latter deliberately absent, because ETags and
  `If-Match` are the consistency story. `MKCALENDAR`/`MKCOL`, the full
  `calendar-query` filter tree and `expand` have shipped; see above. Notes rooms
  stay out either way: vNote is not a DAV resource type.
- Telnet BBS: file transfer, `C`hat and help files have shipped (see above).
  What is left from `citadel.rc` is the native file-transfer verbs —
  `OPEN`/`READ`/`CLOS`/`UOPN`/`WRIT` — which would give the official `citadel`
  text client downloads over the same store.
- XMPP, not implemented (Citadel does not have them either): MUC, offline
  storage, stored rosters/subscriptions, s2s.
- IMAP depth: `CONDSTORE`/`QRESYNC`, server-side sort/thread, `BODYSTRUCTURE`.
- JMAP depth: `Email/import`, `SearchSnippet/get`, push over EventSource, and
  `Email/query` sorts other than newest-first. `Thread/get` scans the account
  rather than an index, because a thread id is a function of the References
  header rather than a stored column — fine at BBS scale, wrong at mailbox
  scale.
- Citadel breadth, what is left. `CONF` and expiry have shipped (above).
  Remaining:
  - **The native file-transfer verbs** — `OPEN`/`READ`/`CLOS`/`UOPN`/`UCLS`/
    `WRIT`, plus `DELF`/`MOVF` and the `RDIR`/`QDIR` listings. These sit on the
    file areas that already exist, and would give the official `citadel` text
    client downloads. The best-specified piece left, and the natural follow-on
    to the file-area work.
  - **Address books**: a user's vCard is not published to the Global Address
    Book on `REGI`, so that room stays empty on a real system. Citadel's verbs
    are `GVSN`/`GVEA`/`DVCA` — note there is no `IGAB`, whatever this list said
    before. `ContactAddressOptions` in the web composer also reads only the
    user's own Contacts room, so it would pick up colleagues for free.
  - **The Citadel network mesh**, and the NNTP peer-feed verbs. Worth knowing
    before starting: **Citadel's own NNTP implements none of
    `IHAVE`/`CHECK`/`TAKETHIS` or `MODE STREAM`** — it has ACTIVE, AUTHINFO,
    GROUP, LISTGROUP, NEWSGROUPS and the article verbs and stops there. So this
    is RFC 3977/4644 work with no parity oracle behind it, unlike everything
    else in this section. The prerequisite either way is a Message-ID index:
    `IHAVE` and `CHECK` ask "do I already have this?" and there is no index to
    answer from.
- SMTP: PIPELINING, CHUNKING/BDAT, DSN.
- Mail authentication depth: DMARC aggregate (`rua`) reports; ARC, so forwarded
  mail keeps an authenticated chain; MTA-STS / DANE for outbound transport.
- Sieve `regex`, the one extension of the four left undone. It is an expired
  draft rather than an RFC, and the value it adds over `:matches` — which now
  captures into `${1}`..`${9}` — is small next to putting a backtracking engine
  on the delivery path against text a sender chooses. Reconsider only with a
  regex implementation that is bounded by construction.
- Hardening: SCRAM-SHA-256, charset transcoding beyond UTF-8/Latin-1.
- **`core/src/wildmat.cpp` has no step budget.** `MatchItem` recurses over every
  `*` split with no counter, so a pattern like `*a*a*a*a*b` against
  sender-chosen text is exponential — exactly the property the Sieve `regex`
  refusal above was written to avoid, and exactly what the 0.7.0 capture matcher
  was given a budget for. NNTP wildmat patterns come from a client, so this is
  reachable.
- **The telnet front-end reads a spurious empty command when server-side echo is
  on.** Accept its `WILL ECHO` (send `IAC DO ECHO`) and every real command is
  followed by an empty line the BBS answers with "Unknown command. Press ? for
  help." Reproducible with three lines of socket code; `tools/screenshots.py`
  works around it by declining the offer and echoing locally.
