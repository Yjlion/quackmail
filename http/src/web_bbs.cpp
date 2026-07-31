#include "web.hpp"
#include "web_views.hpp"

#include "quackmail/citadel_msg.hpp"
#include "quackmail/mime.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Floor;
using quackmail::citadel::Message;
using quackmail::citadel::Room;
using quackmail::citadel::RoomStats;

namespace {

int64_t CapNum(const Ctx &ctx, size_t i) {
	std::string s = ctx.Cap(i);
	return s.empty() ? -1 : (int64_t)std::strtoll(s.c_str(), nullptr, 10);
}

// Shell options for any page that belongs to a room. Carrying default_view onto
// the <body> is what will let a room render as something other than a message
// list without every handler having to know about layout.
PageOpts RoomPage(const Room &room) {
	PageOpts opts;
	opts.active = "bbs";
	opts.view = (int)room.default_view;
	opts.wide = true;
	return opts;
}

// The one-line badge after a room name: unread count, and why it is unusual.
std::string RoomBadges(const Room &room, const RoomStats &stats) {
	std::string out;
	if (stats.new_count > 0) {
		// "3 new of 10" only reads correctly when there is something new;
		// with nothing unread it has to stand on its own.
		out += " <strong>" + std::to_string(stats.new_count) + " new</strong>";
		out += " <span class=\"muted\">of " + std::to_string(stats.total) + "</span>";
	} else if (stats.total == 0) {
		out += " <span class=\"muted\">empty</span>";
	} else {
		out += " <span class=\"muted\">" + std::to_string(stats.total) +
		       (stats.total == 1 ? " message" : " messages") + "</span>";
	}
	std::vector<std::string> tags;
	if (room.qr_flags & quackmail::citadel::QR_READONLY) {
		tags.push_back("read-only");
	}
	if (room.qr_flags & quackmail::citadel::QR_PASSWORDED) {
		tags.push_back("password");
	}
	if (room.qr_flags & quackmail::citadel::QR_PRIVATE) {
		tags.push_back("private");
	}
	if (room.qr_flags & quackmail::citadel::QR_DIRECTORY) {
		tags.push_back("directory");
	}
	if (!tags.empty()) {
		std::string joined;
		for (size_t i = 0; i < tags.size(); i++) {
			joined += (i ? ", " : "") + tags[i];
		}
		out += " <span class=\"muted\">(" + T(joined) + ")</span>";
	}
	return out;
}

std::string RoomListHtml(Ctx &ctx, const std::vector<Room> &rooms) {
	if (rooms.empty()) {
		return "<p class=\"muted\">No rooms here.</p>";
	}
	std::vector<int64_t> nums;
	nums.reserve(rooms.size());
	for (auto &r : rooms) {
		nums.push_back(r.room_num);
	}
	// One query for the whole list; GetRoomStats per room would be O(4N).
	auto stats = quackmail::citadel::RoomStatsBulk(ctx.con, ctx.username, nums);

	std::string out = "<div class=\"roomgrid\">";
	for (size_t i = 0; i < rooms.size(); i++) {
		out += "<div>" + Link(RoomHref(rooms[i]), rooms[i].display_name) +
		       RawHtml(RoomBadges(rooms[i], stats[i])) + "</div>";
	}
	return out + "</div>";
}

void GetBbsIndex(Ctx &ctx) {
	auto floors = quackmail::citadel::ListFloors(ctx.con);
	std::string body;
	for (auto &floor : floors) {
		auto rooms = quackmail::citadel::ListRooms(ctx.con, ctx.username, floor.floor_num, "all");
		if (rooms.empty()) {
			continue;
		}
		body += "<h2>" + T(floor.name) + "</h2>";
		body += RoomListHtml(ctx, rooms);
	}
	if (body.empty()) {
		body = "<p class=\"muted\">You have no rooms. They may all be forgotten.</p>";
	}
	auto zapped = quackmail::citadel::ListRooms(ctx.con, ctx.username, -1, "zapped");
	if (!zapped.empty()) {
		body += "<h2>Forgotten rooms</h2>";
		body += "<p class=\"muted\">Hidden from the lists above until you open one again.</p>";
		body += RoomListHtml(ctx, zapped);
	}
	PageOpts opts;
	opts.active = "bbs";
	opts.wide = true;
	Render(ctx, "Rooms", body, opts);
}

void GetBbsFloor(Ctx &ctx) {
	Floor floor;
	if (!quackmail::citadel::GetFloor(ctx.con, CapNum(ctx, 0), floor)) {
		NotFound(ctx);
		return;
	}
	auto rooms = quackmail::citadel::ListRooms(ctx.con, ctx.username, floor.floor_num, "all");
	PageOpts opts;
	opts.active = "bbs";
	opts.wide = true;
	Render(ctx, floor.name, RoomListHtml(ctx, rooms), opts);
}

// ---- one room ------------------------------------------------------------

constexpr int64_t kDefaultPageSize = 30;

std::string PagerHtml(const Room &room, const std::string &filter, int64_t page, int64_t per,
                      int64_t total_pages) {
	if (total_pages <= 1) {
		return std::string();
	}
	auto href = [&](int64_t p) {
		return RoomHref(room) + "?f=" + filter + "&p=" + std::to_string(p) + "&n=" + std::to_string(per);
	};
	std::string out = "<div class=\"pager\">";
	if (page > 1) {
		out += Link(href(page - 1), "Newer");
	}
	out += "<span class=\"muted\">Page " + std::to_string(page) + " of " + std::to_string(total_pages) +
	       "</span>";
	if (page < total_pages) {
		out += Link(href(page + 1), "Older");
	}
	return out + "</div>";
}

void GetBbsRoom(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, CapNum(ctx, 0), room)) {
		NotFound(ctx);
		return;
	}
	if (!RequireUnlocked(ctx, room, RoomHref(room))) {
		return;
	}
	// Opening a forgotten room restores it, which is what Citadel does.
	if (quackmail::citadel::IsZapped(ctx.con, ctx.username, room.room_num)) {
		std::string err;
		quackmail::citadel::ZapRoom(ctx.con, ctx.username, room.room_num, false, err);
	}

	// A room whose view has its own renderer uses it. `?view=raw` is a
	// deliberate escape hatch back to the message list — you want it the first
	// time a Contacts room turns out to hold something that is not a vCard.
	if (ctx.req.Param("view") != "raw") {
		const RoomViewHandler &vh = ViewFor((int)room.default_view);
		if (vh.index) {
			vh.index(ctx, room);
			return;
		}
	}

	std::string filter = ctx.req.Param("f");
	if (filter != "new" && filter != "old" && filter != "all") {
		filter = "all";
	}
	int64_t per = std::min<int64_t>(std::max<int64_t>(ctx.ParamInt("n", kDefaultPageSize), 5), 200);
	int64_t page = std::max<int64_t>(ctx.ParamInt("p", 1), 1);

	auto stats = quackmail::citadel::GetRoomStats(ctx.con, ctx.username, room.room_num);
	auto nums = quackmail::citadel::RoomMessages(ctx.con, room.room_num, filter, 0, stats.last_read);
	// Newest first on screen, which is what a web reader expects even though
	// the store hands them back ascending.
	std::reverse(nums.begin(), nums.end());

	int64_t total_pages = nums.empty() ? 1 : (int64_t)((nums.size() + (size_t)per - 1) / (size_t)per);
	if (page > total_pages) {
		page = total_pages;
	}
	size_t begin = (size_t)((page - 1) * per);
	size_t end = std::min(nums.size(), begin + (size_t)per);

	std::string body = "<div class=\"actions\">";
	body += Link(RoomHref(room) + "?f=new", "Unread", "btn sec");
	body += Link(RoomHref(room) + "?f=all", "All", "btn sec");
	body += Link(RoomHref(room) + "?f=old", "Already read", "btn sec");
	if (!(room.qr_flags & quackmail::citadel::QR_READONLY)) {
		body += Link(RoomHref(room, "/compose"), "Post a message", "btn sec");
	}
	body += FormStart(ctx, RoomHref(room, "/markread"), "inline") + Button("Mark all read", "sec") +
	        FormEnd();
	if (room.room_num != quackmail::citadel::kLobbyRoom && room.mailbox_owner == 0) {
		body += FormStart(ctx, RoomHref(room, "/zap"), "inline") + Button("Forget this room", "sec") +
		        FormEnd();
	}
	body += "</div>";

	if (!room.info.empty()) {
		body += "<p class=\"muted\">" + T(room.info) + "</p>";
	}
	body += "<p class=\"muted\">" + std::to_string(stats.new_count) + " unread of " +
	        std::to_string(stats.total) + ".</p>";

	if (nums.empty()) {
		body += "<p class=\"muted\">Nothing to show.</p>";
		Render(ctx, room.display_name, body, RoomPage(room));
		return;
	}

	body += "<div class=\"wrap\"><table><tr>" + Head("Subject") + Head("From") + Head("Date") + "</tr>";
	for (size_t i = begin; i < end; i++) {
		Message msg;
		if (!quackmail::citadel::LoadMessage(ctx.con, nums[i], msg)) {
			continue;
		}
		bool unread = nums[i] > stats.last_read;
		std::string subject = DecodeHeader(msg.subject);
		if (subject.empty()) {
			subject = "(no subject)";
		}
		body += std::string("<tr") + (unread ? " class=\"unread\"" : "") + ">";
		body += "<td>" + Link(RoomHref(room, "/msg/" + std::to_string(nums[i])), subject) + "</td>";
		body += Cell(DecodeHeader(msg.author));
		body += Cell(FormatTime(msg.msgtime));
		body += "</tr>";
	}
	body += "</table></div>";
	body += PagerHtml(room, filter, page, per, total_pages);

	Render(ctx, room.display_name, body, RoomPage(room));
}

