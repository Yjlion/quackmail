# MEMORY.md — durable project context

Long-lived context for QuackCit: what shipped, what was decided and why, and the
facts about the test environment that are expensive to rediscover. Practical
"how do I work here" instructions live in [CLAUDE.md](CLAUDE.md); the live task
list is [TODO.md](TODO.md).

## What QuackCit is

Originally "QuackMail", a mail server as a DuckDB extension. On 2026-07-21 it
was re-scoped: the centerpiece is now the **Citadel BBS/groupware protocol**, and
SMTP/IMAP/POP3 are gateways over the Citadel room store. Repo and extension
prefixes still say `quackmail`; the product is QuackCit.

## Shipped work

| Phase | Scope | Landed |
|---|---|---|
| 1 | SMTP rework: inbound MX (2525) + authenticated submission (2587/2465) + relay drainer | PR #6 |
| — | HANDOFF doc (superseded by these files) | PR #7 |
| 2 | IMAP expansion: STARTTLS + AUTHENTICATE, core RFC 3501, folders/public rooms, NAMESPACE/UIDPLUS/BODYSTRUCTURE | PR #8 |
| 3 | Citadel native: presence (`RWHO`), instant messages (`SEXP`/`GEXP`), `CHEK`, `DELE`/`MOVE`, default room/floor parity | PR #8 |
| 4 | POP3 Citadel parity + `qm_pop3s` + agent docs | PR #10 |
| 5 | Telnet/telnets BBS shell (`quackmail_telnet`) | PR #11 |
| 6 | NNTP/NNTPS reader + poster (`quackmail_nntp`) | PR #12 |
| 7 | XMPP c2s bridged to Citadel express messages (`quackmail_xmpp`) | PR #13 |
| 8 | SMTP authentication (SPF/DKIM/DMARC/DNSBL), site policy, outbound signing + rate limiting, LMTP, real Sieve/ManageSieve, `deploy/` CLI tooling | PR #14 |
| 9 | Telnet BBS depth (floors, zap, skip, registration, admin verbs) + `quackmail_http`: webmail, the BBS over the web, and the admin console | PR #15 |
| 10 | Web overhaul phase 1: persistent connections, `/static` assets, a grouped sidebar | PR #24 |
| 10 | Web overhaul phase 2a: bundled IANA tz database, vCard/iCalendar, `UpsertByEuid`, the contacts view | PR #25 |
| 10 | Web overhaul phase 2b: shared content-line grammar, vNote, viewer time zone, calendar/tasks/notes/blog views | PR #26 |
| 10 | Web overhaul phase 3: MIME builder, two HTML sanitizers, rich compose with cid:, Sieve rule builder | PR #27 |

Phases 2 and 3 were validated byte-for-byte against the real Citadel server, and
the **official `citadel` text client drives a full clean session** against
QuackCit (login banner, `<K>nown rooms`, no spurious errors).

## Decisions worth remembering

### Groupware objects are ordinary messages, keyed by euid

A contact or event is a `format_type = 4` message wrapping one `text/vcard` or
`text/calendar` part, with `citadel_messages.euid` set to the object's own UID —
Citadel's own arrangement. `format_type == 4` already means "serve `raw`
verbatim" everywhere in the tree, so a contact created in the browser is a
readable MIME message over IMAP, NNTP and POP3 with **no new code**, and
`mime::FlattenParts` finds the part. `test/integration/test_contacts.py` checks
that rather than assuming it.

Saving an object again replaces it: `citadel::UpsertByEuid` inserts and then
unlinks the previous one. It is a separate call rather than something
`InsertMessage` learned to do, because ordinary messages have no identity and
must keep appending. It lives in core so the native `ENT0`/`EUID` path, the web
UI and any future CalDAV module cannot disagree about what a second save means.
An empty euid is **refused** rather than treated as an insert — without one there
is nothing to replace, so accepting it would turn every later save into a
duplicate.

