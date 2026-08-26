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

// The room-view picker, for the room settings page and the aide's room editor.
// One list: two copies had drifted, and the aide's could silently reset a room
// to a view it had simply never heard of.
std::vector<std::pair<std::string, std::string>> ViewOptions(Ctx &ctx);

// The handler for a view code. Falls back to the message-board handler for any
// view without one, which is always a truthful way to show a room: the objects
// really are messages.
const RoomViewHandler &ViewFor(int default_view);

// Each view file contributes its handler.
// Mail folders. Registered for VIEW_DRAFTS as well as VIEW_MAILBOX: a draft is
// a stored message in a personal room like any other, and the same listing and
// the same bulk actions apply to it.
const RoomViewHandler &MailboxView();
const RoomViewHandler &ContactsView();
const RoomViewHandler &CalendarView();
const RoomViewHandler &CalBriefView();
const RoomViewHandler &TasksView();
const RoomViewHandler &NotesView();
const RoomViewHandler &BlogView();
const RoomViewHandler &JournalView();
// Registered for VIEW_WIKIMD as well as VIEW_WIKI. See the note on that constant
// in citadel_store.hpp: markdown is a property of a page's MIME type here, not
// of the room's view code, so the two share every handler.
const RoomViewHandler &WikiView();

// Every e-mail address in the signed-in user's own Contacts room, as
// "Name <address>" strings — for a compose-form address-book picker. Empty
// when signed out or there is no Contacts room yet.
std::vector<std::string> ContactAddressOptions(Ctx &ctx);

// The tasks list's one-click complete toggle. It is not part of RoomViewHandler
// because no other view has an equivalent, and inventing a generic "toggle" slot
// for one caller would be worse than one extra route.
void TasksComplete(Ctx &ctx, const quackmail::citadel::Room &room);

// The /bbs/room/:n/item/... routes, which every object view shares.
void RegisterViewRoutes(std::vector<Route> &out);

// The wiki's own routes. Separate because a wiki page is addressed by name
// rather than by message number, so it does not fit the shared /item/ space.
void RegisterWikiRoutes(std::vector<Route> &out);

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

// The same, rendering nothing: returns the new msgnum, or -1 with `status` set
// to the HTTP status the failure deserves and `err` to a human reason. This is
// what SaveObject is built on, and what the DAV layer calls — a PUT needs the
// message number for its ETag and a status code rather than a styled page.
int64_t SaveObjectRaw(Ctx &ctx, const quackmail::citadel::Room &room, const std::string &euid,
                      const std::string &subject, const std::string &content_type,
                      const std::string &body, int &status, std::string &err);

// ---- rich text (Markdown/HTML), shared across object views ---------------
// Wiki pages were the first view to store a body as either Markdown or HTML
// (web_wiki.cpp) rather than plain text. These are the reusable pieces of
// that pipeline, factored out so Notes/Tasks/Calendar/Blog/Journal can offer
// the same choice without each re-deriving it. Wiki itself still layers its
// own [[Link]] resolution on top in web_wiki.cpp; everything else uses these
// exactly as they are.

// The two content-type strings a stored body's format is recorded as.
extern const char *const kHtmlContentType;
extern const char *const kMarkdownContentType;

// The "Format" <select> every edit form offering rich text shares.
std::string FormatSelect(const std::string &field_name, const std::string &current);

// Render a stored body to safe HTML for display. Markdown is rendered then
// sanitized; HTML is sanitized directly — both go through the same allow-list
// sanitizer, because generated markup gets no special trust either.
std::string RenderFormattedBody(const std::string &body, const std::string &content_type);

// Given what an edit form posted for "format" and "body" (or an equivalent
// text field), decide the content type to store under and the body to store.
// HTML is sanitized *before* it is stored, so what is kept is already safe
// rather than depending on every future reader to clean it; Markdown is kept
// as typed, since it is rendered fresh on every view.
void ResolveFormat(const std::string &posted_format, const std::string &posted_body,
                   std::string &content_type, std::string &stored_body);

} // namespace qmweb
} // namespace duckdb