void GetBbsMessage(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, CapNum(ctx, 0), room)) {
		NotFound(ctx);
		return;
	}
	if (!RequireUnlocked(ctx, room, RoomHref(room))) {
		return;
	}
	Message msg;
	if (!LoadMessageIn(ctx, room, CapNum(ctx, 1), msg)) {
		NotFound(ctx);
		return;
	}

	std::string body = "<p>" + Link(RoomHref(room), "Back to " + room.display_name) + "</p>";
	body += RenderMessage(ctx, room, msg);

	std::string num = std::to_string(msg.msgnum);
	body += "<div class=\"actions\">";
	if (room.mailbox_owner > 0) {
		// A personal mail folder: mail actions, which go through the submission
		// path rather than posting into the room.
		std::string q = "?room=" + std::to_string(room.room_num) + "&";
		body += Link("/mail/compose" + q + "reply=" + num, "Reply", "btn sec");
		body += Link("/mail/compose" + q + "reply=" + num + "&all=1", "Reply all", "btn sec");
		body += Link("/mail/compose" + q + "forward=" + num, "Forward", "btn sec");
		body += FormStart(ctx, "/mail/delete", "inline") + Hidden("room", std::to_string(room.room_num)) +
		        Hidden("msgnum", num) +
		        Button(room.display_name == "Trash" ? "Delete permanently" : "Move to Trash", "danger") +
		        FormEnd();
	} else {
		if (!(room.qr_flags & quackmail::citadel::QR_READONLY)) {
			body += Link(RoomHref(room, "/compose?reply=" + num), "Reply", "btn sec");
		}
		body += FormStart(ctx, RoomHref(room, "/delete"), "inline") + Hidden("msgnum", num) +
		        Button("Delete", "danger") + FormEnd();
	}
	body += Link(RoomHref(room, "/msg/" + num + "/source"), "View source", "btn sec");
	body += "</div>";

	// Reading advances the pointer, but never backwards.
	auto stats = quackmail::citadel::GetRoomStats(ctx.con, ctx.username, room.room_num);
	if (msg.msgnum > stats.last_read) {
		quackmail::citadel::SetLastRead(ctx.con, ctx.username, room.room_num, msg.msgnum);
	}

	std::string subject = DecodeHeader(msg.subject);
	Render(ctx, subject.empty() ? std::string("(no subject)") : subject, body, RoomPage(room));
}

