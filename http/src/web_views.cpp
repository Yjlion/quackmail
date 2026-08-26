#include "web_views.hpp"

#include "quackmail/citadel_msg.hpp"
#include "quackmail/html_sanitize.hpp"
#include "quackmail/markdown.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/util.hpp"

#include <cstdlib>

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Message;
using quackmail::citadel::Room;

namespace {

// The fallback: a room whose view has no renderer is a message board. Every
// entry point is null, which is the signal to the router to use the existing
// BBS handlers unchanged.
const RoomViewHandler kBoardView = {
    quackmail::citadel::VIEW_BBS, "Messages", "message", nullptr, nullptr, nullptr, nullptr, nullptr};

} // namespace

std::vector<std::pair<std::string, std::string>> ViewOptions(Ctx &ctx) {
	// Only the views that have a renderer. VIEW_QUEUE is in the enum because
	// the wire numbering is Citadel's, not because we draw it; offering it here
	// would set a room to a view that falls back to the plain message list,
	// which reads as a bug rather than as a choice.
	//
	// There is one list, used by both the room settings page and the aide's
	// room editor. Two copies had already drifted: the aide's stopped at Notes
	// and could silently reset a Blog room to a message board on save.
	std::vector<std::pair<std::string, std::string>> out = {
	    {"0", "Message board"},      {"1", "Mailbox"},  {"2", "Address book"},
	    {"3", "Calendar"},           {"7", "Calendar, as a list"},
	    {"4", "Tasks"},              {"5", "Notes"},    {"6", "Wiki"},
	    {"10", "Blog"},              {"8", "Journal"}};
	// VIEW_WIKIMD is not a code current Citadel knows — see citadel_store.hpp.
	// It renders here, but offering it would put a number on the wire that
	// reads out of bounds in WebCit, so it takes a deliberate opt-in.
	if (ConfigBool(ctx.con, "qm_wiki_markdown_view", false)) {
		out.push_back({"12", "Wiki (Markdown view code)"});
	}
	return out;
}

const RoomViewHandler &ViewFor(int default_view) {
	switch (default_view) {
	case quackmail::citadel::VIEW_MAILBOX:
	case quackmail::citadel::VIEW_DRAFTS:
		return MailboxView();
	case quackmail::citadel::VIEW_ADDRESSBOOK:
		return ContactsView();
	case quackmail::citadel::VIEW_CALENDAR:
		return CalendarView();
	case quackmail::citadel::VIEW_CALBRIEF:
		return CalBriefView();
	case quackmail::citadel::VIEW_TASKS:
		return TasksView();
	case quackmail::citadel::VIEW_NOTES:
		return NotesView();
	case quackmail::citadel::VIEW_BLOG:
		return BlogView();
	case quackmail::citadel::VIEW_JOURNAL:
		return JournalView();
	case quackmail::citadel::VIEW_WIKI:
	case quackmail::citadel::VIEW_WIKIMD:
		return WikiView();
	default:
		// VIEW_BBS and VIEW_QUEUE. The queue is Citadel's own spool view rather
		// than a user one, and a board is what a board should look like.
		return kBoardView;
	}
}

// ---- shared helpers ------------------------------------------------------

// Both of these live in core now: inbound iTIP writes calendar objects into the
// same rooms this module does, and a second spelling of the wrapper would leave
// one of them unable to read what the other wrote.
std::string ObjectBody(const Message &msg, const std::string &want_type) {
	return quackmail::citadel::ObjectBody(msg, want_type);
}

std::string WrapObject(Ctx &ctx, const std::string &content_type, const std::string &body,
                       const std::string &subject, const std::string &uid) {
	return quackmail::citadel::WrapObject(content_type, body, subject, uid, ctx.username,
	                                      ConfigStr(ctx.con, "c_fqdn", "localhost"));
}

int64_t SaveObjectRaw(Ctx &ctx, const Room &room, const std::string &euid, const std::string &subject,
                      const std::string &content_type, const std::string &body, int &status,
                      std::string &err) {
	// The one post predicate. Re-deriving it is how the read-only flag stopped
	// applying to a front-end once before.
	if (!quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		status = 403;
		err = "You cannot add anything to this room.";
		return -1;
	}
	if (euid.empty()) {
		status = 400;
		err = "That object has no identifier, so it cannot be saved.";
		return -1;
	}

	Message msg;
	msg.euid = euid;
	msg.author = ctx.username;
	msg.author_usernum = quackmail::citadel::GetOrAssignUserNum(ctx.con, ctx.username);
	msg.msgtime = (int64_t)std::time(nullptr);
	msg.subject = subject;
	msg.format_type = 4;
	msg.origin_room = room.display_name;
	msg.node = ConfigStr(ctx.con, "c_nodename", "");
	msg.raw = WrapObject(ctx, content_type, body, subject, euid);

	std::string store_err;
	int64_t msgnum = quackmail::citadel::UpsertByEuid(ctx.con, msg, room.room_num, store_err);
	if (msgnum < 0) {
		status = 400;
		err = store_err.empty() ? "The object could not be saved." : store_err;
		return -1;
	}
	status = 200;
	return msgnum;
}

