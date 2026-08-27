#include "web.hpp"
#include "web_i18n.hpp"
#include "web_views.hpp"

#include "quackmail/citadel_msg.hpp"
#include "quackmail/html_sanitize.hpp"
#include "quackmail/mime.hpp"

#include <algorithm>
#include <cctype>
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

// Blog and Journal entries are the only ordinary (format_type 0) messages
// offered a rich-text choice. A plain message-board room's posts still come
// from whatever the sender's client sent — this is not a general "compose in
// HTML" feature for every room, only for the two views that read like
// articles rather than a conversation.
bool IsRichRoom(const Room &room) {
	return room.default_view == quackmail::citadel::VIEW_BLOG ||
	      room.default_view == quackmail::citadel::VIEW_JOURNAL;
}

// Shell options for any page that belongs to a room. Carrying default_view onto
// the <body> is what will let a room render as something other than a message
// list without every handler having to know about layout.
PageOpts RoomPage(const Room &room) {
	PageOpts opts;
	// By number, so the sidebar highlights this room when it lists it and falls
	// back to "All rooms" when it does not.
	opts.active = "room:" + std::to_string(room.room_num);
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
	body += Link("/search?room=" + std::to_string(room.room_num), "Search this room", "btn sec");
	body += FormStart(ctx, RoomHref(room, "/markread"), "inline") + Button("Mark all read", "sec") +
	        FormEnd();
	if (room.room_num != quackmail::citadel::kLobbyRoom && room.mailbox_owner == 0) {
		body += FormStart(ctx, RoomHref(room, "/zap"), "inline") + Button("Forget this room", "sec") +
		        FormEnd();
	}
	// Shown to whoever holds the RFC 4314 `a` right — an aide, or anyone an aide
	// has delegated the room to. Personal folders are managed from Mail.
	if (room.mailbox_owner == 0 && quackmail::citadel::CanAdminister(ctx.con, ctx.username, room)) {
		body += Link(RoomHref(room, "/settings"), "Room settings", "btn sec");
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
		body += Cell(FormatTime(ctx, msg.msgtime));
		body += "</tr>";
	}
	body += "</table></div>";
	body += PagerHtml(room, filter, page, per, total_pages);

	Render(ctx, room.display_name, body, RoomPage(room));
}

