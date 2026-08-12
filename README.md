# QuackCit

A **[Citadel](https://www.citadel.org/) groupware/BBS server that runs *inside*
DuckDB**. Each protocol front-end is a separate DuckDB extension you load into a
running DuckDB; the whole message store — users, floors, rooms, and messages —
is a set of SQL tables.

Like real Citadel, QuackCit speaks its own native client/server protocol
(`protocol.txt`, TCP 504) *and* the standard mail protocols (SMTP, IMAP, POP3)
over the same store. A message that arrives by SMTP shows up in the recipient's
Mail **room**, readable both from a Citadel client and over POP3/IMAP.

> **This is vibe-coded software.** Nearly all of it was written by an LLM,
> against a real Citadel server used as an oracle for protocol behaviour. It is
> an experiment in how far that approach goes on a large, specification-heavy
> codebase — not a product, and not something anyone has audited. There has been
> no security review, no interoperability testing beyond the suites in this
> repository, and no promise that the on-disk schema is stable between commits.
>
> It speaks SMTP well enough to accept mail from the internet, which is exactly
> what makes that worth saying out loud: **do not put it in front of mail you
> would mind losing.** Several defaults are chosen so a fresh install is
> reachable rather than locked down — a self-signed certificate, an unrestricted
> web form origin, plaintext HTTP — and each one is called out in
> [Security posture](#security-posture) with the setting that tightens it.

```sql
LOAD quackmail;            -- umbrella: schema, users, admin, MIME helpers
LOAD quackmail_citadel;    -- the native Citadel protocol
CALL qm_user_add('alice', 'secret');
CALL cit_start('0.0.0.0', 5040);        -- background Citadel listener
-- ... clients connect, post to rooms ...
SELECT display_name, highest_msg FROM citadel_rooms;
SELECT author, subject FROM citadel_messages;
CALL cit_stop();
```

## Design

The server *is* DuckDB with extensions loaded — not a separate daemon that
happens to use DuckDB for storage. When you call a `*_start` function, the
extension spawns a background listener thread in the DuckDB process; each
connection runs a protocol session against an internal DuckDB connection.

Each protocol is its own loadable `.duckdb_extension` (its own shared object).
They do **not** share C++ state at runtime — they coordinate through the
**shared DuckDB database**: the tables are the bus. Common C++ plumbing (schema,
socket/TLS server scaffolding, the Citadel room/message store, MIME parsing,
SASL auth, a Sieve evaluator) lives in `core/` and is linked into every
extension.

| Extension | Functions | Role |
|---|---|---|
| `quackmail` (umbrella) | `qm_version`, `qm_status`, `qm_user_add/remove`, `cit_room_add`, `cit_room_kill`, `cit_floor_add`, `qm_mime_*`, `qm_parse_date`, and the site-policy admin functions (`qm_domain_*`, `qm_alias_*`, `qm_acl_*`, `qm_rbl_*`, `qm_dkim_*`, `qm_ratelimit_*`, `qm_config_*`) | schema init, users, room/floor admin, MIME helpers, policy administration |
| `quackmail_citadel` | `cit_start/_stop/_status` | ✅ native Citadel protocol (TCP 504; dev default 5040) |
| `quackmail_smtp_in` | `qm_smtp_in_start/_stop/_status`, `qm_lmtp_*` | ✅ inbound MX with SPF/DKIM/DMARC/DNSBL, hosted domains, aliases and allow/block rules — plus an LMTP local-injection listener (24; dev 2033) |
| `quackmail_smtp_out` | `qm_smtp_submission_start/_stop/_status`, `qm_smtp_smtps_*`, `qm_smtp_relay_*` | ✅ authenticated submission (587/465; dev 2587/2465) with DKIM signing and per-user rate limiting, plus the outbound queue drainer |
| `quackmail_pop3` | `qm_pop3_start/_stop/_status`, `qm_pop3s_*` | ✅ POP3 gateway (STLS + implicit TLS) → serves each user's Mail room |
| `quackmail_imap` | `qm_imap_start/_stop/_status`, `qm_imaps_*` | ✅ IMAP4rev1 gateway (STARTTLS + implicit TLS) → mailboxes = rooms, with `NAMESPACE`, `UIDPLUS`, `MOVE`, RFC 4314 `ACL` and `IDLE` |
| `quackmail_managesieve` | `qm_managesieve_start/_stop/_status` | ✅ ManageSieve (RFC 5804, port 4190) — install the Sieve filters the delivery path applies |
| `quackmail_nntp` | `qm_nntp_start/_stop/_status`, `qm_nntps_*` | ✅ NNTP reader **and poster** (119/563; dev 1119/1563) — rooms are newsgroups |
| `quackmail_xmpp` | `qm_xmpp_start/_stop/_status`, `qm_xmpps_*` | ✅ XMPP c2s (5222/5223; dev 15222/15223) — instant messages bridged to Citadel's |
| `quackmail_telnet` | `qm_telnet_start/_stop/_status`, `qm_telnets_*` | ✅ BBS shell over telnet (23; dev 2300) and telnets (992; dev 2992) — the Citadel text-client experience, server-side |
| `quackmail_http` | `qm_http_start/_stop/_status`, `qm_https_*` | ✅ webmail, the BBS, groupware (contacts, calendar, tasks, notes, blog) and the admin console (80/443; dev 8080/8443) — server-rendered, no JavaScript framework — plus **CalDAV and CardDAV** at `/dav/` and **JMAP** (RFC 8620 + 8621) at `/jmap/`, so a phone syncs the same rooms |
| `quackmail_spool` | `qm_listserv_start/_stop/_status`, `qm_listserv_run`, `qm_fetch_*` | ✅ periodic background work — the only module with no listener. Distributes mailing lists (fan-out, digests, moderation) and pulls messages in from remote POP3/IMAP mailboxes and RSS/Atom feeds |

`*_start(host, port)` also accepts named params: `tls_cert`, `tls_key`,
`implicit_tls`, `starttls`. With `starttls => true` and no cert paths, a
throwaway self-signed certificate is generated in memory — enough to exercise an
upgrade by hand, but it differs per listener and is gone on restart, so a
*server* wants a certificate on disk. `deploy/quackcit.sh` writes one on first
start if you have none (see [Running and administering a server](#running-and-administering-a-server)).
All control functions return a status row:
`(action, running, host, port, connections, note)`.

> **Port 504** is the IANA-assigned Citadel port, but it is privileged on Linux,
> so `cit_start` defaults to **5040** for development (just as `qm_smtp_in`
> defaults to 2525 for 25). Pass the port explicitly to use 504.

## Data model

The store is Citadel-shaped: **users → floors → rooms → messages**, with
per-user last-read pointers and a reference-count message model (one message can
be pointed into several rooms). Created idempotently on load by
`EnsureSchema` / `EnsureCitadelSchema` (`core/src/citadel_store.cpp`).

| Table | Purpose |
|---|---|
| `citadel_users` | Citadel user metadata: `usernum`, `axlevel`, post/call counters (credentials live in `quackmail_users`). |
| `citadel_floors` | Floors — named containers grouping rooms. Seeded with floor 0, "Main Floor". |
| `citadel_rooms` | Rooms: `room_num`, `name`/`display_name`, `floor_num`, `qr_flags` (public/private/passworded/mailbox), `mailbox_owner`, `highest_msg`. Seeded with Lobby (0) and Aide (1); user rooms start at 100; personal mailbox rooms are `<usernum>.<name>`. |
| `citadel_messages` | Messages: `msgnum`, `author`, `recipient`, `msgtime`, `subject`, `format_type` (0 = Citadel, 4 = RFC822/MIME), `raw` canonical bytes. |
| `citadel_room_msgs` | `(room_num, msgnum)` message pointers — the ref-count model. |
| `citadel_room_state` | Per-user `last_read` pointer + room flags. |
| `citadel_msg_flags` | IMAP flags (`\Seen`, `\Deleted`, …) per `(msgnum, username)`. |
| `citadel_room_acl` | RFC 4314 access control entries per `(room_num, identifier)`. Only explicit grants; ordinary rights are derived from the room. |
| `citadel_user_prefs` | String-valued per-user preferences (the web colour theme), alongside the `US_*` bit field in `citadel_users.flags`. |
| `citadel_config` | `INFO`/config key-value (nodename, fqdn, humannode, …). |

Also present: `quackmail_users` (credentials), `quackmail_sieve_scripts`,
`quackmail_outbound` (relay queue), `citadel_user_reg` (the `REGI` registration
record plus the biography), and `quackmail_web_sessions` /
`quackmail_web_login_fails` (browser sessions for the web front-end — only the
SHA-256 of each token is stored).

### Site policy

Mail policy is a second set of tables, created alongside the rest and read by
both SMTP front-ends. All of it is empty by default: a fresh install accepts
mail only for `c_fqdn`, queries no blocklist, and signs nothing.

| Table | Purpose |
|---|---|
| `quackmail_domains` | Domains we accept mail for beyond `c_fqdn`: `kind` is `local` (deliver here) or `relay`, plus the default `dkim_selector`. |
| `quackmail_aliases` | `alias` → `destination`. Several rows for one alias fan out to several users; `@example.com` is that domain's catch-all. A destination that is not a local user is **forwarded** onto the outbound queue — not open relay, since the mail is addressed to a domain we host and an admin configured that target explicitly. Chains are followed, with a depth cap so a cycle cannot hang `RCPT`. |
| `quackmail_acl` | Allow/block rules over `scope` ∈ {`ip`, `sender`, `domain`, `rcpt`, `helo`}. Patterns are globs; `ip` also takes CIDR. **Allow always beats block**, so a narrow allow carves an exception out of a broad block. |
| `quackmail_rbl_zones` | DNSBL zones to query, in order. Empty by default — blocklist checking is opt-in. |
| `quackmail_dkim_keys` | Outbound signing keys. The private half is stored here, so the database file's permissions are the security boundary for it. |
| `quackmail_rate_limits` | Per-user send quotas; the row with an empty username is the default (100 per 300 s, 500 per 24 h). |
| `quackmail_send_log` | The sliding window the limiter counts over. |
| `quackmail_inbound_log` | What the inbound checks decided, per recipient: SPF/DKIM/DMARC/DNSBL verdicts and the disposition. |

## Mailing lists

A mailing list **is a room**. `citadel_lists` marks a room as one and gives it a
posting address; `citadel_list_subs` holds the subscribers. This is Citadel's
own model — `listrecp` / `digestrecp` entries on a room — in tables.

| Table | Purpose |
|---|---|
| `citadel_lists` | One row per list room: the posting address, `mode` (`post`/`digest`/`both`), `post_policy` (`anyone`/`subscribers`/`moderated`), subject tag, footer, digest settings, and the `last_sent`/`last_digest` watermarks. |
| `citadel_list_subs` | `(room_num, address)`, with `kind` (`post`/`digest`) and `state` (`pending`/`active`/`unsub_pending`). A pending row carries a confirmation token and an expiry. |
| `citadel_list_held` | Posts parked for an aide, with the raw message. Approving posts it into the room; the spooler distributes it from there. |

Distribution is driven from the **room**, not from the SMTP handler: the
`qm_listserv` spooler walks each list room for messages past its watermark and
queues a copy per subscriber. That is what makes a post from the BBS, NNTP,
webmail or a native Citadel client reach subscribers too — mail is only one of
the ways a message gets into a room. Sending itself goes onto
`quackmail_outbound`, so retries and backoff stay in one place.

Every copy carries the RFC 2369/2919 `List-*` headers and a
`<list>-bounces@` envelope sender, and any inbound `DKIM-Signature`,
`Authentication-Results`, `Return-Path` or `List-*` header is stripped — the
subject tag and footer change bytes a signature covers, and a sender-supplied
`List-Unsubscribe` would point subscribers' unsubscribe button wherever they
liked.

Self-service works by mail at `<list>-subscribe@`, `<list>-unsubscribe@` and
`<list>-request@` (help), and over the web at `/lists`. Neither takes effect on
its own: both mail a confirmation containing a token to the address named, and
only `<list>-confirm-<token>@` — or the link in that mail — completes the
change. Without that, either interface would be a way to sign anybody up for
anything.

Administered with `quackcitadm.sh list` or at `/admin/lists`.

## Pulling messages in

The other direction: poll a POP3 or IMAP mailbox on someone else's server, or an
RSS/Atom feed, and post what is new into a room. A newsgroup, a webmail account
and a blog then all read through the same BBS, mail client or newsreader as
everything else.

| Table | Purpose |
|---|---|
| `quackmail_feeds` | What to poll, where to put it, and how to resume: source, credentials, target room (or target user, which routes through their Sieve script), interval, per-run cap, and the `uidvalidity`/`last_uid`/`etag`/`last_modified` resume state plus the last run's status. |
| `quackmail_feed_seen` | Identifiers already posted — POP3 `UIDL`s, IMAP `<uidvalidity>.<uid>`, RSS guids. This is what makes a poll idempotent; it is pruned to the newest few thousand per feed. |

The clients are new core code, since the tree only spoke these protocols as a
server: `core/mail_client.cpp` (POP3 `UIDL`/`RETR`/`DELE`, IMAP `UID SEARCH` /
`UID FETCH BODY.PEEK[]`) and `core/http_client.cpp` + `core/feed.cpp`. The HTTP
client is separate from `core/http.cpp` because that one deliberately refuses
chunked transfer encoding — sound for a server, fatal for a client, since feed
servers chunk. RSS/Atom parsing runs over the XML tokenizer already in core for
XMPP, so nothing new is linked in.

Messages are left on the server by default: pulling from a mailbox must not
empty it by accident. Credentials are stored in the clear, as
`quackmail_dkim_keys` stores private keys — the database file's permissions are
the boundary — and a stored password is never rendered back into the admin page.

Polling runs on the `qm_fetch` worker. Administered with `quackcitadm.sh feed` or
at `/admin/feeds`.

## Mail authentication

### Inbound

`quackmail_smtp_in` runs each check at the protocol stage where it belongs: the
IP rules at connect, the HELO rule at EHLO, SPF at `MAIL FROM`, the recipient
rules and the DNSBL at `RCPT TO` (deferred so an allow-listed recipient such as
postmaster stays reachable from a listed address), and DKIM + DMARC over the
message bytes exactly as received. Every accepted message is then stamped with
`Received:`, `Authentication-Results:` and `Received-SPF:` — prepended *after*
verification, so they cannot disturb a signature.

The shipped posture is to report everything and reject little:

| Config key | Default | Effect |
|---|---|---|
| `qm_dmarc_enforce` | `1` | honour the sender's own published policy: `p=reject` → 550, `p=quarantine` → filed into `qm_quarantine_room` |
| `qm_rbl_reject` | `1` | refuse a client listed by a configured DNSBL (none are configured by default) |
| `qm_spf_reject` | `0` | SPF hard-fail alone does not reject — forwarded mail fails SPF routinely |
| `qm_dkim_reject` | `0` | a broken signature alone does not reject; DMARC decides what it means |
| `qm_quarantine_room` | `Junk` | where quarantined mail is filed |

Change any of them with `quackcitadm.sh config set qm_spf_reject 1`.

### Mail filters

`/prefs/sieve` shows the active Sieve script as a list of rule cards — add,
delete, reorder — with the source below it. The rules are **derived from the
script text on every page load** and never stored beside it: ManageSieve and the
admin console write the same row, so a table of structured rules would either be
silently overwritten or start describing filtering that is not what runs.

When a script says something the rule view cannot express — a nested `if`, an
`else`, an `envelope` test, anything using `variables` — the page says so and
offers only the text editor. It never shows a partial decomposition, and it
refuses to rewrite such a script.

A rule can carry several conditions (matching all or any of them) and several
actions, and a rule with *no* conditions applies to every message — which is the
shape an out-of-office reply has. The actions offered are exactly the ones the
engine implements: file into a folder (optionally creating it), keep, forward,
refuse, discard, and reply once. Filing or keeping can also set IMAP flags, so a
rule can file something as already read and every mail client agrees.

`/admin/sieve` is the same builder pointed at any user's filters.

The engine is RFC 5228 plus `reject`, `envelope`, `body`, `copy`, `mailbox`
(`fileinto :create`), `imap4flags`, `variables` and `vacation`; `regex` is
deliberately absent, being an expired draft whose backtracking would run on the
delivery path. Auto-replies never answer a mailing list, an automated message, a
bounce, mail you were only copied on, or yourself, and answer any one
correspondent at most once per `:days` (7 by default). An operator can switch
them off site-wide with `qm_sieve_vacation`.

Only the actions the engine implements are offered (`fileinto`, `keep`,
`redirect`, `reject`, `discard`); there is no vacation, because there is no
vacation.

DMARC's organizational domain comes from a **bundled Public Suffix List**
(`core/src/psl_data.cpp`, regenerated by `tools/gen_psl.py` and committed — the
build never fetches anything). Both sections of the list are included, so a
provider-delegated name is its own organizational domain rather than aligning
with every other customer of that provider. `qm_psl_org_domain('mail.bbc.co.uk')`
exposes the lookup for inspection.

### Time zones

Calendars need to know what time an event is, so the **IANA time zone database
is bundled** the same way (`core/src/tzdata.cpp`, regenerated by
`tools/gen_tzdata.py` and committed). Deliberately not DuckDB's `icu` extension
— a calendar that only works when an optional extension happens to be installed
is not a calendar — and not `/usr/share/zoneinfo` at runtime, which minimal
containers routinely ship without and which would let two installs of the same
release disagree about when a meeting is.

Transitions before 1970 are dropped, and everything past the last stored
transition comes from the zone's POSIX TZ rule, so a date in 2050 is still
right rather than frozen at the last recorded offset. `qm_tz_version()` reports
the bundled release; refreshing it is a generator run plus a commit.

Recurring events expand in **wall-clock** terms in the event's own zone, which
is what keeps a weekly 09:00 meeting at 09:00 across a daylight-saving change
instead of drifting an hour.

Each user picks their own zone on `/prefs`; the site default is
`qm_default_tz` (UTC unless an operator changes it). Every date the web
interface renders goes through it, so two people reading the same room see the
same instant written in their own local time.

Note that an exact list is *not* uniformly stricter than the two-label
approximation this replaced: it is correct, which in places means more
permissive. It also decides which parent-domain DKIM key signs mail for a
subdomain (see `policy::DkimKeyFor`).

### Outbound

Submitted mail is signed with the key for its `From:` header domain (falling
back to the envelope sender's domain, then to the organizational domain, so one
key covers subdomains). Signing happens once at submission, so the locally
delivered copy and every queued copy carry the same signature.

```bash
deploy/quackcitadm.sh dkim keygen example.com mail
```

That generates a 2048-bit key, stores it, and prints the TXT record to publish
at `mail._domainkey.example.com`. Confirm it with
`dig +short TXT mail._domainkey.example.com`, then check a signed message with
`deploy/quackcitadm.sh dkim verify /path/to/message.eml`.

Rate limiting charges **one unit per envelope recipient**, so a message to 50
addresses spends 50. Over-quota submissions get `451` — transient, so a
well-behaved client retries rather than bouncing the mail.

```bash
deploy/quackcitadm.sh ratelimit set '' 100 300 500   # the default policy
deploy/quackcitadm.sh ratelimit set bulkmailer 500 300 5000
deploy/quackcitadm.sh ratelimit status alice
```

### LMTP

`qm_lmtp_start` opens an RFC 2033 listener for **trusted local injection**. It
greets with `LHLO`, emits one reply per recipient after `DATA`, and **performs
no sender authentication and no spam filtering at all** — no SPF, DKIM, DMARC,
DNSBL or access rules. Domain routing, alias expansion and the recipient's
Sieve filter still apply, because those are addressing rather than filtering.

Anything that can reach this socket can inject mail as anyone. It binds to
loopback regardless of `QUACKCIT_HOST` unless `QUACKCIT_HOST_LMTP` overrides it.
Do not expose it.

## Native Citadel protocol

`quackmail_citadel` implements a useful subset of the Citadel client/server
protocol (stateful, 3-digit result codes, pipe-delimited params, `000`-terminated
listings):

- **Session**: greeting, `NOOP`, `ECHO`, `IDEN`, `QUIT`, `LOUT`, `INFO`.
- **Auth**: `USER`/`PASS`, `NEWU` (create + log in), `SETP` (set password).
- **Users**: `LIST` (directory, honouring `US_UNLISTED`), `REGI`/`GREG`
  (registration), `EBIO`/`RBIO` (biography) — the same records the BBS shell
  and the web console read and write.
- **Floors**: `LFLR` (list), `CFLR` (create, aide).
- **Rooms**: `LKRA`/`LKRN`/`LKRO` (list all/new/old), `GOTO`, `CRE8`, `KILL`,
  `GETR`/`SETR`, `RINF`, `SLRP` (set last-read).
- **Messages**: `MSGS` (`all`/`new`/`old`/`last`/`first`/`gt`/`lt`), `MSG0`
  (field listing), `MSG2` (raw), `ENT0` (post).

Config-heavy admin verbs (`CONF`, `DOWN`, `SCDN`, `TERM`, `EXPI`), the Citadel
network mesh, and instant messaging are deferred (see Roadmap).

## The BBS shell (telnet)

A real Citadel install has no telnet listener: the BBS experience comes from the
`citadel` text client speaking the native protocol. `quackmail_telnet` *is* that
client, running server-side, so a plain `telnet` gets the BBS — registration and
login, the `<Room>>` prompt, and the menu from `citadel.rc`:

```
telnet localhost 2300
```
```
QuackCit BBS - The Cloud

Enter your name (or 'new' to register): alice
Password:

Lobby>  1 new of 1 messages
Room cmds:    <K>nown rooms, <G>oto next room, <.G>oto a specific room, ...
```

Rooms: `<K>`nown, `<G>`oto (marks the room read and moves on), `<S>`kip (moves
on and leaves it unread), `<A>`bandon, `<U>`ngoto, `<Z>`ap to forget a room,
`<+>`/`<->` next/previous room and `<>>`/`<<>` next/previous floor.
Messages: read `<N>`ew/`<O>`ld/`<F>`orward/`<R>`everse/`<L>`ast five, `<E>`nter,
`<D>`elete. General: `<W>`ho, `<P>`age, `<M>`ail, `<I>`nfo, `<Q>`uiet mode,
`<X>` expert mode, `<?>` help, `<T>`erminate.

`;` opens the floor commands (`;C`onfigure floor mode, `;G`oto, `;S`kip to,
`;Z`ap, `;K`nown, and `;A`dmin create/edit/kill for aides). `.` opens the rest
of the `citadel.rc` menu: `.K`nown with filters (`.KZ`apped, `.KD`irectory,
`.KP`rivate, `.KR`ead-only, `.KM`atch, `.KF`loors), `.R`ead (`user list`, `bio`,
`configuration`, `system info`), `.E`nter (`password`, `configuration`,
`registration`, `bio`, a new `room`), `.W`holist (long, stealth) and, for aides,
`.A`dmin (edit/kill room, info file, move a message, edit/delete/validate
users).

Preferences persist in `citadel_users.flags` using Citadel's own `US_*` bits, so
expert mode, floor mode and the paginator survive a disconnect — and the web
console's preferences page edits exactly the same column. The screen size comes
from the telnet NAWS negotiation, and listings pause at each screenful.

Sessions register in `citadel_sessions`, so telnet users and native Citadel
clients see each other in the who-list and can page one another.

## The web interface (HTTP/HTTPS)

`quackmail_http` serves three things from one listener pair — `qm_http`
(80; dev 8080) and `qm_https` (443; dev 8443, implicit TLS):

- **Webmail** at `/mail/` — the user's personal rooms as folders, a paged
  message list, a read pane with attachments, and compose/reply/reply-all/
  forward. Sending goes through the *same* path as SMTP submission: the send
  quota is checked first, the message is DKIM-signed once, local recipients are
  delivered and remote ones queued, and a copy is filed into Sent Items.

  A folder is a `VIEW_MAILBOX` room and renders as a mailbox rather than a
  message board: a checkbox per row, flag and attachment columns, size, and a
  bulk bar that files, flags, marks read or deletes **whatever is ticked**. It
  is one `<form>` with a `formaction` per button — plain HTML, so selecting
  three messages and deleting them works with JavaScript off. The only scripted
  part is the select-all checkbox, which stays hidden until `qc.js` marks the
  document rather than sitting there doing nothing.

  Read state in a folder is the IMAP `\Seen` flag, not the Citadel last-read
  pointer the message board uses: a pointer is a high-water mark and cannot say
  "this one, not that one". Opening a message sets it, and it is the same
  `citadel_msg_flags` row a desktop client reads, so the two agree.

  Composing offers **formatted text**, producing a `multipart/alternative` with
  both halves so a recipient on a text-only client, or reading over Citadel or
  POP3, still gets something readable. A pasted or dropped image becomes a real
  `cid:` part rather than a base64 blob inside the body. The editor is 200 lines
  of `contenteditable` with no dependency, and it is an enhancement: with
  JavaScript off the form is the plain textarea it has always been.

  Composed HTML is sanitized on the server against an **allow-list**, before the
  message is built — not on display. What is stored is already safe, because it
  will be re-served from this origin to the recipient, to everyone in a public
  room, and to every subscriber of a mailing list.
- **The BBS** at `/bbs/` — floors, rooms, reading, posting, replying, marking
  read, forgetting a room, the who-list and instant messages. A post made here
  is an ordinary `format_type = 0` Citadel message, so it reads back over the
  native protocol, telnet, NNTP, IMAP and POP3.
- **The sidebar** carries unread counts: every mail folder, and the rooms with
  something new in them, most unread first. It is one room listing plus one or
  two `RoomStatsBulk` calls — fewer queries than the four `FindUserRoom` lookups
  it used to make for the groupware links, which now come out of the same
  listing. `qm_web_sidebar_rooms` (default 10) caps the room half and 0 turns it
  and its query off; folder counts are a handful of personal rooms and always
  shown. A room page marks that room current, falling back to *All rooms* for
  one the sidebar does not list.
- **Reading is a flow**, not a round trip through the list: *Newer*, *Older* and
  *Next unread* sit above every message. Each is a bounded min/max over the
  `(room_num, msgnum)` key rather than a listing of the room, and *next unread*
  means what that room's own listing means — `\Seen` in a mail folder, the
  last-read pointer on a board.
- **Search** at `/search`, and from the box in the page header. Subject and
  sender are matched across every room the caller can read; message text is a
  second mode, because a body is not a column — a `format_type = 4` message
  keeps its text base64-encoded inside `raw`, so matching it means decoding each
  candidate through the same `citadel::BodyText` the other front-ends use. That
  path is bounded by `qm_web_search_scan` (default 2000 messages, newest first)
  and the page says when it hit the bound rather than passing off a partial
  answer as a complete one.

  The room set *is* the access control: search enumerates rooms through the same
  helper the room list uses, so a private room stays invisible, a forgotten room
  stays forgotten, another user's mailbox is never reachable, and a passworded
  room is not searched until that session has unlocked it. A `room=` parameter
  naming a room outside that set is not a scope — it is ignored, never a query
  against that room.
- **Room views.** A room's Citadel `default_view` decides how it renders:

  | View | What you get |
  |---|---|
  | `Contacts` | an address book; each e-mail address is a link that composes to it |
  | `Calendar` | a month grid, or an agenda for `CALBRIEF`; the day number creates an event |
  | `Tasks` | VTODO with due date, priority and progress, sorted as a work queue |
  | `Notes` | a card grid of `text/vnote` sticky notes, colours and all |
  | `Blog`, `Journal` | whole entries newest-first rather than a list of subjects |
  | anything else | the ordinary message list |

  `?view=raw` forces the message list for *any* room, which is what you want the
  first time a Contacts room turns out to hold something that is not a vCard.

  Contacts, calendar entries, tasks and notes are ordinary `format_type = 4`
  messages wrapping one `text/vcard`, `text/calendar` or `text/vnote` part and
  keyed by the object's own UID — so an object created in the browser is a
  readable MIME message over IMAP with no extra code, and saving it again
  replaces it rather than appending a copy. Blog and journal rooms hold plain
  messages, so an entry reads back over every other protocol as itself.

  Editing preserves what the forms do not show. A calendar event keeps the alarm
  someone set on their phone, a contact keeps a second address, a note keeps
  where another client placed it on a pinboard.
- **Room settings** at `/bbs/room/:n/settings` — not part of the admin console.
  The page is gated on the RFC 4314 `a` ("administer") right, so an aide hands
  somebody a room by granting it and that person manages the room without ever
  reaching `/admin`, which is root-equivalent. The grant can be made from that
  page, from `quackcitadm.sh room acl`, or from any mail client:

  ```
  a1 SETACL "Project X" alice lrswia                  # alice now runs that room
  ```

  ```sh
  quackcitadm.sh room rights "Project X" alice        # what alice may actually do
  ```

  A room's administrator sets its name, floor, view, description and flags;
  edits its access list; opens or closes its `room_<name>@` address; runs its
  mailing list's presentation and membership; polls a feed pointed at it; and
  deletes it. Two things stay an operator's, and the page says why: **creating**
  a feed (it stores a plaintext password and dials whatever host it names) and
  **making** a room into a mailing list (that mints an inbound address and a
  fan-out engine). Inviting a subscriber sends a confirmation and nothing else,
  so a room administrator cannot sign up an address nobody reads.
- **Creating a room** at `/bbs/new`, gated on `qm_room_create_axlevel` — the
  Citadel access level it takes, defaulting to aide (6) so nothing changes on an
  existing server until an operator lowers it. Whoever creates a room
  administers it. An invitation-only room is listed only for the people its
  access list names with `l`, which is what makes "private" mean invitation
  rather than merely name-guessing.
- **The admin console** at `/admin/` — users, rooms, floors and `citadel_config`,
  plus the whole mail-policy set (domains, aliases, ACLs, DNSBL zones, DKIM
  keys, send quotas), the inbound audit log, the outbound queue, any user's
  Sieve scripts, and the signed-in browsers.

Everything is **server-rendered HTML** — no framework, no build step, and no
CDN. There is a stylesheet and a small script at `/static`, both compiled into
the extension and served from memory: `tools/gen_assets.py` turns the files in
`http/assets/` into a committed C++ source file, so the build fetches and
generates nothing and there is no data directory to install. Their URLs carry a
content hash (`/static/qc.9f3a1c2b.css`), which is what lets them be cached
`immutable` for a year — a changed file is a changed URL, so there is never a
stale copy and never a cache to bust.

The palette, the reset and the layout skeleton stay **inlined in every page**,
so a page renders legibly on the first packet and stays legible if `/static` is
unreachable behind a proxy. The per-user colour theme has to be inline anyway:
it is a `:root` override, and a cacheable file cannot vary per user.

**Every page works with JavaScript disabled**, including the collapsing mobile
navigation (a hidden checkbox and a sibling selector). The script only adds a
confirmation step to the forms that ask for one.

Connections are persistent, since a page is now several requests — see
[Security posture](#security-posture) for the limits that bound them.

### Security posture

The web front-end is the only part of QuackCit a browser can reach, so it is
the part with the most deliberate hardening. Read that in the light of the
[disclaimer](#quackcit) at the top: none of it has been reviewed by anyone, and
the two items marked **relaxed by default** are the price of a fresh install
being reachable at all.

- **HTTPS is not forced on a fresh install** — *relaxed by default*. The plain
  listener can redirect everything to `https://<c_fqdn>`, built from the
  configured name and never from the client's `Host` header, because using the
  header there would be an open redirect with extra steps. But that redirect
  carries no port, so on the default dev ports (8080/8443) it sends browsers to
  a closed one; the first `quackcit.sh start` therefore writes
  `qm_web_force_https = 0` and the interface is served in the clear. Set it back
  to `1` once you have a certificate clients trust, or behind a reverse proxy
  listed in `qm_web_trusted_proxies` — `X-Forwarded-Proto` is honoured only from
  a peer on that list. Sessions are pinned to the transport they were minted
  over either way, so nothing is laundered between the two listeners.

  One thing does build a URL from the `Host` header, and the difference is
  worth stating rather than leaving as an apparent inconsistency: JMAP's
  Session resource (`apiUrl`, `uploadUrl`, `downloadUrl`). A client calls those
  verbatim instead of resolving them against the request the way it resolves a
  DAV href, so a path is not a URL there — and `c_fqdn` carries no port, which
  would describe every server not on 80/443 wrongly. A `Location` is followed
  by a browser and can be cached; these go back to the authenticated client
  that just sent the header, on the same connection, in a `no-store` response,
  for that client to call us on. The header is still validated to
  `host[:port]` and falls back to `c_fqdn` otherwise, and `qm_web_base_url`
  overrides the whole thing for a reverse proxy that rewrites `Host`.
- **Form origins are unrestricted until you restrict them** — *relaxed by
  default*. `qm_web_origins` is a comma-separated list of host globs allowed to
  submit forms; while it is empty, any origin may. This server is reached by
  whatever name its operator points at it — a LAN address, a container name, a
  tunnel — and the previous behaviour of falling back to `c_fqdn` meant every
  deployment whose `c_fqdn` was not exactly the browsed name rejected all of its
  own POSTs with *"This form was submitted from another site"*. The CSRF
  synchronizer token below is the actual cross-site defence; the origin check is
  a cheap second one, and setting `qm_web_origins` is what turns it on.
- **The admin console is off until you turn it on** (`qm_web_admin_enabled`),
  requires TLS (`qm_web_admin_require_tls`), honours the `webadmin` ACL scope
  for a network allow-list, and asks for the operator's own password again
  before creating accounts, resetting passwords, writing config or generating
  DKIM keys. Reaching it is equivalent to root on the host.
- **Sessions** are 32 random bytes in an `HttpOnly; SameSite=Lax` cookie;
  only the SHA-256 is stored, so the database never holds anything replayable.
  A session is **pinned to the transport it was minted over**, in both
  directions, so a cookie cannot be laundered between the plaintext and TLS
  listeners. Changing a password revokes every session for that account.
- **CSRF** uses a synchronizer token derived from a server secret, checked in
  the router rather than in each handler, so a new POST route cannot forget it.
- **Message bodies are attacker-controlled** — they arrive over SMTP. Text is
  escaped at every interpolation (decoded first, escaped second, so an RFC 2047
  encoded-word cannot decode back into a tag). A `text/html` part is sanitized
  and then served from its own route into a `sandbox`ed iframe under
  `default-src 'none'`, with remote images blocked until the reader asks for
  them. Attachments are always `application/octet-stream` with
  `Content-Disposition: attachment` and `nosniff`, never the sender's type.
- Every page carries a CSP with a per-response nonce **and** `'self'` (no
  `'unsafe-inline'` anywhere), `nosniff`, `X-Frame-Options: DENY`,
  `Referrer-Policy: no-referrer` and `Cache-Control: private, no-store`.
  `'self'` is what lets `/static` be used; the frame a **message body** is
  rendered into deliberately does not get it, since that markup came from
  whoever sent the mail.
- **Connections are persistent, and bounded.** Up to 100 requests per
  connection, a 5-second idle deadline between them, a 60-second ceiling on the
  connection, and at most `qm_http_max_connections` (default 256) open at once.
  The *first* request on a connection still gets the full 15-second header
  deadline, which is the slow-loris defence. Any malformed or oversized request
  closes the connection rather than answering and continuing: a 413 or 411 is
  sent without reading the body the client announced, so those bytes are still
  on the wire and reusing the socket would parse them as the next request.
- Rooms and messages are addressed by number and re-checked on every request:
  a room is resolved through the visibility rules, and a message number is
  verified to be pointed into that room before it is loaded.

Failed sign-ins are counted per client address and answered with a `429` once
they pile up; the wrong-user and wrong-password replies are identical, so the
form cannot be used to enumerate accounts.

## News (NNTP)

Every room a user can see is a newsgroup, using Citadel's own name mapping
(`Lobby` → `ctdl.lobby`, `Global Address Book` → `ctdl.global+20address+20book`,
`0000000002.Mail` unchanged), and the room's message pointers are the article
numbers. `LIST ACTIVE/NEWSGROUPS/OVERVIEW.FMT` (with wildmat patterns), `GROUP`,
`LISTGROUP`, `ARTICLE`/`HEAD`/`BODY`/`STAT`, `NEXT`/`LAST`, `OVER`/`XOVER`,
`NEWGROUPS`, `DATE`, `AUTHINFO`, and `STARTTLS` are implemented.

Unlike a stock Citadel server, which answers `POST` with
`500 I'm afraid I can't do that.`, QuackCit **accepts posted articles**: they are
stored as ordinary Citadel messages, so an article posted over NNTP is readable
from a Citadel client, the BBS shell, IMAP and POP3. (It also resolves
`<message-id>` fetches and reports real `:bytes`/`:lines` in `OVER`, both of
which Citadel punts on.)

## Instant messaging (XMPP)

`quackmail_xmpp` speaks client-to-server XMPP: STARTTLS, SASL `PLAIN` (and the
legacy `jabber:iq:auth`), resource binding, sessions, `jabber:iq:roster`,
presence, `vcard-temp`, `urn:xmpp:ping` and service discovery.

Like Citadel, there is no stored roster: **the roster and presence list are the
people currently logged in** — read from `citadel_sessions`, so XMPP clients,
telnet users and native Citadel clients all see one another. A `<message>` is
written to `citadel_express`, the same queue the native protocol's `SEXP`/`GEXP`
uses, and queued messages are pushed to connected XMPP clients as `<message>`
stanzas. Sending from XMPP and reading with `GEXP` from a Citadel client (or the
BBS shell) works in both directions.

## Mail gateways

The standard-protocol extensions are front-ends onto the same room store:

- **`quackmail_smtp_in`** authenticates the sender (SPF/DKIM/DMARC/DNSBL), routes
  the recipient through the hosted-domain and alias tables, runs their Sieve
  script, and delivers into their Mail room (or a `fileinto` room) as a
  `format_type = 4` message. See [Mail authentication](#mail-authentication).
  It offers no AUTH and never relays — submission lives in `quackmail_smtp_out`.
- **`quackmail_managesieve`** lets users install those Sieve scripts over RFC
  5804 (`PUTSCRIPT`/`SETACTIVE`/`GETSCRIPT`/…), validating each one against the
  same parser the delivery path uses, so a script that installs is a script
  that runs.
- **`quackmail_pop3`** authenticates a user and serves their Mail room over
  `USER/PASS/STLS/CAPA/STAT/LIST/UIDL/RETR/TOP/DELE/RSET/NOOP/LAST/QUIT` —
  the same command set, response wording and UPDATE-state semantics as a stock
  Citadel POP3 server. Two listeners like Citadel's: `qm_pop3` (110; dev 1110,
  STLS) and `qm_pop3s` (995; dev 1995, implicit TLS).
- **`quackmail_imap`** (minimal) maps mailboxes to rooms (`INBOX` → the user's
  Mail room) and supports `LOGIN/LIST/SELECT/FETCH/STORE/EXPUNGE`, reusing the
  `core/` MIME parser for `BODY[...]`/`ENVELOPE`. Two listeners like Citadel's:
  `qm_imap` (143; dev 1143, STARTTLS) and `qm_imaps` (993; dev 1993, implicit
  TLS). Access control follows RFC 4314 (`ACL` is advertised, as the real
  Citadel server does): `MYRIGHTS`, `GETACL`, `LISTRIGHTS`, `SETACL`,
  `DELETEACL`.

### Room mail and subaddressing

Rights are **derived** from the room itself — its owner, `qr_flags`, and the
caller's access level — and then unioned with any explicit `citadel_room_acl`
entry. Deriving keeps `qr_flags` the single source of truth for ordinary
permissions; the table only adds grants Citadel has nowhere else to put. Because
the two are unioned, an ACL entry can widen access but never narrow it: taking
read access away is still a matter of the room's flags.

Two grants carry more than the rest. `a` ("administer") is what
`citadel::CanAdminister` asks for, and it is how a room is delegated: the holder
manages that room and nothing else, from the web, the CLI or `SETACL`. `l`
("lookup") makes an invitation-only room visible to the person named, so a
private room can be joined by invitation rather than only by an aide moving
people into it. `anyone` may never hold `a` — it covers callers who are not
signed in.

The grant that matters most for mail is RFC 4314's `p` ("post") right for the
special identifier `anyone`, which is what makes a public room reachable by
e-mail at `room_<name>@<fqdn>` (spaces in the room name become underscores):

```
a1 SETACL "Main Floor/Announcements" anyone lrsp      # from any mail client
```

```sh
quackcitadm.sh room acl Announcements anyone lrsp     # or from the CLI
quackcitadm.sh room acl Announcements                 # show the entries
quackcitadm.sh room acl Announcements anyone ''       # revoke
```

No room is reachable until someone does this, and a personal mailbox, an
invitation-only room and a passworded room are refused even if an entry says
otherwise. `qm_room_mail` is the site-wide off switch.

Mail to `user+detail@<fqdn>` (RFC 5233) files into that user's existing `detail`
folder — the same personal rooms IMAP shows as `INBOX/<folder>` and Sieve's
`fileinto` targets. A folder that does not exist falls back to the inbox rather
than being created, because the sender picks the name; `qm_subaddress_create`
opts in to creating them, and `qm_subaddress_sep` changes or disables the
separator. Sieve sees the split through `envelope :detail` / `:user`, and an
explicit `fileinto` outranks it.

Envelope resolution order is: room address, alias, local user, then the
subaddress split of the base address. Room first means a domain catch-all cannot
swallow `room_*`; subaddress last means an explicit alias for `bob+sales@` still
wins.

### MIME & message parsing (RFC 2045–2049, 822/2822/5322)

The `core/` MIME plumbing (`quackmail::mime`) is exposed as SQL table functions
on the umbrella extension, so you can parse the `raw` of any stored message:

```sql
SELECT section, content_type, filename, size_bytes
FROM   qm_mime_parts((SELECT raw::VARCHAR FROM citadel_messages WHERE msgnum = 1));

SELECT * FROM qm_mime_headers(msg);                       -- (name, value)
SELECT decoded FROM qm_mime_decode_header('=?UTF-8?B?SGVsbG8=?=');   -- -> "Hello"
SELECT name, address FROM qm_mime_addresses('Jane <jane@x.com>, Bob <bob@y.com>');
SELECT epoch, iso FROM qm_parse_date('Mon, 02 Jan 2006 15:04:05 -0700');
```

## TLS & AUTH

TLS uses the **system OpenSSL** (`libssl-dev`); DuckDB's bundled mbedTLS is
crypto-only. SASL `PLAIN`/`LOGIN` (SMTP) is offered only after TLS; credentials
are verified against `quackmail_users` with a constant-time compare. The native
Citadel `USER`/`PASS` verify against the same table.

Passwords are stored with **scrypt** (RFC 7914, N=16384 r=8 p=1 — 16 MiB and
~80 ms per check), through OpenSSL's `EVP_PBE_scrypt`. Memory-hardness is the
point: PBKDF2 and bcrypt both fit in a GPU's registers, scrypt does not. The
`algo` column is self-describing (`scrypt$16384$8$1`), so the work factor can
be raised later without invalidating rows already stored at the old one.

This replaced a single round of salted SHA-256, which a leaked
`quackmail_users` table gave up at GPU speed. Rows written by the old scheme
**still verify**, and are rewritten with scrypt on their owner's next
successful sign-in — otherwise an existing install would keep the weak hash for
every account until somebody happened to change a password, which for most
accounts is never.

## Build

Requires a C++17 compiler, CMake, Ninja/Make, and `libssl-dev`. The socket layer
is POSIX; the extensions build and run on Linux (CI targets `linux_amd64`).

```bash
git clone --recurse-submodules <repo>       # duckdb + extension-ci-tools submodules (pinned v1.5.4)
cd quackmail
GEN=ninja make                              # builds DuckDB + all extensions
```

Artifacts land in `build/release/extension/<name>/<name>.duckdb_extension`, and a
DuckDB CLI with every extension statically linked is at `build/release/duckdb`.

### Install from a release

Pushing a `v*` tag runs `.github/workflows/release.yml`, which builds the
extensions and attaches a `quackmail-<tag>-linux_amd64.tar.gz` bundle to a
GitHub Release. The bundle is self-contained — the `.duckdb_extension` files, a
statically linked `duckdb` CLI, and the `deploy/` scripts that run them as a
server. Nothing else is needed: no Python, no separate DuckDB.

```bash
sudo tar -xzf quackmail-<tag>-linux_amd64.tar.gz -C /opt
sudo mv /opt/quackmail-<tag>-linux_amd64 /opt/quackmail

sudo /opt/quackmail/quackcit.sh start
sudo /opt/quackmail/quackcitadm.sh user add alice s3cret
```

The scripts notice they are in an unpacked bundle rather than a checkout and
put the database in `/var/lib/quackcit`, logs in `/var/log/quackcit` and the
control FIFO in `/run/quackcit`; point `QUACKCIT_STATE_DIR`, `QUACKCIT_LOG_DIR`
and `QUACKCIT_RUN_DIR` somewhere writable to run without root.
`quackcit.service` in the bundle is a systemd unit template.

To drive DuckDB by hand instead:

```bash
cd /opt/quackmail
./duckdb -unsigned          # unsigned extensions require -unsigned
```
```sql
LOAD './quackmail.duckdb_extension';
LOAD './quackmail_citadel.duckdb_extension';
```

## Try it

```bash
./build/release/duckdb
```
```sql
LOAD quackmail;
LOAD quackmail_citadel;
CALL qm_user_add('alice', 'secret');
CALL cit_start('127.0.0.1', 5040);
```
From another shell, drive the native protocol by hand:
```bash
nc 127.0.0.1 5040
200 quackcit|QuackCit BBS|quackmail.test|QuackCit 0.1.0
USER alice
300 Password required for alice.
PASS secret
200 alice|4|0|0|0|1|0
GOTO Lobby
200 Lobby|0|0|1|1|0|0|0|0||0|0|0|0
ENT0 1||0|0|Hello
400 Enter message; terminate with '000' on a line by itself.
Hi from the Lobby.
000
200 1
MSGS all
100 listing follows
1
000
QUIT
```
Deliver mail and read it back over POP3 (unified store):
```sql
LOAD quackmail_smtp_in; LOAD quackmail_pop3;
CALL qm_smtp_in_start('127.0.0.1', 2525, starttls => true);
CALL qm_pop3_start('127.0.0.1', 1110);
```

## Running and administering a server

`deploy/` has two scripts, both POSIX shell with no interpreter to install. Both
read `deploy/quackcit.conf` (override the path with `QUACKCIT_CONF`), where the
database location, bind address, TLS material and every port live. Values
already in the environment win, so one-off overrides work. They work the same
from a git checkout and from an unpacked release, detecting which they are in.

```bash
deploy/quackcit.sh start          # background, PID file, log file
deploy/quackcit.sh status
deploy/quackcit.sh logs 100
deploy/quackcit.sh stop

QUACKCIT_PORT_SMTP=25 deploy/quackcit.sh foreground   # run in this terminal
```

**TLS out of the box.** With no certificate configured, the first `start`
generates a self-signed one under `$QUACKCIT_TLS_DIR` (`/var/lib/quackcit/tls`
in a release, beside the database in a checkout) and starts every listener with
it — including the implicit-TLS ones, SUBMISSIONS/POP3S/IMAPS/TELNETS/NNTPS/XMPPS and the
HTTPS web interface, which are skipped outright when there is no certificate to
give them. It is issued to `hostname -f` with `localhost`, `127.0.0.1` and `::1`
as alternative names, lasts ten years, and is reused by every restart, so a
client that accepted the fingerprint keeps working. Nothing will trust it —
for production point `QUACKCIT_TLS_CERT` and `QUACKCIT_TLS_KEY` at real material
(a Let's Encrypt `fullchain.pem`/`privkey.pem` pair works as-is), which takes
precedence and disables the generated one. `QUACKCIT_TLS_AUTOGEN=0` opts out
entirely.

**One account, with a password you have to go and read.** The first start of an
empty database creates the accounts in `QUACKCIT_SEED_USERS`, which defaults to
a single aide named `admin`. Its password is generated — 24 alphanumerics from
`openssl` or `/dev/urandom` — and written to
`$QUACKCIT_SEED_SECRET_FILE` (`/var/lib/quackcit/initial-credentials` in a
release), mode `0600`. It is never printed to the terminal or into the log;
only the path is. Change it and delete the file. Seed whoever you like instead,
generating some passwords and choosing others:

```sh
: "${QUACKCIT_SEED_USERS:=admin - 6
alice - 4
bob hunter2 4}"
```

`<name> <password> <axlevel>` per line, `-` for "generate one", access level 6
for an aide and 4 for an ordinary user. This runs only while the database has
no accounts at all — it cannot reset a password later, so use
`quackcitadm.sh user add` after that.

**The server names itself after the host.** The first start resolves
`hostname -f` (then `hostname`, then `localhost`) and stores it as `c_fqdn`,
the name used for SMTP banners, `Received:` and `Message-ID` headers, the domain
mail is accepted for, and the certificate subject. It only ever replaces an
empty value or the `quackmail.test` placeholder, so a name you chose survives
every restart. Set `QUACKCIT_FQDN` to decide it yourself, in which case it is
authoritative and applied on every start.

**The log is syslog.** `quackcit.log` is RFC 5424, one record per line:

```
<22>1 2026-07-28T15:41:02Z mail quackcit 12345 server - qm_http	true	0.0.0.0	8080	0	started
```

`<22>` is `mail.info` — set `QUACKCIT_SYSLOG_FACILITY` to any facility name or
number. Everything the server prints goes through it, so a collector tailing the
file gets the same records `quackcit.sh logs` shows you. `quackcit.err` is
deliberately *not* in that format and should not be pointed at a collector: it
is the control channel's error return path, and `quackcitadm.sh` reads a failed
request's error text out of it byte for byte. Neither file is rotated yet.

`quackcitadm.sh` administers the server — users, domains, aliases, access
rules, DKIM keys, quotas, Sieve scripts, the outbound queue and server config:

```bash
deploy/quackcitadm.sh user add alice s3cret
deploy/quackcitadm.sh domain add example.com
deploy/quackcitadm.sh alias add sales@example.com alice
deploy/quackcitadm.sh alias add @example.com catchall     # domain catch-all
deploy/quackcitadm.sh acl block ip 192.0.2.0/24 'noisy range'
deploy/quackcitadm.sh acl allow ip 192.0.2.7 'except this host'
deploy/quackcitadm.sh rbl add zen.spamhaus.org
deploy/quackcitadm.sh dkim keygen example.com mail
deploy/quackcitadm.sh ratelimit status alice
deploy/quackcitadm.sh queue list
deploy/quackcitadm.sh help
```

It works whether or not the server is running. **DuckDB allows a single
read-write process per database file**, so while the server holds it the script
cannot simply open the file; it sends the statement over the server's control
channel instead, and falls back to opening the database directly when the server
is stopped.

The server *is* the DuckDB CLI, holding the database open with every listener
extension loaded — the listeners are threads inside it. Its standard input is a
FIFO opened read-write, which is both what keeps the process alive (the read
never reaches end-of-file) and what `quackcitadm.sh` administers it through: a
request is SQL wrapped in `.output` redirections, and the reply is the file the
CLI writes. The FIFO accepts arbitrary SQL, so it is created mode 0600 and
should not live on a shared filesystem.

## Tests

```bash
make test                                   # sqllogictest: load/start/stop/status, admin fns, MIME,
                                            #   site policy (test/sql/mailpolicy.test) and the
                                            #   Sieve parser (test/sql/sieve.test)
pip install duckdb==1.5.4
python3 test/integration/test_citadel.py     # native protocol: NEWU->GOTO->ENT0->MSGS->MSG0
python3 test/integration/test_smtp_in.py     # MX: recipient validation, domains, aliases, ACLs -> POP3
python3 test/integration/test_smtp_policy.py # outbound DKIM signing + per-user rate limiting
python3 test/integration/test_lmtp.py        # LMTP per-recipient replies, no spam checks
python3 test/integration/test_managesieve.py # ManageSieve round trip, then the filter routes delivery
python3 test/integration/test_sieve_extensions.py # flags reach IMAP, vacation answers a sender once
python3 test/integration/test_itip.py        # scheduling: PUT mails an invitation, a reply updates the event

pip install jmapc                            # optional; the test below skips itself without it
python3 test/integration/test_jmap_client.py # JMAP driven by a third-party client library
```

Most integration tests assert the wire format this server produces, which
cannot catch a format that is self-consistently wrong. `test_caldav.py` passed
for months while real clients could not sync, because resource names were
required to equal the object UID and vdirsyncer names them itself.
`test_jmap_client.py` is the answer to that class of bug for JMAP: every
request goes through `jmapc`'s own models, so a response shape somebody else's
deserializer rejects fails without anyone having thought to assert it. It found
that all three Session URLs were relative paths, which made `/jmap/` unusable
to any real client.

The DKIM tests run entirely offline: `dkim::Verify` takes an injectable key
lookup, and `policy::DkimKeyLookup` resolves locally stored keys before falling
back to DNS, so a sign→verify round trip needs no resolver.

## Roadmap

- **IMAP depth**: `BODYSTRUCTURE`, `CONDSTORE`/`QRESYNC`, server-side
  `SORT`/`THREAD`, and the extension batch (RFC 3501+). `NAMESPACE`, `UIDPLUS`,
  `MOVE`, `ACL` and `IDLE` are implemented.
- **Citadel breadth**: `CONF`/config verbs, message expiry/purge (`EXPI`),
  instant messaging (`SEXP`/`GEXP`), address books / vCard rooms, and the
  Citadel network mesh (inter-node message replication).
- **SMTP**: PIPELINING (2920), CHUNKING/BDAT (3030), and DSN (3461).
- **Mail authentication depth**: DMARC aggregate (`rua`) reporting, ARC (8617)
  so forwarded mail survives, and MTA-STS / DANE for outbound transport.
- **Hardening**: SCRAM-SHA-256 SASL (5802/7677) and charset transcoding beyond
  UTF-8/Latin-1. Password hashing is scrypt (see [TLS & AUTH](#tls--auth)).
- **Sieve**: the variables (5229), regex, vacation (5230) and imap4flags (5232)
  extensions; the parser covers the RFC 5228 core plus `reject`, `envelope`,
  `body` and `copy` today.
