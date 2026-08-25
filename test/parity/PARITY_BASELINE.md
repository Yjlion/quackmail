# Parity baseline — real Citadel (debian.lan) vs QuackCit

Captured from a stock Citadel Groupware server on `debian.lan` (users admin/admin,
leo/leo) via the native protocol (504) and IMAP (143 STARTTLS). Raw transcripts in
`real_citadel/`. This is the oracle QuackCit must match.

## Greeting (native 504)
```
200 debian Citadel server ready.
```
QuackCit currently sends `200 <node>|<human>|<fqdn>|<ver>` (pipe-delimited) — **mismatch**.
Target: `200 <nodename> Citadel server ready.`

## Floors (LFLR)
```
0|Main Floor|<refcount>
```
Only **Main Floor** (0). QuackCit already seeds this. ✓

## Rooms (LKRA) — canonical default set
Line format: `name|QRflags|floor|listorder|?|?|view|view|mtime|`

Public / system rooms (mailbox_owner = 0):
| Room                 | QRflags | floor | view | notes |
|----------------------|---------|-------|------|-------|
| Lobby                | 2       | 0     | 0    | QR_PERMANENT; base room |
| Aide                 | 6       | 0     | 0    | PERMANENT+PRIVATE (aide only) |
| Global Address Book  | 6       | 0     | 2    | address-book view, aide/special |
| Trashcan             | 2       | 0     | 0    | system trash (public floor room) |

Per-user mailbox rooms (QRflags 16390 = MAILBOX 16384 + PRIVATE 4 + PERMANENT 2),
auto-created for every user:
| Room        | view | IMAP name        |
|-------------|------|------------------|
| Mail        | 1    | INBOX            |
| Sent Items  | 1    | INBOX/Sent Items |
| Drafts      | 1    | INBOX/Drafts     |
| Trash       | 1    | INBOX/Trash      |
| Calendar    | 3    | INBOX/Calendar   |
| Contacts    | 2    | INBOX/Contacts   |
| Notes       | 5    | INBOX/Notes      |
| Tasks       | 4    | INBOX/Tasks      |

Citadel views: 0=BBS/messages, 1=Mail, 2=AddressBook, 3=Calendar, 4=Tasks, 5=Notes.

**QuackCit today seeds only Lobby(0)+Aide(1)+Main Floor** and creates just a "Mail"
room on demand — missing Sent Items/Drafts/Trash/Calendar/Contacts/Notes/Tasks + the
public Trashcan/Global Address Book.

## QR flag values (must match on the wire)
Real Citadel: PERMANENT=2, PRIVATE=4, PASSWORDED=8, GUESSNAME=16, DIRECTORY=32,
NETWORK=2048, READONLY=8192, MAILBOX=16384. QuackCit's enum had PERMANENT=1 and
MAILBOX=0x400 — **realigned** to the above so LKRA/GOTO qr_flags match.

## IMAP (143)
- CAPABILITY (pre-TLS): `IMAP4REV1 NAMESPACE ID AUTH=PLAIN AUTH=LOGIN UIDPLUS STARTTLS ACL METADATA`
  (post-STARTTLS: same minus STARTTLS). No LOGINDISABLED; plaintext LOGIN allowed.
- NAMESPACE: `(("INBOX/" "/")) NIL (("Main Floor/" "/"))` — delimiter `/`.
- LIST `"" "*"` (leo):
  ```
  (\NoSelect \HasChildren) "/" "Main Floor"
  () "/" "INBOX"                    <- Mail
  () "/" "INBOX/Calendar"
  () "/" "INBOX/Contacts"
  () "/" "INBOX/Drafts"
  () "/" "INBOX/Notes"
  () "/" "INBOX/Sent Items"
  () "/" "INBOX/Tasks"
  () "/" "INBOX/Trash"
  () "/" "Main Floor/Lobby"
  () "/" "Main Floor/Trashcan"
  ```
- STATUS INBOX: `(MESSAGES 0 RECENT 0 UIDNEXT 9 UNSEEN 0)`.

QuackCit IMAP today: no STARTTLS/AUTHENTICATE/NAMESPACE/STATUS/SEARCH/APPEND/COPY/MOVE/
CREATE/DELETE/RENAME/SUBSCRIBE, UIDVALIDITY hardcoded 1, lists only the user's own
mailbox rooms (no INBOX/ prefix, no floor path for public rooms).

## Native protocol samples
- TIME: `200 <unixtime>|<gmtoff>|<isdst>|<serverstart>`
- RWHO: `100`-listing, `session|user|room|host|client|lastcmd_time|lastcmd|flags|...|axlevel`
- INFO: ~30 fields (QuackCit sends 14) — see `real_citadel/native_admin.txt`.
- IDEN → `200 Ok`; MSGP → `200 Preferred MIME formats have been set.`

## Workstream mapping
- **A** (seeding): add the 4 public rooms + 8 per-user rooms with correct views/flags; realign QR_*.
- **B** (IMAP): STARTTLS/AUTHENTICATE, NAMESPACE, INBOX/ + floor-path LIST, STATUS/SEARCH/APPEND/COPY/MOVE, folder ops, real UIDVALIDITY.
- **C** (Citadel): RWHO/SEXP/GEXP, MOVE/DELE, TIME, aide verbs, greeting/INFO fidelity.

## Wiki rooms (VIEW_WIKI)

Captured 2026-08-25 from the docker oracle: a room created with
`CRE8 1|WikiParity|0||0|0|64|6`, its view set with
`SETR WikiParity|||2|0|0|64|6|0` (SETR's field order is *not* GETR's — floor is
index 5, order 6, view 7), and one page written three times with
`ENT0 1||0|4|home||1|||home` (field 9 is the EUID; field 6 is do_confirm, and
with it off the server sends no reply at all after the `000`).

`wiki_page.txt` and `wiki_history.txt` are `MSG2` of the two messages. Note that
`MSG2` prepends synthesized RFC822 headers; the *stored* blob begins at the
`Content-type:` line, and that blob — headers included — is what Citadel diffs.

What the capture confirmed, all of it now pinned offline in `test/sql/wiki.test`:

- `WIKI history|home` returns one memo per line, newest first, as
  `<old msgnum>|<timestamp>|<author>|`.
- Each part's `filename` is that memo base64'd **including its trailing NUL**.
- The preamble text and the `Citadel--Multipart--%04x--%08lx` boundary shape.
- The diffs are **reverse** — the newest one removes the line most recently
  added — and are ordinary 3-line-context unified diffs over the whole blob.
- Our patcher replays every one of the oracle's four revisions, and the diff we
  produce for the same change is byte-identical to libxdiff's.
- **A save whose text is unchanged is rejected** (`-3 Internal error`), which is
  the behaviour `UpsertByEuid` now reproduces as `err = "no changes"`.
