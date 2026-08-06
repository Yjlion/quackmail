#include "web.hpp"
#include "web_views.hpp"

#include "quackmail/citadel_msg.hpp"
#include "quackmail/mime.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>

// The mail folder view.
//
// Until this file existed, VIEW_MAILBOX fell through to the message-board
// renderer: Subject / From / Date, one row per message, and no action anywhere
// on the page. Every folder EnsureUserRooms provisions — Mail, Sent Items,
// Drafts, Trash — is a VIEW_MAILBOX room, so that was *the* webmail listing.
//
// It also left `/mail/move` and `/mail/flag` unreachable. Both handlers were
// written, registered and correct, and nothing rendered a form that posted to
// either, so a message could not be filed or flagged from a browser at all.
// This view is what gives them their forms.
//
// Everything here is a plain form. The bulk actions are one <form> around the
// table with a `formaction` per button — HTML, not script — so selecting three
// messages and deleting them works with JavaScript off, exactly like every
// other page in this module. The only scripted thing is the select-all
// checkbox, which is hidden until qc.js marks the document, because a control
// that did nothing without script would be a lie rather than an enhancement.

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Message;
using quackmail::citadel::Room;

namespace {

constexpr int64_t kDefaultPageSize = 30;

struct MsgFlags {
	bool seen = false;
	bool flagged = false;
};

// \Seen and \Flagged for a whole page of messages, in one query. Asking per row
// would be two scalar queries per message, and this table is the same one IMAP
// reads — a flag set here shows up in a desktop client and the other way round.
std::map<int64_t, MsgFlags> FlagsFor(Ctx &ctx, const std::vector<int64_t> &nums) {
	std::map<int64_t, MsgFlags> out;
	if (nums.empty()) {
		return out;
	}
	std::string list;
	for (auto n : nums) {
		if (!list.empty()) {
			list += ",";
		}
		// Message numbers this request just read out of the store, never
		// anything the client supplied.
		list += std::to_string(n);
	}
	auto r = Exec(ctx.con,
	              "SELECT msgnum, flag FROM citadel_msg_flags WHERE username = $1 AND msgnum IN (" +
	                  list + ")",
	              {Value(ctx.username)});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		int64_t num = mat.GetValue(0, i).GetValue<int64_t>();
		std::string flag = mat.GetValue(1, i).GetValue<std::string>();
		if (flag == "\\Seen") {
			out[num].seen = true;
		} else if (flag == "\\Flagged") {
			out[num].flagged = true;
		}
	}
	return out;
}

// Does this message carry something a reader would call an attachment? A named
// part, in other words — an inline `cid:` image that the HTML half references is
// not one. The bytes are already in hand because the listing loads each message
// for its subject, so this costs a MIME parse per row and no extra query.
bool HasAttachment(const Message &msg) {
	if (msg.format_type != 4) {
		return false;
	}
	auto entity = quackmail::mime::ParseEntity(msg.raw);
	for (auto &part : quackmail::mime::FlattenParts(entity)) {
		if (!part.filename.empty()) {
			return true;
		}
	}
	return false;
}

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

