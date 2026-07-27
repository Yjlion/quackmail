# TODO.md

Live task list. Context in [MEMORY.md](MEMORY.md), working instructions in
[CLAUDE.md](CLAUDE.md).

## In flight

Nothing in flight. Next work comes off the backlog below.

## Shipped

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

## Backlog

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
- Mail authentication depth: bundle a Public Suffix List so DMARC's
  organizational domain is exact rather than the current two-label
  approximation; DMARC aggregate (`rua`) reports; ARC, so forwarded mail keeps
  an authenticated chain; MTA-STS / DANE for outbound transport.
- Sieve extensions beyond the current core + `reject`/`envelope`/`body`/`copy`:
  variables, regex, vacation, imap4flags.
- Hardening: SCRAM-SHA-256, bcrypt/argon2 password hashing, charset transcoding
  beyond UTF-8/Latin-1.
