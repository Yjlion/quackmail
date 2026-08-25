# Mailing lists and feeds

Both halves of the timer-driven side: fanning messages out of a room to
subscribers, and pulling messages from elsewhere into one.

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
