# Protocol coverage

One table per protocol: the RFCs it claims, what is implemented, and what is
not. The "not" column is the honest half — it is derived from
[`TODO.md`](../TODO.md), which is the live backlog.

Ports are given as **standard (dev default)**. The dev defaults are
non-privileged and chosen so QuackCit can run beside a real Citadel server;
they live in `quackcit_services()` in
[`deploy/quackcit_common.sh`](../deploy/quackcit_common.sh).

## At a glance

| Protocol | Extension | Port(s) | TLS |
|---|---|---|---|
| Citadel | `quackmail_citadel` | 504 (5040) | STARTTLS |
| SMTP (MX) | `quackmail_smtp_in` | 25 (2525) | STARTTLS |
| LMTP | `quackmail_smtp_in` | 24 (2033) | none, loopback only |
| Submission | `quackmail_smtp_out` | 587 (2587) | STARTTLS |
| Submissions | `quackmail_smtp_out` | 465 (2465) | implicit |
| POP3 / POP3S | `quackmail_pop3` | 110 (1110) / 995 (1995) | STLS / implicit |
| IMAP / IMAPS | `quackmail_imap` | 143 (1143) / 993 (1993) | STARTTLS / implicit |
| ManageSieve | `quackmail_managesieve` | 4190 | STARTTLS |
| NNTP / NNTPS | `quackmail_nntp` | 119 (1119) / 563 (1563) | STARTTLS / implicit |
| XMPP / XMPPS | `quackmail_xmpp` | 5222 (15222) / 5223 (15223) | STARTTLS / implicit |
| Telnet / Telnets | `quackmail_telnet` | 23 (2300) / 992 (2992) | none / implicit |
| HTTP / HTTPS | `quackmail_http` | 80 (8080) / 443 (8443) | none / implicit |

`quackmail_spool` binds nothing: it is the timer-driven half (mailing lists,
remote pulls, certificate renewal). See [Mailing lists and feeds](lists.md) and
[TLS](tls.md).

## Citadel (native)

The centrepiece. See [The BBS](bbs.md) for the verb list.

**Implemented** — session, auth, users and registration, floors, rooms and room
admin, message reading and posting, last-read pointers, express messages
(`SEXP`/`GEXP`), who-list (`RWHO`), RFC 4314 rights.

**Not** — `CONF` and the config verbs, `EXPI` message expiry, the Citadel
network mesh (inter-node replication).

## SMTP

| | |
|---|---|
| **In** | RFC 5321, SPF (7208), DKIM (6376), DMARC (7489), DNSBL, hosted domains, aliases, allow/block rules, per-user rate limits, Sieve at delivery |
| **Out** | authenticated submission (4954), DKIM signing, MX resolution, a queue drainer with retry |
| **LMTP** | RFC 2033, loopback only — it performs no sender authentication and no spam filtering by design |
| **Not** | PIPELINING (2920), CHUNKING/BDAT (3030), DSN (3461) |

Mail authentication depth still open: DMARC aggregate (`rua`) reports, ARC
(8617), MTA-STS and DANE. See [Mail](mail.md).

## IMAP

**Implemented** — IMAP4rev1 (3501), `NAMESPACE` (2342), `UIDPLUS` (4315),
`MOVE` (6851), `ACL` (4314), `IDLE` (2177), `QUOTA` (9208), `LIST-EXTENDED`,
`SASL-IR`.

**Not** — `CONDSTORE`/`QRESYNC`, server-side `SORT`/`THREAD`, `BODYSTRUCTURE`.

`QUOTA` advertises `QUOTA=RES-STORAGE` and serves `GETQUOTAROOT`, `GETQUOTA`
and `SETQUOTA`. Three things about it are worth stating because they are the
ones clients trip over:

- **There is one quota root per user, named `""`**, shared by every mailbox.
  That is Dovecot's default and therefore the best-tested value in the wild.
- **`STORAGE` is in kibibytes, not bytes** (RFC 9208 §5). Usage rounds up and
  the limit rounds down. Reporting bytes here is the classic bug: it shows
  10 MB as 10 GB.
- A public room, or a user with no ceiling, gets a `* QUOTAROOT` line naming no
  root and **no `* QUOTA` line at all** — the correct answer for "not in any
  root", rather than an invented infinity.

