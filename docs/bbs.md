# The BBS

The native Citadel protocol, the server-side text client a plain
`telnet` reaches, and the two other protocols that read the same rooms.

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

- **Configuration**: `CONF` — `GETVAL`/`PUTVAL`/`LISTVAL` (the modern key/value
  form, which maps straight onto `citadel_config`), `GETSYS`/`PUTSYS` (arbitrary
  stanzas as euid-keyed messages in *Local System Configuration*), and the
  deprecated positional `GET`/`SET`. Citadel's own source marks those last two
  "please do not add fields or change their order", and its text client still
  sends them, so all 73 positions are answered — retired ones included, as blank
  lines, because renumbering would shift every field after them. Aide-only, and
  a `SET` is logged to the Aide room.
- **Expiry**: `GPEX`/`SPEX` read and write a policy (mode `0` next-level, `1`
  manual, `2` by count, `3` by age) at four levels — room, floor, site, and
  mailboxes — and `TDAP` runs the purger. A room that says next-level defers to
  its floor, then to the site; with nothing configured anywhere the answer is
  *manual*, because deleting mail on the strength of an empty configuration
  would be the worst available reading of it. `QR_PERMANENT` rooms are never
  swept, which is what that flag has always claimed and never did.

  Worth knowing: the modern Citadel **server** implements neither `GPEX` nor
  `SPEX`, while its own text client still sends them — so that client's expiry
  editor talks to nothing against a real Citadel. It works here.

  The sweep also runs on a timer (`qm_expire`, and `qm_expire_run()` for one
  pass now). Deliberately a timer rather than the 1-in-16 coin flip in the HTTP
  router that every other sweep hangs off: a site with no web traffic would
  otherwise never expire anything.

`DOWN`, `SCDN`, `TERM`, the Citadel network mesh and the native file-transfer
verbs (`OPEN`/`READ`/`CLOS`/`UOPN`/`WRIT`) are deferred (see Roadmap). There is
no `EXPI` verb in Citadel; `TDAP` is the one that runs the purger.

## The BBS shell (telnet)

![Signing in over telnet](../screenshots/text-login.png)

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

![Reading new messages](../screenshots/text-read-new.png)

![Entering a message](../screenshots/text-enter.png)

Preferences persist in `citadel_users.flags` using Citadel's own `US_*` bits, so
expert mode, floor mode and the paginator survive a disconnect — and the web
console's preferences page edits exactly the same column. The screen size comes
from the telnet NAWS negotiation, and listings pause at each screenful.

Sessions register in `citadel_sessions`, so telnet users and native Citadel
clients see each other in the who-list and can page one another.

`C`hat holds a conversation rather than firing a single page. It is built on the
same `citadel_express` rows `P`age, the web chat view and XMPP use, so a line
typed at a terminal reaches somebody reading their mail in a browser and their
reply comes back — one conversation, not four. `.` as the correspondent sends to
everyone currently online. There is no separate chat channel, which is the
deliberate difference from real Citadel's room-wide `CHAT`.

`.H`elp is prose, where `?` is the command menu. Topics are compiled in, so a
fresh install has working help with nothing to seed; an aide can override any of
them by posting to a room called `Help` with the euid `help/<topic>`.

### File directories

A room with the `QR_DIRECTORY` flag is a file area. The three companion flags
decide what may be done in it: `QR_UPLOAD` to deposit, `QR_DOWNLOAD` to fetch,
and `QR_VISDIR` to see the listing without being able to fetch — which is how an
upload-only drop box is expressed. All four are Citadel's own bits, and until now
nothing in this server read them.

| Command | |
|---|---|
| `.RF` | list the directory |
| `.RFG` | fetch a file — type it out, Xmodem, or base64 |
| `.EF` | upload one, by Xmodem or by pasting base64 |
| `.AFD` / `.AFE` | delete, or change a description (aide) |

**A file is a message**: a directory room's files are ordinary euid-keyed
messages carrying one attachment part, exactly like a mail attachment. That is
what makes per-user storage quotas, the room ACL, DAV sync tombstones and room
deletion apply to files without any of them being taught about files.

The transfer protocol is **Xmodem-1K with CRC**. Zmodem is not implemented: it is
a much larger protocol for the same result over a link TCP has already made
reliable. Typing a text file out through the pager, and base64 for a terminal
with no transfer protocol at all, cover the rest. Anything large, or a whole
directory at once, is better fetched over WebDAV at `/dav/files/` — the same
files, the same rooms, the same permissions.

Note one consequence of Xmodem having no length field: the last block is padded
and the padding is stripped on arrival, so a file whose real last byte is `0x1A`
cannot survive an Xmodem round trip. WebDAV and FTP have no such limit.

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

### Peer feeds

`IHAVE` (RFC 3977 §6.3.2) and `MODE STREAM` with `CHECK`/`TAKETHIS` (RFC 4644),
so this server can take a news feed from a peer. All three turn on one question
— *do I already have this article?* — which was unanswerable until a Message-ID
index existed: resolving an id previously meant scanning the selected group,
fine for a reader fetching one article and hopeless for a peer offering
thousands.

Unlike everything else here, **this half has no Citadel behind it**: Citadel's
own NNTP implements none of these verbs. The RFCs are the spec.

Three decisions worth knowing:

- **A peer must authenticate.** The verbs sit after the `480` gate deliberately.
- **An article for a group this server does not carry is refused permanently**
  (`437`), not deferred. Creating a room for every group a peer offers would let
  one peer fill the room list.
- **A transit article keeps the `From:` it arrived with.** Rewriting it to the
  peer's login would be forging it.

## Instant messaging (XMPP)

`quackmail_xmpp` speaks client-to-server XMPP: STARTTLS, SASL `PLAIN` (and the
legacy `jabber:iq:auth`), resource binding, sessions, `jabber:iq:roster`,
presence, `vcard-temp`, `urn:xmpp:ping` and service discovery.

Like Citadel, there is no stored roster: **the roster and presence list are the
people currently logged in** — read from `citadel_sessions`, so XMPP clients,
telnet users, native Citadel clients and anyone signed in to the web interface
all see one another. A `<message>` is written to `citadel_express`, the same
queue the native protocol's `SEXP`/`GEXP` uses, and queued messages are pushed
to connected XMPP clients as `<message>` stanzas. Sending from XMPP and reading
with `GEXP` from a Citadel client, from the BBS shell, or in a browser at
`/chat` works in every direction: there is one queue, and it is a table.

A browser is a presence row like any other — see
[Who is online](web.md) — which also means a `citadel_sessions` row now has to
be *reaped* rather than only unregistered. A front-end that declares a
heartbeat is held to it; one that cannot (a telnet session blocked in a read
has no timer) unregisters on a clean disconnect and is otherwise swept after
`qm_session_stale_secs`.