Insert-then-unlink, not unlink-then-insert: `InsertMessage` runs its own
transaction so the pair cannot be wrapped in an outer one, and of the two
failure modes, briefly having two copies is visible and recoverable while
unlinking and then failing to insert loses the object.

### Recurrence expands on the clock, not on the instant

"Every Tuesday at 09:00" means 09:00 *local*. `ical::Expand` therefore steps in
wall-clock terms in the event's own zone and converts each occurrence back
through the bundled tz database. Expanding in UTC is the bug every naive
calendar has: a weekly meeting silently moves an hour when the clocks change.
This is the reason the zone database had to land before the calendar, not
alongside it.

Two representations exist for the same reason: `ical::Component` is the tree as
it arrived, `ical::Item` is the flat shape a form edits. Saving goes tree → item
→ form → item → tree, so alarms, `X-` properties and attendee parameters survive
an edit. `EmitItem` builds a *fresh* component and is only for creating an
object; using it to save an edit is how someone's phone reminder disappears, and
its docstring says so.

### The time zone database is bundled, like the PSL

Not DuckDB's `icu` extension — PR #23 deliberately removed that dependency, and
a calendar that works only when an optional extension is installed is not a
calendar. Not `/usr/share/zoneinfo` either: minimal containers ship without it,
and reading it would make two installs of the same release disagree about when a
meeting is based on their base image.

The cost is dated data that goes stale by political decree several times a year,
so `tz::Version()` is surfaced and refreshing is a `gen_tzdata.py` run plus a
commit. Two bugs worth remembering from building it: the type in force *before*
a zone's first transition is not `type_at[0]` (that is what applies after it —
RFC 8536 says the first non-DST type), and converting a wall clock back to an
instant has to probe the offset a day either side rather than at the wall time
itself, or the two candidates converge and an ambiguous time is silently
resolved instead of reported.

### Composed HTML gets an allow-list; received HTML gets a deny-list

Two sanitizer profiles, and the asymmetry is the point.

`SanitizeForDisplay` cleans a part that arrived in someone else's mail. A
deny-list is defensible there *only* because it is not the boundary: the part
renders inside a sandboxed frame under its own `default-src 'none'` policy, and
the sanitizer is defence in depth behind that.

`SanitizeForCompose` cleans HTML a local user typed, and is a true allow-list
applied **before the message is built**. That HTML is stored and then re-served
from our origin to other people — the recipient, everyone in a public room, every
list subscriber. "It is the user's own input" stops being true the moment paste is
involved: markup copied out of a hostile page is a stored-XSS vector aimed at
third parties, so the tests assert against the *stored* bytes rather than a
rendered page. A display-side test would pass with the hole wide open.

Specific decisions worth keeping: a URL scheme is compared after control
characters are stripped (`java	script:` is the classic bypass);
`data:image/svg+xml` is refused where png/jpeg/gif/webp are allowed, because SVG
is scriptable; an `http:` image is dropped on the way in because it is a tracking
pixel, leaving the reader's `?images=1` opt-in as the only way to load remote
images; and an element not on the list loses its tag but keeps its words.

### The Sieve script text is the single source of truth

The rule builder derives rules from the script on every load and stores nothing
structured. `quackmail_sieve_scripts` is also written by ManageSieve and by
`/admin/sieve`, so a rules table would mean regenerating destroys an out-of-band
edit, or trusting the table makes the UI describe filtering that is not what
runs. Marker comments fail the same way and evaporate the first time a
third-party client rewrites the script.

`Decompose` walks the same AST `Evaluate` runs, so what the UI shows cannot drift
from what happens. The important half is what it refuses: a nested `if`, an
`else`, an action outside any rule, an `envelope` or `exists` test each return a
sentence the page shows instead of a builder — and a rule-add against such a
script is a 400, not a silent rewrite. Rule *names* live in `# rule: <name>`
comments, and a name belongs to the `if` it sits directly above; anything in
between and it is a comment about something else.

