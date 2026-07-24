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
| 8 | SMTP authentication (SPF/DKIM/DMARC/DNSBL), site policy, outbound signing + rate limiting, LMTP, real Sieve/ManageSieve, `deploy/` CLI tooling | in progress |

Phases 2 and 3 were validated byte-for-byte against the real Citadel server, and
the **official `citadel` text client drives a full clean session** against
QuackCit (login banner, `<K>nown rooms`, no spurious errors).

## Decisions worth remembering

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
- **DMARC's organizational domain is approximated.** No Public Suffix List is
  bundled, so it is "last two labels" plus a table of common multi-label
  suffixes. The error direction is deliberate: relaxed alignment comes out
  stricter than a PSL would make it, never looser, so it cannot turn a Fail into
  a Pass. Bundling a PSL is on the backlog.
- **LMTP is the deliberate hole in the fence.** It performs no authentication
  and no spam filtering, by design, because it is the trusted local-injection
  path. It binds to loopback unless explicitly overridden, and the README says
  plainly not to expose it.
- **DuckDB allows one read-write process per database file**, so the admin CLI
  cannot open the database while the server holds it. `run_quackcit.py` exposes
  a mode-0600 Unix socket next to the database; `quackcitadm.sh` sends SQL there
  when the server is up and opens the file directly when it is down. That socket
  accepts arbitrary SQL — treat it as root-equivalent.
- **DuckDB table functions require constant-foldable arguments**, so
  `qm_dkim_verify(qm_dkim_sign(...))` is only expressible if both are *scalar*
  functions. The composable forms are scalar; `qm_dkim_verify_detail` remains a
  table function for the per-signature breakdown.

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
- QuackCit source lives in `~/quackmail`; the dev launcher is
  `deploy/run_quackcit.py` with `QUACKCIT_DB=~/quackcit.duckdb`. Seeded users
  `admin/admin` (aide) and `leo/leo` mirror the oracle for 1:1 diffing.
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