void GetBbsCompose(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, CapNum(ctx, 0), room)) {
		NotFound(ctx);
		return;
	}
	if (!RequireUnlocked(ctx, room, RoomHref(room))) {
		return;
	}
	if (!quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "This room does not accept new messages from you.");
		return;
	}

	std::string node = ConfigStr(ctx.con, "c_nodename", "quackcit");
	std::string subject, refs, quoted;
	int64_t reply_to = ctx.ParamInt("reply", 0);
	if (reply_to > 0) {
		Message orig;
		if (LoadMessageIn(ctx, room, reply_to, orig)) {
			subject = DecodeHeader(orig.subject);
			if (subject.rfind("Re: ", 0) != 0) {
				subject = "Re: " + subject;
			}
			std::string id = quackmail::citadel::MessageId(orig, node);
			refs = orig.references.empty() ? id : orig.references + " " + id;
			quoted = DecodeHeader(orig.author) + " wrote:\n";
			std::string text = quackmail::citadel::BodyText(orig);
			size_t pos = 0;
			while (pos < text.size()) {
				size_t nl = text.find('\n', pos);
				quoted += "> " + text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
				quoted += "\n";
				if (nl == std::string::npos) {
					break;
				}
				pos = nl + 1;
			}
		}
	}

	std::string body = FormStart(ctx, RoomHref(room, "/post"));
	body += Hidden("refs", refs);
	body += "<label class=\"field\"><span>Subject</span>" + TextInput("subject", subject) + "</label>";
	body += "<label class=\"field\"><span>Message</span>" + TextArea("body", quoted, 16) + "</label>";
	body += "<p>" + Button("Post") + " " + Link(RoomHref(room), "Cancel") + "</p>";
	body += FormEnd();
	Render(ctx, "Post to " + room.display_name, body, RoomPage(room));
}