`SETQUOTA` from a non-aide is `NO [NOPERM]`. An `APPEND` that would go over is
refused with `NO [OVERQUOTA]` **before** the `+` continuation, so the client is
never left pushing a literal at a server that has stopped listening for one.

## POP3

RFC 1939 plus `STLS` (2595), `SASL` (5034), `UIDL`, `TOP`. Serves each user's
Mail room. Nothing outstanding.

## Sieve and ManageSieve

**Implemented** — RFC 5228 core, plus `reject`, `envelope`, `body`, `copy`,
`imap4flags` (5232), `variables` (5229) and `vacation` (5230). ManageSieve
itself is RFC 5804.

**Not** — `regex`, deliberately: it is an expired draft, and the value it adds
over `:matches` (which captures into `${1}`..`${9}`) is small next to putting a
backtracking engine on the delivery path against sender-chosen text.

## NNTP

RFC 3977 reader **and** poster; rooms are newsgroups. Not implemented: the peer
feed verbs `IHAVE`/`CHECK`/`TAKETHIS`, which belong with the Citadel network
mesh.

## XMPP

c2s only (6120/6121): presence, roster from the user directory, one-to-one
messages bridged to Citadel express messages. Not implemented — and not
implemented by real Citadel either — MUC, offline storage, stored
rosters/subscriptions, and s2s.

## HTTP

Three surfaces on one listener pair. See [The web interface](web.md).

| Surface | Standard | State |
|---|---|---|
| Webmail, BBS, groupware, admin | — | server-rendered, works with JavaScript off |
| CalDAV | 4791, 5545, 6638 (partial) | collections, objects, `calendar-query`, free/busy, iTIP/iMIP |
| CardDAV | 6352 | collections, objects |
| JMAP | 8620, 8621, 9425 | Session, `Email/*`, `Mailbox/*`, `Thread/*`, blob up/download, submission, `Quota/get` |

**Not** — RFC 6638 auto-scheduling (the `schedule-inbox-URL`/`schedule-outbox-URL`
collections and outbox `POST`), DAV `LOCK`/`UNLOCK` (ETags and `If-Match` are
the consistency story instead), `MKCALENDAR`/`MKCOL`, `calendar-query` filters
past comp-name and time-range, `expand` on a recurring event. JMAP:
`Email/import`, `SearchSnippet/get`, push over EventSource, `Quota/query`.

JMAP's quota (RFC 9425, `urn:ietf:params:jmap:quota`) reports the same ceiling
IMAP does but in **octets**, not kibibytes — the two units are not
interchangeable and neither conversion is reused for the other. An account with
no ceiling answers `Quota/get` with an empty list, because `hardLimit` is a
mandatory `UnsignedInt` with no encoding for "unlimited" and inventing one
breaks every client's percentage arithmetic. The state string is the account
state joined to the quota generation: the first moves when usage moves, the
second when an operator edits the ceiling, and neither alone can answer
`Quota/changes`.

## Telnet

Not a protocol Citadel implements at all: a real install has no telnet
listener, and the BBS experience comes from the `citadel` text client speaking
the native protocol. `quackmail_telnet` **is** that client, running
server-side, driven by the same `citadel.rc` menu.

**Not** — file transfer (the `QR_UPLOAD`/`QR_DOWNLOAD`/`QR_VISDIR` room flags
and the `.Read file` / `.Admin File` family), `C`hat, and help files.

## ACME

RFC 8555, **http-01 only**, in `quackmail_spool` with the challenge served by
`quackmail_http`. Directory, nonces, account, order, authorization, finalize,
download and revoke; RS256 account keys; renewal on a worker with exponential
backoff; a renewed certificate taken into service without dropping a connection.

**Not** — dns-01 and therefore wildcards (`dns.hpp` resolves, it does not
update), tls-alpn-01, External Account Binding, ES256. See
[TLS](tls.md#automatic-certificates-acme).

## MIME and message parsing

RFC 2045–2049, 822/2822/5322: multipart, nested parts, `quoted-printable` and
`base64`, RFC 2047 encoded-words, RFC 2231 parameter continuations, and date
parsing. Charset support is UTF-8 and Latin-1; transcoding beyond those is
open.