// Move between messages without going back through the list.
//
// Three bounded lookups rather than RoomMessages, which returns every message
// number in the room: reading one message should not cost a listing of ten
// thousand. Each is a min/max over the (room_num, msgnum) primary key.
//
// "Unread" is what the room's own listing means by it — \Seen in a mail folder,
// the last-read pointer on a board — so the link agrees with the bold rows the
// reader just came from.
std::string NavHtml(Ctx &ctx, const Room &room, const Message &msg, const RoomStats &stats) {
	auto scalar = [&](const std::string &sql, vector<Value> params) -> int64_t {
		auto r = Exec(ctx.con, sql, std::move(params));
		if (!r) {
			return 0;
		}
		auto &mat = r->Cast<MaterializedQueryResult>();
		if (mat.RowCount() < 1) {
			return 0;
		}
		Value v = mat.GetValue(0, 0);
		return v.IsNull() ? 0 : v.GetValue<int64_t>();
	};

	int64_t room_num = room.room_num;
	int64_t newer = scalar("SELECT min(msgnum) FROM citadel_room_msgs WHERE room_num = $1 AND msgnum > $2",
	                       {Value::BIGINT(room_num), Value::BIGINT(msg.msgnum)});
	int64_t older = scalar("SELECT max(msgnum) FROM citadel_room_msgs WHERE room_num = $1 AND msgnum < $2",
	                       {Value::BIGINT(room_num), Value::BIGINT(msg.msgnum)});
	int64_t unread = 0;
	if (room.mailbox_owner > 0) {
		unread = scalar("SELECT min(m.msgnum) FROM citadel_room_msgs m WHERE m.room_num = $1 "
		                "AND m.msgnum > $2 AND NOT EXISTS (SELECT 1 FROM citadel_msg_flags f "
		                "WHERE f.msgnum = m.msgnum AND f.username = $3 AND f.flag = $4)",
		                {Value::BIGINT(room_num), Value::BIGINT(msg.msgnum), Value(ctx.username),
		                 Value("\\Seen")});
	} else {
		unread = scalar("SELECT min(msgnum) FROM citadel_room_msgs WHERE room_num = $1 AND msgnum > $2",
		                {Value::BIGINT(room_num), Value::BIGINT(stats.last_read)});
	}

	// Newest-first is the order the listing is in, so "Newer" is the higher
	// message number and belongs on the left, beside the pager's own wording.
	std::string out = "<div class=\"pager msgnav\">";
	if (newer > 0) {
		out += Link(RoomHref(room, "/msg/" + std::to_string(newer)), "Newer");
	}
	if (older > 0) {
		out += Link(RoomHref(room, "/msg/" + std::to_string(older)), "Older");
	}
	// Suppressed when it would point where "Newer" already does: two links to
	// one message is noise.
	if (unread > 0 && unread != newer) {
		out += Link(RoomHref(room, "/msg/" + std::to_string(unread)), "Next unread");
	}
	if (newer == 0 && older == 0) {
		return std::string(); // the only message in the room
	}
	return out + "</div>";
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

	// Reading advances the pointer, but never backwards. Done before the page is
	// built, so the "next unread" link below is computed against the state this
	// read leaves behind rather than the one it found.
	auto stats = quackmail::citadel::GetRoomStats(ctx.con, ctx.username, room.room_num);
	if (msg.msgnum > stats.last_read) {
		quackmail::citadel::SetLastRead(ctx.con, ctx.username, room.room_num, msg.msgnum);
		stats.last_read = msg.msgnum;
	}
	MarkSeen(ctx, room, msg.msgnum);

	std::string body = RawHtml(NavHtml(ctx, room, msg, stats));
	body += RenderMessage(ctx, room, msg);


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
	if (IsRichRoom(room)) {
		body += "<label class=\"field\"><span>Format</span>" +
		        Select("format", {{"", "Plain text"},
		                          {kHtmlContentType, "Formatted text (HTML)"},
		                          {kMarkdownContentType, "Markdown"}},
		               "") +
		        "</label>";
	}
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
	msg.subject = ctx.req.Form("subject");
	msg.references = ctx.req.Form("refs");
	msg.origin_room = room.display_name;
	msg.node = ConfigStr(ctx.con, "c_nodename", "quackcit");

	// Only Blog/Journal offer a format at all (see IsRichRoom), so every other
	// room's posts keep coming through exactly as they do today: format_type 0,
	// a native Citadel message that reads back over the Citadel protocol,
	// telnet, NNTP, IMAP and POP3 unchanged.
	std::string format = IsRichRoom(room) ? ctx.req.Form("format") : std::string();
	if (format == kMarkdownContentType || format == kHtmlContentType) {
		// Rendered (and, for Markdown, sanitized as part of rendering) *before*
		// storage: there is no edit path for a blog/journal entry to come back
		// through, so keeping the source around would serve no reader and
		// storing unsanitized HTML would not either.
		std::string html = format == kMarkdownContentType
		                       ? RenderFormattedBody(text, kMarkdownContentType)
		                       : quackmail::html::SanitizeForCompose(text);
		msg.format_type = 4;
		msg.raw = WrapObject(ctx, kHtmlContentType, html, msg.subject, "");
	} else {
		msg.format_type = 0;
		msg.raw = text;
	}

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
	// A mail folder counts unread by \Seen, so moving the pointer alone would
	// leave every row bold and the sidebar count untouched — the button would
	// look broken in the one place it is most used.
	if (room.mailbox_owner > 0) {
		Exec(ctx.con,
		     "INSERT INTO citadel_msg_flags (msgnum, username, flag) "
		     "SELECT rm.msgnum, $1, $2 FROM citadel_room_msgs rm WHERE rm.room_num = $3 "
		     "AND NOT EXISTS (SELECT 1 FROM citadel_msg_flags f WHERE f.msgnum = rm.msgnum "
		     "AND f.username = $1 AND f.flag = $2)",
		     {Value(ctx.username), Value("\\Seen"), Value::BIGINT(room.room_num)});
	}
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

// "Ada Lovelace <ada@example>" -> "Ada Lovelace"; a bare address is its own
// display name. Purely presentational — nothing downstream parses these back.
static std::string DisplayNameOf(const std::string &from) {
	size_t lt = from.find('<');
	if (lt == std::string::npos) {
		return from;
	}
	std::string name = from.substr(0, lt);
	while (!name.empty() && (name.back() == ' ' || name.back() == '"')) {
		name.pop_back();
	}
	size_t i = 0;
	while (i < name.size() && (name[i] == ' ' || name[i] == '"')) {
		i++;
	}
	return name.empty() ? from : name.substr(i);
}

// The same string's address half, or "" when there is no angle-bracket form.
static std::string AddressOf(const std::string &from) {
	size_t lt = from.find('<');
	size_t gt = from.rfind('>');
	if (lt == std::string::npos || gt == std::string::npos || gt < lt) {
		return std::string();
	}
	return from.substr(lt + 1, gt - lt - 1);
}

// The monogram for the avatar block: the first character of the display name,
// upper-cased. Deliberately one *byte* only when it is ASCII — taking one byte
// out of a UTF-8 name would emit a broken sequence, so anything non-ASCII falls
// back to a placeholder rather than to mojibake.
static std::string Initial(const std::string &from) {
	std::string name = DisplayNameOf(from);
	if (name.empty()) {
		return "?";
	}
	unsigned char c = (unsigned char)name[0];
	if (c < 0x80) {
		return std::string(1, (char)toupper(c));
	}
	// A multi-byte first character: take the whole sequence, uppercased by the
	// font rather than by us.
	size_t len = 1;
	while (len < name.size() && ((unsigned char)name[len] & 0xC0) == 0x80) {
		len++;
	}
	return name.substr(0, len);
}

// In a mail folder, reading a message also sets \Seen. The folder listing shows
// that rather than the Citadel last-read pointer — a pointer is a high-water
// mark and cannot say "this one, not that one" — and it is the same flag IMAP
// shares, so opening a message here marks it read in a desktop client too.
//
// A no-op outside a mailbox: a message board has only the last-read pointer,
// and giving its messages per-user flags would make the two disagree.
void MarkSeen(Ctx &ctx, const Room &room, int64_t msgnum) {
	if (room.mailbox_owner <= 0) {
		return;
	}
	Exec(ctx.con,
	     "INSERT INTO citadel_msg_flags (msgnum, username, flag) "
	     "SELECT $1, $2, $3 WHERE NOT EXISTS (SELECT 1 FROM citadel_msg_flags "
	     "WHERE msgnum = $1 AND username = $2 AND flag = $3)",
	     {Value::BIGINT(msgnum), Value(ctx.username), Value("\\Seen")});
}

// The action bar for one message. Part of RenderMessage rather than of the page
// around it, because the reading pane renders a message without a page around
// it and must not end up with a different set of controls.
//
// The data-key attributes are what qc.js binds the keyboard shortcuts to: the
// shortcut clicks the control that is already there rather than duplicating
// what it does, so the two can never drift.
static std::string MessageActions(Ctx &ctx, const Room &room, const Message &msg) {
	std::string num = std::to_string(msg.msgnum);
	std::string out = "<div class=\"toolbar msgactions\">";

	// On a phone the list is hidden while the reader is open, so the way back
	// has to be on the reader itself.
	out += Link(RoomHref(room), Tr(ctx, "msg.back"), "btn sec backtolist hidewide");

	if (room.mailbox_owner > 0) {
		// A personal mail folder: mail actions, which go through the submission
		// path rather than posting into the room.
		std::string q = "?room=" + std::to_string(room.room_num) + "&";
		out += ButtonGroup(
		    "<a role=\"button\" class=\"secondary\" data-key=\"reply\" href=\"" +
		    A("/mail/compose" + q + "reply=" + num) + "\">" + Icon("reply") + T(Tr(ctx, "msg.reply")) +
		    "</a>"
		    "<a role=\"button\" class=\"secondary\" data-key=\"replyall\" href=\"" +
		    A("/mail/compose" + q + "reply=" + num + "&all=1") + "\">" + Icon("replyall") +
		    T(Tr(ctx, "msg.reply_all")) + "</a>"
		    "<a role=\"button\" class=\"secondary\" data-key=\"forward\" href=\"" +
		    A("/mail/compose" + q + "forward=" + num) + "\">" + Icon("forward") +
		    T(Tr(ctx, "msg.forward")) + "</a>");

		// Which way the flag button toggles depends on where the flag is now.
		bool flagged = false;
		if (auto fr = Exec(ctx.con,
		                   "SELECT 1 FROM citadel_msg_flags WHERE msgnum = $1 AND username = $2 "
		                   "AND flag = $3",
		                   {Value::BIGINT(msg.msgnum), Value(ctx.username), Value("\\Flagged")})) {
			flagged = fr->Cast<MaterializedQueryResult>().RowCount() > 0;
		}
		std::string here = RoomHref(room, "/msg/" + num);
		out += FormStart(ctx, "/mail/flag", "inline") + Hidden("room", std::to_string(room.room_num)) +
		       Hidden("msgnum", num) + Hidden("back", here) +
		       "<button class=\"secondary\" data-key=\"flag\" name=\"set\" value=\"" +
		       A(flagged ? "unflagged" : "flagged") + "\">" + Icon("flag") +
		       T(Tr(ctx, flagged ? "mailbox.clear_flag" : "msg.flag")) + "</button>" + FormEnd();

		// Filing it somewhere else. The same endpoint the folder listing's bulk
		// move posts to, with one message selected instead of several.
		std::vector<std::pair<std::string, std::string>> folders;
		for (auto &f : MailFolders(ctx)) {
			if (f.room_num != room.room_num) {
				folders.push_back({f.display_name, f.display_name});
			}
		}
		if (!folders.empty()) {
			out += FormStart(ctx, "/mail/move", "inline") +
			       Hidden("room", std::to_string(room.room_num)) + Hidden("msgnum", num) +
			       Select("folder", folders, "") + Button(Tr(ctx, "msg.move"), "sec") + FormEnd();
		}

		out += FormStart(ctx, "/mail/delete", "inline") + Hidden("room", std::to_string(room.room_num)) +
		       Hidden("msgnum", num) +
		       "<button class=\"outline danger\" data-key=\"trash\">" + Icon("trash") +
		       T(Tr(ctx, room.display_name == "Trash" ? "mailbox.delete_forever" : "mailbox.to_trash")) +
		       "</button>" + FormEnd();
	} else {
		if (!(room.qr_flags & quackmail::citadel::QR_READONLY)) {
			out += "<a role=\"button\" class=\"secondary\" data-key=\"reply\" href=\"" +
			       A(RoomHref(room, "/compose?reply=" + num)) + "\">" + Icon("reply") +
			       T(Tr(ctx, "msg.reply")) + "</a>";
		}
		out += FormStart(ctx, RoomHref(room, "/delete"), "inline") + Hidden("msgnum", num) +
		       "<button class=\"outline danger\" data-key=\"trash\">" + Icon("trash") +
		       T(Tr(ctx, "msg.delete")) + "</button>" + FormEnd();
	}
	out += Link(RoomHref(room, "/msg/" + num + "/source"), Tr(ctx, "msg.source"), "btn sec");
	return out + "</div>";
}

std::string RenderMessage(Ctx &ctx, const Room &room, const Message &msg) {
	std::string subject = DecodeHeader(msg.subject);
	std::string author = DecodeHeader(msg.author);
	std::string recipient = DecodeHeader(msg.recipient);

	// The subject leads, the way it does in every mail client — the old
	// four-row definition list buried it as one field among four.
	std::string out;
	if (!subject.empty()) {
		out += "<h2 class=\"subj\">" + T(subject) + "</h2>";
	}

	out += "<div class=\"msghead\">";
	// A monogram rather than a fetched avatar: there is no image to fetch, no
	// third party to ask for one, and it still gives the eye something stable
	// to track down a thread.
	out += "<div class=\"avatar\" aria-hidden=\"true\">" + T(Initial(author)) + "</div>";
	out += "<div class=\"meta\"><div class=\"line1\">";
	out += "<span class=\"who\">" + T(DisplayNameOf(author)) + "</span>";
	std::string addr = AddressOf(author);
	if (!addr.empty() && addr != DisplayNameOf(author)) {
		out += "<span class=\"addr\">" + T(addr) + "</span>";
	}
	out += "<span class=\"when\">" + T(FormatTime(ctx, msg.msgtime)) + "</span>";
	out += "</div>";
	if (!recipient.empty()) {
		out += "<dl><dt>" + T(Tr(ctx, "msg.to")) + "</dt><dd>" + T(recipient) + "</dd></dl>";
	}
	out += "</div></div>";

	// format_type 4 is RFC822/MIME; anything else is native Citadel text.
	if (msg.format_type != 4) {
		out += "<pre class=\"body\">" + T(quackmail::citadel::BodyText(msg)) + "</pre>";
		return MessageActions(ctx, room, msg) + out;
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
		out += "<p class=\"muted\">" + T(Tr(ctx, text ? "msg.html_alt" : "msg.html_only")) + "</p>";
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
		attach += "<a href=\"" +
		          A(RoomHref(room, "/msg/" + std::to_string(msg.msgnum) + "/part/" + p.section)) +
		          "\" download>" + Icon("clip") + "<span>" + T(label) + "</span>" +
		          "<span class=\"size\">" + T(FormatBytes(p.size_bytes)) + "</span></a>";
	}
	if (!attach.empty()) {
		out += "<div class=\"attach\">" + RawHtml(attach) + "</div>";
	}
	return MessageActions(ctx, room, msg) + out;
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