void PostBbsPost(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, CapNum(ctx, 0), room)) {
		NotFound(ctx);
		return;
	}
	if (!quackmail::citadel::RoomUnlocked(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "This room is password protected.");
		return;
	}
	if (room.qr_flags & quackmail::citadel::QR_READONLY) {
		Forbidden(ctx, "This room does not accept new messages.");
		return;
	}
	std::string text = ctx.req.Form("body");
	if (text.empty()) {
		BadRequest(ctx, "A message needs a body.");
		return;
	}

	Message msg;
	msg.author = ctx.username;
	msg.author_usernum = quackmail::citadel::GetOrAssignUserNum(ctx.con, ctx.username);
	msg.msgtime = (int64_t)std::time(nullptr);
	// format_type 0 is a native Citadel message, so a post made here reads back
	// over the Citadel protocol, telnet, NNTP, IMAP and POP3 unchanged.
	msg.format_type = 0;
	msg.subject = ctx.req.Form("subject");
	msg.references = ctx.req.Form("refs");
	msg.origin_room = room.display_name;
	msg.node = ConfigStr(ctx.con, "c_nodename", "quackcit");
	msg.raw = text;

	std::string err;
	std::vector<int64_t> rooms = {room.room_num};
	if (quackmail::citadel::InsertMessage(ctx.con, msg, rooms, err) < 0) {
		ErrorPage(ctx, 500, "Could not post", err);
		return;
	}
	RedirectTo(ctx, RoomHref(room), "posted");
}

void PostBbsMarkRead(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, CapNum(ctx, 0), room)) {
		NotFound(ctx);
		return;
	}
	auto stats = quackmail::citadel::GetRoomStats(ctx.con, ctx.username, room.room_num);
	quackmail::citadel::SetLastRead(ctx.con, ctx.username, room.room_num, stats.highest);
	RedirectTo(ctx, RoomHref(room), "marked");
}

void PostBbsZap(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, CapNum(ctx, 0), room)) {
		NotFound(ctx);
		return;
	}
	std::string err;
	if (!quackmail::citadel::ZapRoom(ctx.con, ctx.username, room.room_num, true, err)) {
		Forbidden(ctx, err);
		return;
	}
	RedirectTo(ctx, "/bbs/", "zapped");
}