void Index(Ctx &ctx, const Room &room) {
	std::string filter = ctx.req.Param("f");
	if (filter != "new" && filter != "old" && filter != "all") {
		filter = "all";
	}
	int64_t per = std::min<int64_t>(std::max<int64_t>(ctx.ParamInt("n", kDefaultPageSize), 5), 200);
	int64_t page = std::max<int64_t>(ctx.ParamInt("p", 1), 1);

	auto stats = quackmail::citadel::GetRoomStats(ctx.con, ctx.username, room.room_num);
	auto nums = quackmail::citadel::RoomMessages(ctx.con, room.room_num, filter, 0, stats.last_read);
	std::reverse(nums.begin(), nums.end());

	int64_t total_pages = nums.empty() ? 1 : (int64_t)((nums.size() + (size_t)per - 1) / (size_t)per);
	if (page > total_pages) {
		page = total_pages;
	}
	size_t begin = (size_t)((page - 1) * per);
	size_t end = std::min(nums.size(), begin + (size_t)per);

	std::string toolbar = "<div class=\"actions\">";
	toolbar += Link("/mail/compose", "Write a message", "btn");
	toolbar += Link("/mail/", "All folders", "btn sec");
	toolbar += Link("/search?room=" + std::to_string(room.room_num), "Search this folder", "btn sec");
	toolbar += Link(RoomHref(room) + "?f=new", "Unread", "btn sec");
	toolbar += Link(RoomHref(room) + "?f=all", "All", "btn sec");
	toolbar += FormStart(ctx, RoomHref(room, "/markread"), "inline") + Button("Mark all read", "sec") +
	           FormEnd();
	toolbar += "</div>";

	PageOpts opts;
	// The sidebar lists every folder, so mark this one rather than the section.
	opts.active = "room:" + std::to_string(room.room_num);
	opts.view = (int)room.default_view;
	opts.wide = true;
	opts.toolbar = toolbar;

	if (nums.empty()) {
		Render(ctx, room.display_name, "<p class=\"muted\">This folder is empty.</p>", opts);
		return;
	}

	std::vector<int64_t> shown(nums.begin() + (long)begin, nums.begin() + (long)end);
	auto flags = FlagsFor(ctx, shown);

	// One form around the table. The default action is the move, because that is
	// the one with a control beside it; every other button names its own with
	// `formaction`, which is plain HTML and needs no script.
	std::string body = FormStart(ctx, "/mail/move");
	body += Hidden("room", std::to_string(room.room_num));
	body += Hidden("back", RoomHref(room) + "?f=" + filter + "&p=" + std::to_string(page) + "&n=" +
	                           std::to_string(per));

	body += "<div class=\"wrap\"><table><tr>";
	// Hidden until qc.js marks the document: without script it would be a
	// checkbox that does nothing.
	body += "<th class=\"pick\"><span class=\"jsonly\"><input type=\"checkbox\" class=\"pickall\" "
	        "aria-label=\"Select every message on this page\"></span></th>";
	body += "<th class=\"mark\" title=\"Flagged\">&#9873;</th>";
	body += "<th class=\"mark\" title=\"Attachment\">&#128206;</th>";
	body += Head("Subject") + Head("From") + Head("Date") + "<th class=\"num\">Size</th></tr>";

	for (size_t i = begin; i < end; i++) {
		Message msg;
		if (!quackmail::citadel::LoadMessage(ctx.con, nums[i], msg)) {
			continue;
		}
		const MsgFlags &f = flags[nums[i]];
		std::string num = std::to_string(nums[i]);
		std::string subject = DecodeHeader(msg.subject);
		if (subject.empty()) {
			subject = "(no subject)";
		}
		// Unread is \Seen here, not the Citadel last-read pointer the message
		// board uses: this is a mail folder, and \Seen is both what a mail
		// client means by read and what IMAP shares with this page. Opening a
		// message sets it, so the column tracks reading.
		body += std::string("<tr") + (f.seen ? "" : " class=\"unread\"") + ">";
		body += "<td class=\"pick\"><input type=\"checkbox\" name=\"msgnum\" value=\"" + A(num) +
		        "\" aria-label=\"Select this message\"></td>";
		body += "<td class=\"mark\">" + std::string(f.flagged ? "&#9873;" : "") + "</td>";
		body += "<td class=\"mark\">" + std::string(HasAttachment(msg) ? "&#128206;" : "") + "</td>";
		body += "<td>" + Link(RoomHref(room, "/msg/" + num), subject) + "</td>";
		body += Cell(DecodeHeader(msg.author));
		body += Cell(FormatTime(ctx, msg.msgtime));
		body += "<td class=\"num\">" + T(FormatBytes((int64_t)msg.raw.size())) + "</td>";
		body += "</tr>";
	}
	body += "</table></div>";

	// The bulk bar. Every button acts on whatever is ticked above it.
	std::vector<std::pair<std::string, std::string>> folders;
	for (auto &f : MailFolders(ctx)) {
		if (f.room_num != room.room_num) {
			folders.push_back({f.display_name, f.display_name});
		}
	}
	body += "<div class=\"actions bulk\">";
	body += "<span class=\"muted\">With the selected:</span>";
	if (!folders.empty()) {
		body += Select("folder", folders, "");
		body += "<button class=\"btn sec\">Move</button>";
	}
	body += "<button class=\"btn sec\" formaction=\"/mail/flag\" name=\"set\" value=\"seen\">"
	        "Mark read</button>";
	body += "<button class=\"btn sec\" formaction=\"/mail/flag\" name=\"set\" value=\"unseen\">"
	        "Mark unread</button>";
	body += "<button class=\"btn sec\" formaction=\"/mail/flag\" name=\"set\" value=\"flagged\">"
	        "Flag</button>";
	body += "<button class=\"btn sec\" formaction=\"/mail/flag\" name=\"set\" value=\"unflagged\">"
	        "Clear flag</button>";
	body += "<button class=\"btn danger\" formaction=\"/mail/delete\">" +
	        std::string(room.display_name == "Trash" ? "Delete permanently" : "Move to Trash") +
	        "</button>";
	body += "</div>";
	body += FormEnd();

	body += PagerHtml(room, filter, page, per, total_pages);

	Render(ctx, room.display_name, body, opts);
}

} // namespace

std::vector<Room> MailFolders(Ctx &ctx) {
	std::vector<Room> out;
	for (auto &room : quackmail::citadel::ListRooms(ctx.con, ctx.username, -1, "all")) {
		if (room.mailbox_owner != 0) {
			out.push_back(room);
		}
	}
	return out;
}

const RoomViewHandler &MailboxView() {
	static const RoomViewHandler kView = {
	    quackmail::citadel::VIEW_MAILBOX, "Mail", "message", Index, nullptr, nullptr, nullptr, nullptr};
	return kView;
}

} // namespace qmweb
} // namespace duckdb
