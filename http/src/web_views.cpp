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
	case quackmail::citadel::VIEW_ADDRESSBOOK:
		return ContactsView();
	default:
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

bool SaveObject(Ctx &ctx, const Room &room, const std::string &euid, const std::string &subject,
                const std::string &content_type, const std::string &body) {
	// The one post predicate. Re-deriving it is how the read-only flag stopped
	// applying to a front-end once before.
	if (!quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "You cannot add anything to this room.");
		return false;
	}
	if (euid.empty()) {
		BadRequest(ctx, "That object has no identifier, so it cannot be saved.");
		return false;
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

	std::string err;
	if (quackmail::citadel::UpsertByEuid(ctx.con, msg, room.room_num, err) < 0) {
		BadRequest(ctx, err.empty() ? "The object could not be saved." : err);
		return false;
	}
	return true;
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

} // namespace

void RegisterViewRoutes(std::vector<Route> &out) {
	// Order matters: the table is scanned linearly and ":m" would happily match
	// the literal segments below, so every literal form has to come first.
	out.push_back({"GET", "/bbs/room/:n/item/new", Role::User, GetItemNew});
	out.push_back({"POST", "/bbs/room/:n/item/save", Role::User, PostItemSave});
	out.push_back({"POST", "/bbs/room/:n/item/delete", Role::User, PostItemDelete});
	out.push_back({"GET", "/bbs/room/:n/item/:m/edit", Role::User, GetItemEdit});
	out.push_back({"GET", "/bbs/room/:n/item/:m", Role::User, GetItem});
}

} // namespace qmweb
} // namespace duckdb
