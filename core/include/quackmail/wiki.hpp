#pragma once

#include "duckdb.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace quackmail {
namespace wiki {

// Wiki rooms: pages, and the version control behind them.
//
// **The storage convention is Citadel's, exactly.** It was read from
// `citadel/server/modules/wiki/serv_wiki.c` and `webcit/wiki.c` rather than
// invented, so a page and its history written here are readable by WebCit and
// the Citadel clients, and theirs are readable here. A QuackCit-shaped revision
// table would have been much easier to write and would have interoperated with
// nothing — the same reasoning that stores sticky notes as `text/vnote` rather
// than as VJOURNAL.
//
// The shape:
//
//   * A page is an ordinary `format_type = 4` message whose **euid is the
//     normalized page name**, and whose subject is that name too.
//   * Its history is a **separate message** with euid `<page>_HISTORY_`, author
//     "Citadel", holding a `multipart/mixed` whose parts are each a
//     `text/x-diff`. New revisions are **prepended**, so the parts run
//     newest-first.
//   * Each part's `Content-disposition: attachment; filename="..."` carries a
//     base64 *memo*, `"<old msgnum>|<old timestamp>|<old author>|"`, **encoded
//     including its trailing NUL** — which is what Citadel does, and what a
//     decoder here has to expect.
//   * The diffs run **backwards**: each one takes the text from the newer
//     revision to the older one, so reading revision R means applying patches to
//     the current page until the memo's msgnum is R.
//   * A revision's identifier is the **old message number**. Nothing is minted.
//   * An edit that produces an empty diff is rejected rather than stored.

// Page name to euid: every byte below 0x20 or above 0x7F becomes '_', then
// ASCII lowercase. A space survives as a space, which surprises people, so it is
// worth saying: this is `str_wiki_index()` in webcit/wiki.c and it must stay
// byte-for-byte identical or our euids stop matching theirs. An empty name is
// the front page, "home".
std::string NormalizeName(const std::string &name);

// The euid of a page's history companion, and the test for one. Citadel matches
// the last nine bytes case-insensitively, which is what stops the save hook
// recursing into the history it just wrote.
std::string HistoryEuid(const std::string &page_euid);
bool IsHistoryEuid(const std::string &euid);

struct Revision {
	int64_t rev = 0;       // the message number the page had at this revision
	int64_t timestamp = 0; // unix seconds
	std::string author;
	std::string diff; // the reverse patch that reaches this revision
};

// Every revision of a page, newest first. An absent or unparseable history is
// an empty list rather than an error: a page written before versioning existed
// simply has no history, and that is not a failure to report to a reader.
std::vector<Revision> History(duckdb::Connection &con, int64_t room_num,
                              const std::string &page_euid);

// The raw message bytes of a page as of revision `rev`, reconstructed by
// applying the stored reverse diffs to the current version. `rev` of 0 means
// the current version.
bool RevisionRaw(duckdb::Connection &con, int64_t room_num, const std::string &page_euid,
                 int64_t rev, std::string &raw_out, std::string &err);

enum class RecordResult {
	Recorded,    // a history entry was written
	NoPrevious,  // the page is new, so there is nothing to diff against
	Unchanged,   // byte-identical to the current version; the save must be refused
	Error,
};

// Record a revision for a page about to be replaced by `new_raw`.
//
// Called from citadel::UpsertByEuid rather than from any one front-end, because
// Citadel does this with a server-wide save hook: an IMAP APPEND or a Citadel
// ENT0 into a wiki room has to produce history too, or the history is only as
// complete as the web interface's share of the edits.
RecordResult RecordRevision(duckdb::Connection &con, int64_t room_num,
                            const std::string &page_euid, const std::string &new_raw,
                            std::string &err);

struct Page {
	std::string euid;
	std::string title; // the subject, which is the name as the author typed it
	int64_t msgnum = 0;
	int64_t msgtime = 0;
	std::string author;
};

// Every page in a room, alphabetical by euid, with history companions removed.
std::vector<Page> ListPages(duckdb::Connection &con, int64_t room_num);

// Delete a page and its history together. Leaving the `_HISTORY_` message
// behind would resurrect the old revisions under a page recreated later.
bool DeletePage(duckdb::Connection &con, int64_t room_num, const std::string &page_euid,
                std::string &err);

// Is this room a wiki? Both codes, so a caller does not have to know that there
// are two. See the note on VIEW_WIKIMD in citadel_store.hpp.
bool IsWikiView(int64_t default_view);

} // namespace wiki
} // namespace quackmail