bool SaveObject(Ctx &ctx, const Room &room, const std::string &euid, const std::string &subject,
                const std::string &content_type, const std::string &body) {
	int status = 200;
	std::string err;
	if (SaveObjectRaw(ctx, room, euid, subject, content_type, body, status, err) >= 0) {
		return true;
	}
	if (status == 403) {
		Forbidden(ctx, err);
	} else {
		BadRequest(ctx, err);
	}
	return false;
}

// ---- rich text (Markdown/HTML) --------------------------------------------

const char *const kHtmlContentType = "text/html";
const char *const kMarkdownContentType = "text/x-markdown";

std::string FormatSelect(const std::string &field_name, const std::string &current) {
	return Select(field_name,
	              {{kHtmlContentType, "Formatted text (HTML)"}, {kMarkdownContentType, "Markdown"}},
	              current);
}

std::string RenderFormattedBody(const std::string &body, const std::string &content_type) {
	if (content_type != kMarkdownContentType) {
		return quackmail::html::SanitizeForCompose(body);
	}
	// The plain Options(): no [[Wiki Link]] resolution here, unlike
	// web_wiki.cpp's own RenderBody. That syntax only means something inside a
	// wiki room.
	return quackmail::html::SanitizeForCompose(
	    quackmail::markdown::Render(body, quackmail::markdown::Options()));
}

void ResolveFormat(const std::string &posted_format, const std::string &posted_body,
                   std::string &content_type, std::string &stored_body) {
	if (posted_format == kMarkdownContentType) {
		content_type = kMarkdownContentType;
		stored_body = posted_body;
		return;
	}
	content_type = kHtmlContentType;
	stored_body = quackmail::html::SanitizeForCompose(posted_body);
}

// ---- routes --------------------------------------------------------------

namespace {

int64_t CapNum(const Ctx &ctx, size_t i) {
	std::string s = ctx.Cap(i);
	return s.empty() ? -1 : (int64_t)std::strtoll(s.c_str(), nullptr, 10);
}

// Resolve the room and its view handler, or render the failure. Every route
// below starts with this, so a room whose view has no renderer 404s rather than
// reaching a null function pointer.
bool ViewRoom(Ctx &ctx, Room &room, const RoomViewHandler **vh) {
	if (!ResolveRoomNumFor(ctx, CapNum(ctx, 0), room)) {
		NotFound(ctx);
		return false;
	}
	if (!RequireUnlocked(ctx, room, RoomHref(room))) {
		return false;
	}
	const RoomViewHandler &found = ViewFor((int)room.default_view);
	if (!found.index) {
		NotFound(ctx);
		return false;
	}
	*vh = &found;
	return true;
}

void GetItemNew(Ctx &ctx) {
	Room room;
	const RoomViewHandler *vh = nullptr;
	if (!ViewRoom(ctx, room, &vh)) {
		return;
	}
	if (!vh->edit) {
		NotFound(ctx);
		return;
	}
	vh->edit(ctx, room, -1);
}

void GetItem(Ctx &ctx) {
	Room room;
	const RoomViewHandler *vh = nullptr;
	if (!ViewRoom(ctx, room, &vh)) {
		return;
	}
	if (!vh->item) {
		NotFound(ctx);
		return;
	}
	vh->item(ctx, room, CapNum(ctx, 1));
}

void GetItemEdit(Ctx &ctx) {
	Room room;
	const RoomViewHandler *vh = nullptr;
	if (!ViewRoom(ctx, room, &vh)) {
		return;
	}
	if (!vh->edit) {
		NotFound(ctx);
		return;
	}
	vh->edit(ctx, room, CapNum(ctx, 1));
}

void PostItemSave(Ctx &ctx) {
	Room room;
	const RoomViewHandler *vh = nullptr;
	if (!ViewRoom(ctx, room, &vh)) {
		return;
	}
	if (!vh->save) {
		NotFound(ctx);
		return;
	}
	vh->save(ctx, room);
}

void PostItemDelete(Ctx &ctx) {
	Room room;
	const RoomViewHandler *vh = nullptr;
	if (!ViewRoom(ctx, room, &vh)) {
		return;
	}
	if (!vh->remove) {
		NotFound(ctx);
		return;
	}
	vh->remove(ctx, room);
}

// Only the tasks view has this, so it checks the view rather than dispatching
// through a slot that would be null everywhere else.
void PostItemComplete(Ctx &ctx) {
	Room room;
	const RoomViewHandler *vh = nullptr;
	if (!ViewRoom(ctx, room, &vh)) {
		return;
	}
	if (vh->view != quackmail::citadel::VIEW_TASKS) {
		NotFound(ctx);
		return;
	}
	TasksComplete(ctx, room);
}

} // namespace

void RegisterViewRoutes(std::vector<Route> &out) {
	// Order matters: the table is scanned linearly and ":m" would happily match
	// the literal segments below, so every literal form has to come first.
	out.push_back({"GET", "/bbs/room/:n/item/new", Role::User, GetItemNew});
	out.push_back({"POST", "/bbs/room/:n/item/save", Role::User, PostItemSave});
	out.push_back({"POST", "/bbs/room/:n/item/delete", Role::User, PostItemDelete});
	out.push_back({"POST", "/bbs/room/:n/item/complete", Role::User, PostItemComplete});
	out.push_back({"GET", "/bbs/room/:n/item/:m/edit", Role::User, GetItemEdit});
	out.push_back({"GET", "/bbs/room/:n/item/:m", Role::User, GetItem});
}

} // namespace qmweb
} // namespace duckdb