void PostBbsUnzap(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, CapNum(ctx, 0), room)) {
		NotFound(ctx);
		return;
	}
	std::string err;
	quackmail::citadel::ZapRoom(ctx.con, ctx.username, room.room_num, false, err);
	RedirectTo(ctx, RoomHref(room), "unzapped");
}

void PostBbsUnlock(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, CapNum(ctx, 0), room)) {
		NotFound(ctx);
		return;
	}
	if (!quackmail::citadel::UnlockRoom(ctx.con, ctx.username, room, ctx.req.Form("password"))) {
		Forbidden(ctx, "That is not the password for this room.");
		return;
	}
	std::string next = ctx.req.Form("next");
	RedirectTo(ctx, http::IsSafeRedirectTarget(next) ? next : RoomHref(room));
}

void PostBbsDelete(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, CapNum(ctx, 0), room)) {
		NotFound(ctx);
		return;
	}
	Message msg;
	int64_t msgnum = ctx.FormInt("msgnum", 0);
	if (!LoadMessageIn(ctx, room, msgnum, msg)) {
		NotFound(ctx);
		return;
	}
	// Your own mailbox is yours; a shared room is the author's or an aide's.
	if (room.mailbox_owner == 0 && !ctx.IsAide() && msg.author != ctx.username) {
		Forbidden(ctx, "You can only delete your own messages here.");
		return;
	}
	std::string err;
	if (!quackmail::citadel::DeleteMessage(ctx.con, room.room_num, msgnum, err)) {
		ErrorPage(ctx, 500, "Could not delete", err);
		return;
	}
	RedirectTo(ctx, RoomHref(room), "deleted");
}

// ---- presence ------------------------------------------------------------

void GetWho(Ctx &ctx) {
	auto sessions = quackmail::citadel::ListSessions(ctx.con);
	std::string body = "<div class=\"wrap\"><table><tr>" + Head("User") + Head("Room") + Head("From") +
	                   Head("Client") + Head("Doing") + "</tr>";
	for (auto &s : sessions) {
		body += "<tr>";
		body += Cell(s.username.empty() ? "(signing in)" : s.username);
		body += Cell(s.room);
		body += Cell(s.host);
		body += Cell(s.client);
		body += Cell(s.last_cmd);
		body += "</tr>";
	}
	body += "</table></div>";

	body += "<h2>Send an instant message</h2>";
	body += FormStart(ctx, "/bbs/page");
	body += "<label class=\"field\"><span>To</span>" + TextInput("to", "") + "</label>";
	body += "<label class=\"field\"><span>Message</span>" + TextInput("text", "") + "</label>";
	body += "<p>" + Button("Send") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">Instant messages reach Citadel, telnet and XMPP clients alike — they all "
	        "read the same queue.</p>";

	PageOpts opts;
	opts.active = "who";
	Render(ctx, "Who is online", body, opts);
}

void PostPage(Ctx &ctx) {
	std::string to = ctx.req.Form("to");
	std::string text = ctx.req.Form("text");
	if (to.empty() || text.empty()) {
		BadRequest(ctx, "Both a recipient and a message are needed.");
		return;
	}
	if (!quackmail::citadel::SendExpress(ctx.con, to, ctx.username, text)) {
		ErrorPage(ctx, 404, "No such user", "There is no local user by that name.");
		return;
	}
	RedirectTo(ctx, "/bbs/who", "paged");
}

} // namespace

std::string RoomHref(const Room &room, const std::string &suffix) {
	// Rooms are addressed by number, never by name: a Citadel room name may
	// contain a '/', which no amount of percent-encoding survives once the
	// path has been split into segments.
	return "/bbs/room/" + std::to_string(room.room_num) + suffix;
}

