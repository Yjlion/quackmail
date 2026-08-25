# Architecture

How QuackCit is put together: what the extensions are, what the tables
are, and why cross-extension state lives in SQL rather than in C++.

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
start if you have none (see [Running a server](running.md)).
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
