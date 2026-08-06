#include "web_views.hpp"

#include "quackmail/citadel_msg.hpp"
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
	default:
		// VIEW_BBS, VIEW_WIKI, VIEW_WIKIMD, VIEW_QUEUE. The wikis need
		// versioning and a markdown renderer, and the queue is Citadel's own
		// spool view rather than a user one.
		return kBoardView;
	}
}

bool HasCustomView(int default_view) {
	return ViewFor(default_view).index != nullptr;
}

// ---- shared helpers ------------------------------------------------------

std::string ObjectBody(const Message &msg, const std::string &want_type) {
	// A groupware object is stored as format_type 4 with one part. Anything else
	// in the room is an ordinary message and is not ours to interpret.
	if (msg.format_type != 4) {
		return std::string();
	}
	auto entity = quackmail::mime::ParseEntity(msg.raw);
	for (auto &part : quackmail::mime::FlattenParts(entity)) {
		std::string type = quackmail::util::Lower(part.content_type);
		if (type == want_type) {
			return part.content;
		}
		// Older Citadel writes text/x-vcard; accept it on the way in and emit
		// the registered type on the way out.
		if (want_type == "text/vcard" && type == "text/x-vcard") {
			return part.content;
		}
	}
	return std::string();
}

std::string WrapObject(Ctx &ctx, const std::string &content_type, const std::string &body,
                       const std::string &subject, const std::string &uid) {
	std::string node = ConfigStr(ctx.con, "c_fqdn", "localhost");
	std::string out;
	out += "Content-Type: " + content_type + "; charset=utf-8\r\n";
	out += "MIME-Version: 1.0\r\n";
	out += "Content-Transfer-Encoding: 8bit\r\n";
	out += "From: " + ctx.username + "@" + node + "\r\n";
	out += "Subject: " + subject + "\r\n";
	if (!uid.empty()) {
		// A Message-ID derived from the object's own UID, so the same object
		// keeps the same identity across edits for anything reading over IMAP.
		out += "Message-ID: <" + uid + ">\r\n";
	}
	out += "\r\n";
	out += body;
	return out;
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