### An all-day value is a date, not an instant

`ical::DateTime::epoch` means a UTC instant when `all_day` is false and a
**wall-clock** value when it is true, because `VALUE=DATE` names a day rather
than a moment. Shifting an all-day value through a zone anyway moves the date:
a 1 April due date became 31 March, and Christmas became Christmas Eve, for any
viewer west of Greenwich. Every renderer takes the flag rather than the bare
epoch, and the header says so where the old comment claimed "always UTC".

The same shape of bug on the write side: `FoldItemInto` only *wrote*
`PERCENT-COMPLETE` and `PRIORITY` when non-zero, so reopening a completed task
left the stored `100` in place and the task read as done again on reload. Zero
means removal, the same rule the contacts form follows for an emptied field.

### The three groupware formats share one line grammar

vCard, iCalendar and vNote are the same format with different property names, so
`core/contentline.cpp` owns the unfolder, the 75-octet folder and the escaper and
each format owns only what is specific to it (which properties are structured,
what they mean). It was extracted when vNote would have made a third copy.

Notes are `text/vnote` rather than iCalendar `VJOURNAL`, which would have needed
no new code at all. The reason is parity: WebCit and the Citadel clients read
vNote, and cross-protocol readability is the entire point of storing groupware as
ordinary messages.

### Blog and journal rooms are not groupware

They hold ordinary messages. What makes the view different is that it shows whole
entries newest-first instead of a list of subjects — so it reuses `RenderMessage`
and the existing compose and read routes rather than growing an item/edit/save
path. A second way to edit the same object would be a second place to keep
correct.

### `default_view` decides how a room renders, and `?view=raw` always escapes

The view codes go on the wire in `GETR`/`SETR`, so they are transcribed from the
`ROOM_VIEWS` enum in `libcitadel/lib/libcitadel.h` on the oracle rather than
from documentation: 6 WIKI, 7 CALBRIEF, 8 JOURNAL, 9 DRAFTS, 10 BLOG, 11 QUEUE.
A different, plausible-looking ordering was in circulation and is wrong. It is an
`enum`, not a `#define`, so `grep 'define VIEW_'` finds nothing and looks like
the constants are absent.

A room whose view has no renderer keeps rendering as a message board, which is
always truthful — the objects really are messages. `?view=raw` forces that for
any room, and you want it the first time a Contacts room contains something that
is not a vCard.

- **Greeting format.** The Citadel greeting must be `200 <node> Citadel server
  ready.` The pipe-delimited variant QuackCit used first was rejected outright by
  the official client. Node/version details go in `INFO` instead.
- **`qr_flags` follow Citadel's canonical bitmask** (personal rooms `16390`), so
  the numbers on the wire in `LKRA`/`GOTO`/`GETR` are byte-compatible.
- **Default rooms/floors match a stock install**: public Lobby/Aide/Global
  Address Book/Trashcan plus per-user Mail/Sent Items/Drafts/Trash/Calendar/
  Contacts/Notes/Tasks with the right `default_view`.
- **IMAP mailbox naming**: `INBOX`, `INBOX/<room>` for personal rooms,
  `<Floor>/<Room>` for public ones — byte-identical mailbox set to the oracle.
- **Branding**: mirror Citadel's wire strings exactly, substituting `QuackCit`
  only where Citadel names itself (greetings, `IMPLEMENTATION`). Anything a
  client parses stays identical.
- **Ports are config, not identity.** The module defines the service; dev ports
  are non-privileged. Nothing is exposed to the internet; TLS is dev self-signed.
- **`MESG hello`** is the official client's login-banner handshake verb. Not
  handling it produced a cosmetic "Unrecognized or unsupported command." nit.
- **Site policy is tables, not config strings.** Domains, aliases, ACLs, DNSBL
  zones, DKIM keys and quotas live in `quackmail_*` tables read by both SMTP
  extensions — the same "tables are the bus" rule as presence. Everything ships
  empty: a fresh install accepts mail only for `c_fqdn`, queries no blocklist
  and signs nothing, so upgrading changes no behaviour until an admin acts.
