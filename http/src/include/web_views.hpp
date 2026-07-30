#pragma once

#include "web.hpp"

#include "quackmail/citadel_store.hpp"

namespace duckdb {
namespace qmweb {

// Room views: what a room looks like when it holds something other than a
// message board.
//
// `citadel_rooms.default_view` has been stored and seeded correctly since the
// store was written, and read by nothing — a Calendar room rendered as a list
// of messages whose bodies were raw iCalendar. This is the dispatch that
// changes that.
//
// The URL space does not change. /bbs/room/:n stays canonical, because rooms are
// addressed by number (a Citadel room name may contain '/'), and RoomHref plus
// every /msg/:m/... route already hangs off it. A parallel /calendar/:n space
// would break bookmarks and touch every file for no gain.

struct RoomViewHandler {
	int view;           // a RoomView code
	const char *label;  // "Contacts", for the room's own toolbar
	const char *noun;   // "contact", for buttons and empty states

	// The room itself. Never null.
	void (*index)(Ctx &ctx, const quackmail::citadel::Room &room);
	// One object, by the message number carrying it. Null falls back to the
	// ordinary message read pane.
	void (*item)(Ctx &ctx, const quackmail::citadel::Room &room, int64_t msgnum);
	// The create/edit form. msgnum < 0 means "new".
	void (*edit)(Ctx &ctx, const quackmail::citadel::Room &room, int64_t msgnum);
	// POST handlers. Null means the view is read-only.
	void (*save)(Ctx &ctx, const quackmail::citadel::Room &room);
	void (*remove)(Ctx &ctx, const quackmail::citadel::Room &room);
};

// The handler for a view code. Falls back to the message-board handler for any
// view without one, which is always a truthful way to show a room: the objects
// really are messages.
const RoomViewHandler &ViewFor(int default_view);

// Does this view have a renderer of its own? Used to decide whether the room's
// toolbar should offer "add a contact" or "post a message".
bool HasCustomView(int default_view);

// Each view file contributes its handler.
const RoomViewHandler &ContactsView();
const RoomViewHandler &CalendarView();
const RoomViewHandler &CalBriefView();
const RoomViewHandler &TasksView();
const RoomViewHandler &NotesView();
const RoomViewHandler &BlogView();
const RoomViewHandler &JournalView();

// The tasks list's one-click complete toggle. It is not part of RoomViewHandler
// because no other view has an equivalent, and inventing a generic "toggle" slot
// for one caller would be worse than one extra route.
void TasksComplete(Ctx &ctx, const quackmail::citadel::Room &room);

// The /bbs/room/:n/item/... routes, which every object view shares.
void RegisterViewRoutes(std::vector<Route> &out);

// ---- shared helpers for object views -------------------------------------

// The MIME part of a groupware message: the text/vcard or text/calendar body.
// Returns "" when the message holds nothing of that type, which is how a stray
// ordinary message in a Contacts room is recognised rather than mis-rendered.
std::string ObjectBody(const quackmail::citadel::Message &msg, const std::string &want_type);

// Wrap a groupware body in the RFC822 envelope the store keeps it in.
// format_type 4 already means "serve `raw` verbatim" everywhere in the tree, so
// an object written this way reads back as a sensible MIME message over IMAP
// with no new code, and mime::FlattenParts finds the part.
std::string WrapObject(Ctx &ctx, const std::string &content_type, const std::string &body,
                       const std::string &subject, const std::string &uid);

// Save an object into a room, replacing any with the same euid. Applies CanPost
// and renders the error page itself, so callers can `if (!SaveObject(...)) return;`.
bool SaveObject(Ctx &ctx, const quackmail::citadel::Room &room, const std::string &euid,
                const std::string &subject, const std::string &content_type, const std::string &body);

} // namespace qmweb
} // namespace duckdb
