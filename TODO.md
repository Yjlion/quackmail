# TODO.md

Live task list. Context in [MEMORY.md](MEMORY.md), working instructions in
[CLAUDE.md](CLAUDE.md).

## In flight

Nothing in flight. Next work comes off the backlog below.

## Shipped

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