- **Report every authentication result, reject on few.** SPF/DKIM/DMARC/DNSBL
  verdicts are always stamped into `Authentication-Results:`/`Received-SPF:`,
  but only the *sender's own* published `p=reject` and an explicit DNSBL listing
  cause a rejection. SPF hard-fail alone does not: forwarded mail and mailing
  lists fail SPF routinely, and rejecting on it loses real mail. Each rule has a
  `citadel_config` override (`qm_spf_reject`, `qm_dkim_reject`, …).
- **DKIM verification takes an injectable key lookup.** `policy::DkimKeyLookup`
  resolves locally stored public keys before falling back to DNS, which is what
  lets the whole sign→verify path be tested with no resolver at all.
- **DMARC's organizational domain comes from a bundled Public Suffix List.**
  `core/src/psl_data.cpp` is generated by `tools/gen_psl.py` and committed,
  because the build has no network step and must keep none. Both the ICANN and
  private sections are bundled (`kPslIcannRules` marks the split), so a
  provider-delegated name is its own organizational domain. This replaced a
  "last two labels plus a table of common suffixes" approximation that was
  documented as never-looser-than-a-PSL; that guarantee is gone, because an
  exact list is simply correct rather than conservative. `qm_psl_org_domain`
  exposes it as a scalar so `test/sql/psl.test` can assert the algorithm —
  including a wildcard rule and its exception — with no DNS in the loop.
