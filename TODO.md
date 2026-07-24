# TODO.md

Live task list. Context in [MEMORY.md](MEMORY.md), working instructions in
[CLAUDE.md](CLAUDE.md).

## In flight

- [ ] **Phase 5 — SMTP authentication, policy and operations**
      (branch `claude/smtp-inbound-outbound-15982f`)
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
  - [ ] build + run the tests on `debian.lan`, probe SPF/DKIM/DMARC against
        real DNS and the Citadel oracle, then open the PR

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
    capabilities/flags, RFC-correct `205` for QUIT (Citadel sends `221`), and
    room ordering in `LIST` (ours follows floor/listorder)
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

## Backlog

- Telnet BBS, still to fill in from `citadel.rc`: floors, zapped/anonymous/
  directory room filters, `S`kip semantics distinct from `G`oto,
  registration/bio, file transfer.
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
