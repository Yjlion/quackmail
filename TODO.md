# TODO.md

Live task list. Context in [MEMORY.md](MEMORY.md), working instructions in
[CLAUDE.md](CLAUDE.md).

## In flight

Nothing.

**Unreleased**, on top of v0.9.0: the web interface rebuild below.

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

- [x] **A modern web interface.** The front-end had the features and not the
      presentation: every action was a separate outlined box in an
      undifferentiated row, reading a message replaced the whole page, and the
      phone view clipped the From and Date columns to a sliver while stacking
      buttons one per line.
  - [x] **Pico CSS v2.1.1 (MIT) and htmx 2.x (0BSD) are vendored** into
        `http/assets/` and compiled in by `tools/gen_assets.py`. Still no build
        step and no CDN; their CVEs are now ours. Neither can carry a licence
        file beside it — the generator serves *every* file in that directory and
        refuses any extension outside its MIME table — so `docs/web.md` is the
        record. 144 KB uncompressed, and this server sends no `Content-Encoding`
        at all, so that is the real cold-load cost; it is `immutable` for a year.
  - [x] **`htmx.config.allowEval = false`**, and `connect-src 'self'` added to
        the CSP. htmx's only `eval`/`new Function` sit behind that flag; the
        policy still refuses `'unsafe-inline'` and `'unsafe-eval'`. The
        connect-src was not optional: with `default-src 'none'` and no
        connect-src stated, every XHR fell back to `'none'` and each pane swap
        was blocked. The message-body frame's policy is passed in separately and
        deliberately did **not** gain it.
  - [x] **The stylesheet arrives in three blocks and the order is the design**:
        the inlined skeleton first (fallback palette at plain `:root`
        specificity), then `pico.css` and `qc.css`, then the per-user theme
        inlined last. Skeleton first is what lets Pico override the fallback;
        theme last is what lets it win. The previous single-block arrangement,
        kept, would have silently defeated Pico's palette everywhere.
  - [x] **A theme is now the `data-theme` attribute plus a dozen `--pico-*`
        overrides.** Light and dark cost the attribute and nothing else; only
        sepia, slate and amber carry any CSS.
  - [x] **A two-pane mailbox.** `?open=<msgnum>` renders listing *and* message,
        so the pane is linkable and the history works; htmx fetches the same URL
        and swaps only the reader. The server chooses between the two from
        `HX-Request`, which is what keeps ~4000 lines of `urllib`-driven tests
        meaningful. `MarkSeen` was factored out of `GetBbsMessage` and
        `MessageActions` moved *into* `RenderMessage` so the pane and the
        standalone page cannot disagree.
  - [x] **`ThreadIdFor` was wrong and is fixed.** A reply hashed the first
        entry of References — the root's Message-ID — but a root was named after
        its *msgnum*, so a root could never join its own thread. JMAP had been
        shipping that since Phase 11; `Thread/get` only ever returned threads of
        one and nothing noticed. Roots now hash their own Message-ID. The web
        listing calls the same function rather than growing a second rule.
  - [x] Conversation grouping (`web_mail_threaded`, off by default, bounded by
        `qm_web_thread_scan`), keyboard shortcuts with a `<dialog>` help overlay,
        an inline `<symbol>` icon sprite (same-document `use`, so no CSP
        question and no cross-document support gap), and a phone layout where
        tables become cards labelled from `data-label`.
  - [x] **Real multilingual support.** The catalog grew a column per language —
        English, German, French — plus `TrN` for plurals, `TrF` for positional
        interpolation, `Accept-Language` negotiation for anonymous pages, and a
        hash lookup in place of the linear scan (which cost the catalog size
        times the number of strings on the page). An untranslated cell falls
        back to English rather than to the key, so a language can land
        incrementally. `test_http.py` greps rendered German for a bare `nav.` or
        `mailbox.` prefix, which is exactly what a missing string looks like.
  - [x] **The JavaScript-off guarantee is retired**, and `docs/web.md` says so.
        Pages are still rendered whole by the server, but now because a URL
        should mean something and the test suite should be able to drive the
        app — not because scripting is assumed absent.
  - [x] `test/integration/test_web_ui.py` (Playwright, system Chromium) covers
        what urllib cannot see: the swap without a navigation, back through
        `hx-push-url`, the shortcuts, thread disclosure, and the 390x844 layout.

Older shipped work — v0.8.0 and before — has moved to
[TODO-archive.md](TODO-archive.md). (v0.9.0's ten UI pull requests were never
written up in either file; see In flight.)

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