std::string RenderMessage(Ctx &ctx, const Room &room, const Message &msg) {
	std::string out = "<div class=\"msghead\"><dl>";
	auto row = [&out](const char *label, const std::string &value) {
		if (!value.empty()) {
			out += "<dt>" + T(label) + "</dt><dd>" + T(value) + "</dd>";
		}
	};
	row("Subject", DecodeHeader(msg.subject));
	row("From", DecodeHeader(msg.author));
	row("To", DecodeHeader(msg.recipient));
	row("Date", FormatTime(msg.msgtime));
	out += "</dl></div>";

	// format_type 4 is RFC822/MIME; anything else is native Citadel text.
	if (msg.format_type != 4) {
		out += "<pre class=\"body\">" + T(quackmail::citadel::BodyText(msg)) + "</pre>";
		return out;
	}

	auto entity = quackmail::mime::ParseEntity(msg.raw);
	auto parts = quackmail::mime::FlattenParts(entity);
	const quackmail::mime::MimePart *text = nullptr;
	const quackmail::mime::MimePart *html = nullptr;
	for (auto &p : parts) {
		if (!p.filename.empty()) {
			continue; // an attachment, not the message body
		}
		if (p.content_type == "text/plain" && !text) {
			text = &p;
		} else if (p.content_type == "text/html" && !html) {
			html = &p;
		}
	}
	if (text) {
		out += "<pre class=\"body\">" + T(text->content) + "</pre>";
	}
	if (html) {
		// Served from its own route into a sandboxed frame, so the sender's
		// markup gets an opaque origin and its own restrictive policy instead
		// of running inside this page. See the /html handler in web_mail.cpp.
		out += "<p class=\"muted\">HTML version" + std::string(text ? " (the plain text above is the same "
		                                                             "message)"
		                                                           : "") +
		       ":</p>";
		out += "<iframe class=\"htmlpart\" sandbox src=\"" +
		       A(RoomHref(room, "/msg/" + std::to_string(msg.msgnum) + "/html")) + "\"></iframe>";
	}
	if (!text && !html) {
		out += "<pre class=\"body\">" + T(quackmail::citadel::BodyText(msg)) + "</pre>";
	}

	std::string attach;
	for (auto &p : parts) {
		if (p.content_type.rfind("multipart/", 0) == 0) {
			continue;
		}
		if (p.filename.empty() && (p.content_type == "text/plain" || p.content_type == "text/html")) {
			continue;
		}
		std::string label = p.filename.empty() ? p.content_type : p.filename;
		attach += "<li>" +
		          Link(RoomHref(room, "/msg/" + std::to_string(msg.msgnum) + "/part/" + p.section), label) +
		          " <span class=\"muted\">" + T(p.content_type) + ", " + T(FormatBytes(p.size_bytes)) +
		          "</span></li>";
	}
	if (!attach.empty()) {
		out += "<h2>Attachments</h2><ul>" + RawHtml(attach) + "</ul>";
	}
	return out;
}

void RegisterBbsRoutes(std::vector<Route> &out) {
	out.push_back({"GET", "/bbs/", Role::User, GetBbsIndex});
	out.push_back({"GET", "/bbs/floor/:n", Role::User, GetBbsFloor});
	out.push_back({"GET", "/bbs/who", Role::User, GetWho});
	out.push_back({"POST", "/bbs/page", Role::User, PostPage});
	out.push_back({"GET", "/bbs/room/:n", Role::User, GetBbsRoom});
	out.push_back({"GET", "/bbs/room/:n/compose", Role::User, GetBbsCompose});
	out.push_back({"GET", "/bbs/room/:n/msg/:m", Role::User, GetBbsMessage});
	out.push_back({"POST", "/bbs/room/:n/post", Role::User, PostBbsPost});
	out.push_back({"POST", "/bbs/room/:n/markread", Role::User, PostBbsMarkRead});
	out.push_back({"POST", "/bbs/room/:n/zap", Role::User, PostBbsZap});
	out.push_back({"POST", "/bbs/room/:n/unzap", Role::User, PostBbsUnzap});
	out.push_back({"POST", "/bbs/room/:n/unlock", Role::User, PostBbsUnlock});
	out.push_back({"POST", "/bbs/room/:n/delete", Role::User, PostBbsDelete});
}

} // namespace qmweb
} // namespace duckdb
