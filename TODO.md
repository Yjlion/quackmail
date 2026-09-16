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

Nothing since v1.0.1. Released work lives in
[TODO-archive.md](TODO-archive.md), newest first.

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

- DAV depth beyond scheduling: `LOCK`/`UNLOCK` (deliberately absent — ETags and
  `If-Match` are the consistency story), `MKCALENDAR`/`MKCOL`, the
  `calendar-query` filters past comp-name and time-range, and `expand` on a
  recurring event. Notes rooms stay out: vNote is not a DAV resource type.
- Telnet BBS, still to fill in from `citadel.rc`: file transfer (the
  `QR_UPLOAD`/`QR_DOWNLOAD`/`QR_VISDIR` room flags and the `.Read file` /
  `.Admin File` family), `C`hat, and help files.
- XMPP, not implemented (Citadel does not have them either): MUC, offline
  storage, stored rosters/subscriptions, s2s.
- IMAP depth: `CONDSTORE`/`QRESYNC`, server-side sort/thread, `BODYSTRUCTURE`.
- JMAP depth: `Email/import`, `SearchSnippet/get`, push over EventSource, and
  `Email/query` sorts other than newest-first. `Thread/get` scans the account
  rather than an index, because a thread id is a function of the References
  header rather than a stored column — fine at BBS scale, wrong at mailbox
  scale.
- Citadel breadth: `CONF`/config verbs, `EXPI` message expiry, address books /
  vCard rooms, the Citadel network mesh (inter-node replication, and with it the
  NNTP peer-feed verbs `IHAVE`/`CHECK`/`TAKETHIS`).
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