- **Room permissions are derived, not stored.** RFC 4314 rights come from the
  room (`mailbox_owner`, `qr_flags`, the caller's axlevel, `RoomUnlocked`) and
  are then unioned with any `citadel_room_acl` row. A second authoritative ACL
  table would have made `qr_flags` and the ACL two sources of truth for the same
  question. The consequence is that an entry can only widen access — which is
  the right trade, because the one thing the table exists for is the grant
  Citadel's flags cannot express.
- **A room is opened to e-mail by an ACL grant, not a room flag.** Granting
  `anyone` the `p` right is exactly what RFC 4314 defines `p` to mean ("post —
  send mail to the submission address for this mailbox"), so the opt-in is one
  standard `SETACL` from any mail client instead of a QuackCit-only bit. It also
  closed a real parity gap: the oracle advertises `ACL` and we did not.
- **`citadel::CanPost` is the one post predicate.** The check
  (`authed && !QR_READONLY && RoomUnlocked`) had been reimplemented in the web
  front-end, NNTP and telnet, and was missing entirely from the native `ENT0`
  verb — a room's read-only flag simply did not apply to a Citadel client. All
  four now go through it, which also means an aide can post to a read-only
  announcement room, as a real Citadel server allows.
- **Listener starts are not posted to the Aide room.** New users, aide actions
  and (optionally) refused mail are, but a message per listener would have
  broken the unqualified `citadel_messages` counts in three integration tests,
  and `deploy/quackcit.sh` already logs listener startup and skipped
  implicit-TLS listeners to syslog.
- **LMTP is the deliberate hole in the fence.** It performs no authentication
  and no spam filtering, by design, because it is the trusted local-injection
  path. It binds to loopback unless explicitly overridden, and the README says
  plainly not to expose it.
- **DuckDB allows one read-write process per database file**, so the admin CLI
  cannot open the database while the server holds it. `quackcitadm.sh` sends SQL
  over the server's mode-0600 control FIFO when the server is up and opens the
  file directly when it is down. That channel accepts arbitrary SQL — treat it
  as root-equivalent.
- **The deploy scripts dropped Python for the binary release (0.4.x).** The
  release tarball ships a statically linked DuckDB CLI, but `run_quackcit.py`
  needed the *DuckDB Python package* pinned to the same version — a second
  install the bundle exists to avoid. So the server became the CLI itself,
  reading its standard input from a FIFO opened `0<>`: the process is a writer
  on its own input, the read never sees EOF, and that idle read replaces the
  launcher's `while not stop: sleep(1)`. The admin channel followed: a request
  is SQL bracketed by `.output` redirections, the reply is the file written, and
  a `mkdir` lock serialises clients. Error text was the only casualty of losing
  the JSON socket — `.output` captures stdout only — so the server's stderr gets
  its own file and the bytes appended across one request are that request's
  error. That is exact only because nothing in the C++ tree writes to stderr.
- **The scripts detect their install layout** rather than being configured for
  it: a checkout keeps repo-relative paths and the `<n>/<n>.duckdb_extension`
  tree, an unpacked bundle uses the flat directory and FHS state. One
  `ext_path` helper spans both, which is what let the same scripts ship in the
  tarball unchanged.
- **DuckDB table functions require constant-foldable arguments**, so
  `qm_dkim_verify(qm_dkim_sign(...))` is only expressible if both are *scalar*
  functions. The composable forms are scalar; `qm_dkim_verify_detail` remains a
  table function for the per-signature breakdown.
- **The two dead flag columns were the right homes for the new per-user state.**
  `citadel_room_state.flags` and `citadel_users.flags` shipped in the original
  schema and were never read or written. They now carry `RS_ZAPPED`/`RS_UNLOCKED`
  and Citadel's canonical `US_*` bits respectively, so the numbers stay
  byte-compatible with a real server exactly as `qr_flags` already did — and the
  telnet shell and the web console read and write the same column, which is why
  expert mode set over telnet shows up in the browser.
- **The HTTP codec lives in `core/`, the pages in `http/`.** Core is where wire
  formats go (`telnet.cpp` and `xmlstream.cpp` each have exactly one consumer
  too), and — the deciding reason — only core code can be exposed as umbrella
  SQL functions, which is what makes percent-decoding, HTML escaping and path
  normalization testable in sqllogictest with no socket in the loop.
- **The web front-end keeps connections alive, and caps them.** *(Reversed. It
  used to close every connection, on the reasoning that one inlined stylesheet
  meant a page was exactly one request, so persistence bought nothing while
  multiplying idle threads "in a server with no connection cap". Both halves of
  that changed at once.)* A page is several requests now that `/static` serves
  cacheable assets, so reuse is worth real latency. It is safe because the thing
  the old note named as missing is no longer missing: `ServerController` has a
  connection cap (`qm_http_max_connections`, default 256; 0 means unlimited, so
  no other protocol is affected), and persistence is bounded three more ways —
  100 requests per connection, a 5 s idle deadline between them, and a 60 s
  ceiling on the connection as a whole. The **first** request still gets the full
  15 s header budget, which is what the slow-loris defence rests on; a later one
  gets the idle budget and reports silence as `Eof` rather than 408.
  **The invariant that makes reuse safe: any non-`Ok` read closes the
  connection.** 413 and 411 answer *without* consuming the body the peer
  announced, so those bytes are still on the wire — reusing the socket would let
  them be parsed as the next request line. That is the same request smuggling
  this codec refuses chunked encoding to prevent, and it has a regression test
  (an oversized `Content-Length` with a second request pipelined behind it).
- **Assets are content-hashed, compiled in, and generated.** `/static/qc.<hash>.css`
  is served `immutable` for a year with no revalidation, which is only sound
  because the URL contains the hash of the bytes: a changed file is a changed
  URL, so nothing cached can be stale and there is no cache to bust. The bytes
  live in `http/src/web_assets.cpp`, generated by `tools/gen_assets.py` and
  committed on exactly the terms `core/src/psl_data.cpp` uses — the build fetches
  and generates nothing, and CI runs `--check` so a stale generated file fails
  the build instead of serving a 404 stylesheet. There is still **no CDN**;
  anything third-party would have to be vendored, and its CVEs adopted with it.
- **The stylesheet splits rather than moving.** *(Amends "one stylesheet,
  inlined; there are no external assets at all".)* What stays inline is what a
  page cannot be read without — the ten custom properties every external rule
  reads, the reset, the layout skeleton. That kills the flash of unstyled
  content and keeps the page legible if `/static` is unreachable behind a
  misconfigured proxy. It is also forced: the per-user theme is a `:root`
  override, which cannot be a cacheable asset and has to arrive with the page.
  For the same reason the theme `<style nonce>` must be emitted *after* the
  `<link>`, or it loses the cascade.
- **CSP gained `'self'` for scripts and styles, and the message frame did not.**
  A nonce and a host source coexist in CSP3 (only `'strict-dynamic'` suppresses
  host sources), so the page policy is additive: `'self'` covers `/static`, the
  nonce still covers the inline critical CSS. The CSP for a *message body* is
  passed to `SecurityHeaders` explicitly and must never gain `'self'` — that
  frame renders markup written by whoever sent the mail, and `'self'` would let
  it pull our scripts. Asserted in `test_http.py`.
- **Web sessions are hashed at rest and pinned to their transport.** The cookie
  holds 32 random bytes; the database holds only the SHA-256, so nothing stored
  can be replayed. A session minted over TLS presented in the clear is revoked
  (it should have been impossible), and one minted in the clear is refused over
  TLS — which stops a MITM on port 80 laundering a stolen cookie into the
  secure area. CSRF is a synchronizer token derived from a server secret in
  `citadel_config`, so it needs no extra column and is reproducible on every
  request; the anonymous variant for the login form is bound to the client
  address and a time bucket instead.
- **The admin console is root-equivalent and ships off.** It writes
  `citadel_config`, mints credentials and generates DKIM keys — the same threat
  model `quackcit.conf` already states for the 0600 control FIFO, but reachable
  from a browser. Hence `qm_web_admin_enabled=0` by default, TLS required, a
  `webadmin` ACL scope for a network allow-list, and a re-authentication prompt
  on the sharpest actions.
- **HTML mail renders in a sandboxed iframe, not inline.** The part is served
  from its own route with `default-src 'none'; img-src data:` and framed with
  `sandbox` (no `allow-scripts`, no `allow-same-origin`), so it gets an opaque
  origin. The sanitizer is defence in depth, not the boundary. Remote images
  stay blocked until the reader asks, so tracking pixels do not fire by default.
- **Decode, then escape.** Headers go through `mime::DecodeEncodedWords` first
  and `EscapeHtml` second. The other order lets `=?utf-8?B?PHNjcmlwdD4=?=`
  decode back into a live tag *after* escaping has run — and message headers
  come from inbound SMTP, i.e. from anyone.
- **Room listings are ordered by the internal key, not by listorder.** The real
  server's `cmd_lkra` just walks the room database, so rooms arrive in key
  order; personal rooms are keyed `0000000002.Calendar`, and digits sort ahead
  of letters, which is why every mailbox precedes the public rooms. Confirmed
  against the oracle for both a plain user and an aide. `listorder` is stored
  and reported but deliberately not used for sorting — the real server ignores
  it when listing, and clients group by floor themselves.
- **Toggle wording comes from the text client, not from taste.** `<X>` prints
  `Expert mode now ON/OFF` and `;C` prints `Floor mode now ON/OFF`, both from
  `textclient/user_functions.c:861-876`; `<Q>` follows `client_chat.c:204`
  (`Quiet mode enabled (no other users may page you)`). The telnet test asserts
  the exact strings, so drift is caught.
- **Rooms are addressed by number in web URLs.** A Citadel room name may contain
  a `/`, which no amount of percent-encoding survives once the path is split
  into segments. Numbers also make the ownership check a single explicit
  function (`ResolveRoomNumFor`), which is what stands between a guessed room
  number and someone else's mailbox.
- **Seeding is a deployment default, not a parity fixture.** `admin/admin` and
  `leo/leo` were seeded so protocol output could be diffed against the oracle
  1:1. That is a development convenience being shipped as two known passwords,
  so the default is now one `admin` with a generated password written to
  `$QUACKCIT_SEED_SECRET_FILE` (mode 0600), and the whole list is
  `QUACKCIT_SEED_USERS`. The parity pair is one variable away when it is needed.
- **Seeding happens before the server starts, not after.** It used to run from
  `cmd_start` over the control channel, which meant `cmd_foreground` — the
  systemd path, which `exec`s into the CLI and never returns — never seeded at
  all. It now runs from `prepare_runtime` through `adm_offline`: loading the
  umbrella runs `EnsureSchema`, and `qm_user_add` and `qm_config_set` both live
  there, so nothing needs a listener to be up. One SQL batch for all accounts,
  because `adm_offline` spawns a DuckDB process per call.
- **`c_fqdn`'s `quackmail.test` counts as unset.** The extension seeds that
  placeholder with `INSERT OR IGNORE`, so a deploy-side "set it if it is
  missing" would never fire. `seed_fqdn` treats empty *and* the placeholder as
  unconfigured and writes `hostname -f`; anything else is the operator's and is
  left alone. `QUACKCIT_FQDN` overrides on every start. This also stopped the
  certificate (issued to the real hostname) and `c_fqdn` from disagreeing.
- **The web origin check is opt-in.** It used to fall back to `c_fqdn` when
  `qm_web_origins` was empty — and since `c_fqdn` was always the seeded
  placeholder, the "no allow-list, allow everything" escape hatch was dead code
  and every POST from a real hostname or LAN address 403'd with *"This form was
  submitted from another site"*. Empty now means allow; the CSRF synchronizer
  token is the real defence and always was. Bracketed IPv6 origins
  (`http://[::1]:8443`) were also being truncated to `[` by the port split.
- **`quackcit.log` is RFC 5424; `quackcit.err` is not a log.** Everything the
  server prints goes through `syslog_stream` over a second FIFO, which keeps
  `$!` on the DuckDB process so the PID file and control channel are unaffected.
  Startup SQL switched to `.mode tabs`/`.headers off` so one row is one line —
  duckbox wraps a single row in several lines of box-drawing characters. The
  error log stays byte-exact because `adm_online` recovers a request's error
  text as the bytes appended to it while that request ran; a syslog header in
  front of those bytes would end up in front of every error the admin CLI
  prints. Neither file rotates, and `errlog_delta`'s absolute offsets are why
  adding rotation is not free.

## The test box: debian.lan

- SSH: `ssh -i ~/.ssh/quackcit_dev leo@debian.lan` (config alias `quackcit`).
  `leo` has working sudo (an old `NOPASSED` sudoers typo was fixed on
  2026-07-24 — earlier notes saying "no sudo" are stale).
- It runs a **real Citadel Groupware server** — the parity oracle — on the
  standard ports: citadel 504, smtp 25/465/587, pop3 110/995, imap 143/993,
  **nntp 119/563**, **xmpp 5222**, http 80/443. There is **no telnet listener**;
  the BBS UX is the text client at `/usr/local/citadel/citadel` over 504, whose
  103-command menu table lives in `/usr/local/citadel/citadel.rc`.
- The box is **disposable — no valuable data**. Restarting Citadel and creating
  rooms/users/floors on it is fine, and necessary for testing admin features.
- **The full Citadel source is at `/root/citadel`** (sudo to read):
  `citadel/server/modules/{pop3,nntp,xmpp,imap,smtp,...}/`, `citadel/server/
  msgbase.c` (message rendering), `textclient/` (the BBS client), `libcitadel/`,
  `webcit-ng/`. This is the authoritative spec — read it instead of guessing.
- Toolchain is user-local: `~/venv` with cmake/ninja/duckdb (pip bootstrapped via
  get-pip.py; Debian strips ensurepip). Build with `-j2` — 3.8G RAM.
- QuackCit source lives in `~/quackmail`; run it with `deploy/quackcit.sh start`
  and `QUACKCIT_DB=~/quackcit.duckdb`. The seeded users used to be `admin/admin`
  and `leo/leo`, mirroring the oracle for 1:1 diffing; that is no longer the
  default (see below). To reproduce it for a parity session, set
  `QUACKCIT_SEED_USERS='admin admin 6
  leo leo 4'` before the first start.
- Parity probes: `~/parity/cit_probe.py` (native), `~/parity/imap_probe.py`,
  `~/parity/drive_client.py` (drives the real text client). Captured oracle
  fixtures are committed under `test/parity/real_citadel/`.
- **Reaching QuackCit with the official client**: the client hardcodes port 504,
  which the oracle owns. `test/parity/port_hook.c` is a tiny `LD_PRELOAD` shim
  rewriting `socket()`/`connect()` to `127.0.0.1:5040`:
  `LD_PRELOAD=port_hook.so citadel -h localhost`. No root, no iptables, oracle
  untouched.

## Oracle protocol facts (probed 2026-07-24)

POP3 — greeting `+OK Citadel POP3 server ready.`; CAPA is exactly
`TOP`/`USER`/`UIDL`/`IMPLEMENTATION ...` on both 110 and 995 (STLS works but is
never advertised); `LAST` supported, `APOP` not; unknown verb →
`-ERR I'm afraid I can't do that.`

NNTP — greeting `200 <node> NNTP Citadel server is not finished yet`;
capabilities `VERSION 2 / READER / MODE-READER / LIST ACTIVE NEWSGROUPS / OVER /
STARTTLS / AUTHINFO USER`; **POST is not implemented** (`500`); overview format
is Subject/From/Date/Message-ID/References/Bytes/Lines.

Newsgroup naming (`serv_nntp.c`): a room name passes through unchanged if it
does not start with `ctdl.`, contains only `[alnum . + -]`, and has at least one
letter **and** at least one dot; otherwise `ctdl.` + lowercased name with every
other byte escaped as `+HH`. So `Aide` → `ctdl.aide`, `Global Address Book` →
`ctdl.global+20address+20book`, `0000000001.Mail` unchanged,
`0000000001.Sent Items` → `ctdl.0000000001.sent+20items`.

XMPP — stream features are `starttls`, SASL `PLAIN`, `jabber:iq:auth`, `bind`,
`session`; roster is presence-derived (empty when nobody else is online);
`disco#info` replies with the (malformed) namespace `...disco#info:query`;
`vcard-temp` returns `<fn>`/`<nickname>`; unknown namespaces get a `503
service-unavailable`.

RFC822 rendering of native messages (`msgbase.c`): headers in the order
Return-Path, Date, Subject, To, References, then `Message-ID: <HEXTIME-msgnum@node>`
and `From: "<author>" <sanitized_author@node>` (non-alphanumerics in the local
part become `_`), a blank line, then the body.

## Known quirks

- `qm_version()` reported `0.1.0` through the v0.2.1 release — the constant was
  never bumped. Fixed for 0.3.x.
- The `v0.3.0` tag landed one commit before its test-fixture fix, so the release
  workflow failed and **the published v0.3.0 release has no assets**. Re-cut as
  v0.3.1 (PR #9).
- Releases are built by `.github/workflows/release.yml` on `v*` tags; its
  packaging step has a **hardcoded extension list** that must be updated whenever
  a module is added.
- **The version string lives in three places** and they must move together, or
  `qm_version()` drifts the way it did before 0.3.x: the constant in
  `quackmail/src/quackmail_extension.cpp`, the seeded `c_version` in
  `core/src/citadel_store.cpp`, and the assertion in `test/sql/quackmail.test`.
  Note the seed is `INSERT OR IGNORE`, so an existing database keeps whatever
  `c_version` it was created with — only fresh installs pick the new one up.
