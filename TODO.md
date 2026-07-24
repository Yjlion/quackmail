# TODO.md

Live task list. Context in [MEMORY.md](MEMORY.md), working instructions in
[CLAUDE.md](CLAUDE.md).

## In flight

- [x] **Phase 0 — GitHub sync.** Prune merged branches; re-cut the broken v0.3.0
      release as **v0.3.1** (PR #9 — merge it, then tag `544ca73`'s successor).
- [ ] **Phase 1 — POP3 parity + agent docs** (branch `pop3-parity`)
  - [x] Citadel command set, wording and UPDATE-state semantics
        (`STLS`, `LAST`, `TOP <n> <lines>`, CAPA, exact `-ERR` strings)
  - [x] `qm_pop3s` implicit-TLS listener (Citadel's 995)
  - [x] `citadel::RenderRfc822`/`MessageId` in core — the RFC822 view POP3 serves
        for native messages (NNTP will reuse it)
  - [x] `test/integration/test_pop3.py`, sqllogictest rows, launcher, README
  - [x] `CLAUDE.md` / `MEMORY.md` / `TODO.md`; `HANDOFF.md` removed
  - [ ] build + run the tests on `debian.lan`, diff against the oracle, open PR

## Next

- [x] **Phase 2 — telnet + telnets BBS shell** (`quackmail_telnet`, dev 2300 /
      2992, real 23/992 — the oracle has no telnet listener, so no collision)
  - [x] IAC option negotiation + line editing in `core/src/telnet.cpp`
  - [x] presence/instant messages moved into core so telnet and native Citadel
        sessions share the who-list and paging
  - [x] commands from `citadel.rc`: login/new user, `G`oto, `K`nown rooms,
        `U`ngoto, `M`ail, read `N`ew/`O`ld/`F`orward/`R`everse/`L`ast five,
        `E`nter message, `W`ho is online, `P`age a user, `X` expert mode,
        `?`Help, `T`erminate, and the `.` dot-command dispatcher
  - [x] verified with the real `telnet` client; message posted over telnet reads
        back over the native Citadel protocol
  - [ ] still to fill in from `citadel.rc`: floors, zapped/anonymous/directory
        room filters, `S`kip semantics distinct from `G`oto, registration/bio,
        file transfer
- [x] **Phase 3 — NNTP + NNTPS** (`quackmail_nntp`, dev 1119 / 1563)
  - [x] reader parity with the oracle (CAPABILITIES, AUTHINFO, LIST ACTIVE/
        NEWSGROUPS/OVERVIEW.FMT, GROUP/LISTGROUP, ARTICLE/HEAD/BODY/STAT,
        NEXT/LAST, OVER/XOVER, NEWGROUPS, DATE, HELP, STARTTLS)
  - [x] `RoomToNewsgroup`/`NewsgroupToRoom` + wildmat in core; personal room keys
        padded to Citadel's `0000000002.Mail` form so group names match exactly
  - [x] **POST**, plus `<message-id>` fetches and real `:bytes`/`:lines`
  - known divergences from the oracle, all deliberate: posting-related
    capabilities/flags, RFC-correct `205` for QUIT (Citadel sends `221`), and
    room ordering in `LIST` (ours follows floor/listorder)
- [ ] **Phase 4 — XMPP** (`quackmail_xmpp`, 5222 / 5223)
  - incremental XML stream tokenizer in core (no expat in the extension build)
  - STARTTLS, SASL PLAIN, legacy `jabber:iq:auth`, bind, session, roster from
    `citadel_sessions`, presence, `<message>` ↔ `citadel_express`, vcard-temp,
    ping, disco#info, `503` fallback

## Backlog (carried from the roadmap)

- `quackmail_managesieve` is still a stub.
- IMAP depth: `IDLE`, `CONDSTORE`/`QRESYNC`, server-side sort/thread.
- Citadel breadth: `CONF`/config verbs, `EXPI` message expiry, address books /
  vCard rooms, the Citadel network mesh (inter-node replication, and with it the
  NNTP peer-feed verbs `IHAVE`/`CHECK`/`TAKETHIS`).
- SMTP: LMTP (2033), enhanced status codes, PIPELINING, CHUNKING/BDAT, 8BITMIME.
- Hardening: SCRAM-SHA-256, bcrypt/argon2 password hashing, full Sieve, charset
  transcoding beyond UTF-8/Latin-1, DKIM/SPF/DMARC.
- IMAP still serves `msg.raw` directly for native (format 0) messages; it should
  use `citadel::RenderRfc822` like POP3 now does.
