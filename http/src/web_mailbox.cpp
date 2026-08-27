#include "jmap.hpp"
#include "web.hpp"
#include "web_i18n.hpp"
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
// The listing is a two-pane shell: the folder on the left, the message being
// read on the right, each scrolling on its own. Opening a message is a real URL
// (`?open=<msgnum>`) that renders both halves, so it is linkable and the browser
// history works; htmx then fetches that same URL and swaps only the reader,
// which is what keeps the list from jumping back to the top. The server decides
// which of the two to send from the `HX-Request` header — a plain GET always
// gets the whole page.
//
// The bulk actions are still one <form> around the listing with a `formaction`
// per button. That is not a concession to anything: it is simply the shortest
// correct way to post a set of checkboxes to one of six endpoints.

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

// One conversation: the message that represents it in the listing (the newest,
// which is what a person is looking for) and the rest, newest first.
struct Thread {
	std::vector<int64_t> msgnums; // newest first; [0] is the one shown collapsed
	int64_t newest = 0;           // sort key
	bool any_unread = false;
};

// Group `nums` (already newest-first) into conversations.
//
// The rule itself is jmap_mail.cpp's ThreadIdFor — the same function JMAP's
// Thread/get answers with, so the web listing and a JMAP client cannot disagree
// about what a thread is. Deriving it here rather than storing it is what keeps
// that true: there is no second copy to fall behind.
//
// The caller bounds the window with ThreadScanCap for the same reason /search is
// bounded: this loads every message in it to read the References header, and an
// unbounded room would make one page load proportional to the room's whole
// history.
int64_t ThreadScanCap(Ctx &ctx) {
	int64_t cap = (int64_t)std::strtoll(ConfigStr(ctx.con, "qm_web_thread_scan", "2000").c_str(),
	                                    nullptr, 10);
	return cap > 0 ? cap : 2000;
}

std::vector<Thread> GroupThreads(Ctx &ctx, const std::vector<int64_t> &nums,
                                 const std::map<int64_t, MsgFlags> &flags) {
	// Resolved once for the whole listing rather than per message.
	std::string node = NodeName(ctx);
	std::vector<Thread> out;
	std::map<std::string, size_t> seen; // thread id -> index into `out`
	for (size_t i = 0; i < nums.size(); i++) {
		Message msg;
		if (!quackmail::citadel::LoadMessage(ctx.con, nums[i], msg)) {
			continue;
		}
		std::string id = ThreadIdFor(msg, node);
		auto it = seen.find(id);
		if (it == seen.end()) {
			Thread t;
			t.msgnums.push_back(nums[i]);
			t.newest = nums[i];
			out.push_back(t);
			seen[id] = out.size() - 1;
			it = seen.find(id);
		} else {
			out[it->second].msgnums.push_back(nums[i]);
		}
		auto f = flags.find(nums[i]);
		if (f == flags.end() || !f->second.seen) {
			out[it->second].any_unread = true;
		}
	}
	return out;
}

std::string PagerHtml(Ctx &ctx, const Room &room, const std::string &filter, int64_t page, int64_t per,
                      int64_t total_pages) {
	if (total_pages <= 1) {
		return std::string();
	}
	auto href = [&](int64_t p) {
		return RoomHref(room) + "?f=" + filter + "&p=" + std::to_string(p) + "&n=" + std::to_string(per);
	};
	std::string out = "<div class=\"pager\">";
	if (page > 1) {
		out += "<a role=\"button\" class=\"secondary\" href=\"" + A(href(page - 1)) + "\">" +
		       Icon("left") + T(Tr(ctx, "pager.newer")) + "</a>";
	}
	out += "<span class=\"muted\">" + T(Tr(ctx, "pager.page")) + " " + std::to_string(page) + " " +
	       T(Tr(ctx, "pager.of")) + " " + std::to_string(total_pages) + "</span>";
	if (page < total_pages) {
		out += "<a role=\"button\" class=\"secondary\" href=\"" + A(href(page + 1)) + "\">" +
		       T(Tr(ctx, "pager.older")) + Icon("right") + "</a>";
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

	// Threading is a per-user preference and defaults off: a BBS mail folder is
	// often a flat pile of unrelated notices, where grouping only gets in the
	// way.
	bool threaded = quackmail::citadel::GetUserPref(ctx.con, ctx.username, "web_mail_threaded") == "1";

	// A page is `per` threads when threading is on and `per` messages when it is
	// not — paging over messages while displaying threads would show a different
	// number of rows on every page.
	std::vector<Thread> threads;
	size_t begin = 0, end = 0, tbegin = 0, tend = 0;
	int64_t total_pages = 1;

	// One segmented group of view filters, one of actions: Pico draws each as a
	// single bar, which is what stops this reading as eight loose boxes.
	std::string filters = ButtonGroup(
	    Link(RoomHref(room) + "?f=all", Tr(ctx, "mailbox.all"), filter == "all" ? "btn" : "btn sec") +
	    Link(RoomHref(room) + "?f=new", Tr(ctx, "mail.unread"), filter == "new" ? "btn" : "btn sec"));

	std::string toolbar = Toolbar(
	    "<label class=\"vh\" for=\"pickall\">" + T(Tr(ctx, "mailbox.select_all")) + "</label>"
	    "<input type=\"checkbox\" id=\"pickall\" class=\"pickall\">" +
	    Link("/mail/compose", Tr(ctx, "mail.write"), "btn") + filters +
	    "<span class=\"spacer\"></span>" +
	    Link("/search?room=" + std::to_string(room.room_num), Tr(ctx, "mailbox.search_folder"), "btn sec") +
	    FormStart(ctx, RoomHref(room, "/markread"), "inline") +
	    IconButton(Tr(ctx, "mailbox.mark_all_read"), "check", "sec") + FormEnd());

	PageOpts opts;
	// The sidebar lists every folder, so mark this one rather than the section.
	opts.active = "room:" + std::to_string(room.room_num);
	opts.view = (int)room.default_view;
	opts.wide = true;
	opts.panes = true;

	// ---- the reader half ---------------------------------------------------
	// `open` is a message number, and LoadMessageIn is what confirms it is
	// pointed into *this* room before anything is rendered — skipping that
	// check would be a direct IDOR, exactly as it would be on /msg/:m.
	int64_t open = ctx.ParamInt("open", 0);
	std::string reader;
	bool opened = false;
	if (open > 0) {
		Message msg;
		if (LoadMessageIn(ctx, room, open, msg)) {
			opened = true;
			reader = RenderMessage(ctx, room, msg);
			MarkSeen(ctx, room, open);
		}
	}

	// An htmx request wants the reader and nothing else. Every other client —
	// including the whole urllib test suite — announces nothing and gets the
	// complete page, so the URL keeps working exactly as it reads.
	if (ctx.req.HasHeader("HX-Request") && open > 0) {
		SecurityHeaders(ctx);
		ctx.resp.Html(opened ? reader : "<p class=\"muted\">" + T(Tr(ctx, "mailbox.gone")) + "</p>");
		return;
	}

	// "Mail" is what the store calls the inbox and "Inbox" is what a person
	// does — the same mapping the sidebar makes at web_chrome.cpp. Every other
	// folder name is a room name, which is data rather than UI copy, and stays
	// as the owner named it.
	std::string heading =
	    room.display_name == "Mail" ? Tr(ctx, "nav.inbox") : room.display_name;

	std::string listhead = "<div class=\"listhead\"><h1>" + T(heading) + "</h1>" +
	                       RawHtml(toolbar) + "</div>";

	if (nums.empty()) {
		std::string empty = "<div class=\"panes\"><div class=\"list\">" + RawHtml(listhead) +
		                    "<p class=\"muted\">" + T(Tr(ctx, "mailbox.empty")) + "</p></div></div>";
		Render(ctx, heading, empty, opts);
		return;
	}

	// Flags come from one query either way, but over different windows: the page
	// when the listing is flat, and the whole scanned prefix when it is
	// threaded, because GroupThreads needs them to decide whether a conversation
	// has anything unread in it. Asking for `nums` outright would put every
	// message number in the room into one IN(...) list.
	std::map<int64_t, MsgFlags> flags;
	if (threaded) {
		int64_t cap = ThreadScanCap(ctx);
		std::vector<int64_t> window(nums.begin(),
		                            nums.begin() + (long)std::min<size_t>(nums.size(), (size_t)cap));
		flags = FlagsFor(ctx, window);
		threads = GroupThreads(ctx, window, flags);
		total_pages = threads.empty() ? 1 : (int64_t)((threads.size() + (size_t)per - 1) / (size_t)per);
		page = std::min(page, total_pages);
		tbegin = (size_t)((page - 1) * per);
		tend = std::min(threads.size(), tbegin + (size_t)per);
	} else {
		total_pages = (int64_t)((nums.size() + (size_t)per - 1) / (size_t)per);
		page = std::min(page, total_pages);
		begin = (size_t)((page - 1) * per);
		end = std::min(nums.size(), begin + (size_t)per);
		std::vector<int64_t> window(nums.begin() + (long)begin, nums.begin() + (long)end);
		flags = FlagsFor(ctx, window);
	}

	std::string keep = "?f=" + filter + "&p=" + std::to_string(page) + "&n=" + std::to_string(per);

	// One form around the listing. The default action is the move, because that
	// is the one with a control beside it; every other button names its own with
	// `formaction`.
	std::string body = FormStart(ctx, "/mail/move");
	body += Hidden("room", std::to_string(room.room_num));
	body += Hidden("back", RoomHref(room) + keep);

	// One row, whether it stands alone or leads a conversation. `extra` is the
	// thread count that rides on the subject; `reply` indents it.
	auto row = [&](int64_t msgnum, const std::string &extra, bool reply) {
		Message msg;
		if (!quackmail::citadel::LoadMessage(ctx.con, msgnum, msg)) {
			return std::string();
		}
		const MsgFlags &f = flags[msgnum];
		std::string num = std::to_string(msgnum);
		std::string subject = DecodeHeader(msg.subject);
		if (subject.empty()) {
			subject = Tr(ctx, "mailbox.no_subject");
		}
		std::string href = RoomHref(room) + keep + "&open=" + num;

		std::string out = "<div class=\"row\">";
		out += "<input type=\"checkbox\" name=\"msgnum\" value=\"" + A(num) + "\" aria-label=\"" +
		       A(Tr(ctx, "mailbox.select_one")) + "\">";
		out += "<span class=\"marks\">";
		// Unread is \Seen here, not the Citadel last-read pointer the message
		// board uses: this is a mail folder, and \Seen is both what a mail
		// client means by read and what IMAP shares with this page.
		if (!f.seen) {
			out += "<span class=\"dot\" aria-hidden=\"true\"></span>";
		}
		if (f.flagged) {
			out += Icon("flag");
		}
		if (HasAttachment(msg)) {
			out += Icon("clip");
		}
		out += "</span>";
		out += "<span class=\"from\">" + T(DecodeHeader(msg.author)) + "</span>";
		out += "<span class=\"date\">" + T(FormatTime(ctx, msg.msgtime)) + "</span>";
		// hx-* is what turns this into a pane swap; the href is what it does
		// without htmx, and what a middle-click or a bookmark gets.
		out += "<span class=\"subject\"><a href=\"" + A(href) + "\" hx-get=\"" + A(href) +
		       "\" hx-target=\"#reader\" hx-swap=\"innerHTML\" hx-push-url=\"true\" "
		       "hx-indicator=\"#reader\">" +
		       T(subject) + "</a>" + RawHtml(extra) + "</span>";
		out += "</div>";
		(void)reply;
		return out;
	};

	body += "<ul class=\"msglist longlist\">";
	if (threaded) {
		for (size_t i = tbegin; i < tend; i++) {
			const Thread &t = threads[i];
			bool current = false;
			for (auto n : t.msgnums) {
				current = current || n == open;
			}
			body += "<li" + std::string(t.any_unread ? " class=\"unread\"" : "") +
			        (current ? " aria-current=\"true\"" : "") + ">";
			body += RawHtml(row(t.msgnums[0], "", false));
			if (t.msgnums.size() > 1) {
				// The replies are a disclosure rather than always-on rows: a
				// forty-message thread must not push every other conversation
				// off the screen. The summary carries the count, so the control
				// says what it will show rather than being an unlabelled arrow.
				body += "<details><summary>" +
				        T(TrF(ctx, "mailbox.more_in_thread",
				              {std::to_string(t.msgnums.size() - 1)})) +
				        "</summary><ul class=\"thread\">";
				for (size_t j = 1; j < t.msgnums.size(); j++) {
					body += "<li>" + RawHtml(row(t.msgnums[j], "", true)) + "</li>";
				}
				body += "</ul></details>";
			}
			body += "</li>";
		}
	} else {
		for (size_t i = begin; i < end; i++) {
			const MsgFlags &f = flags[nums[i]];
			body += "<li" + std::string(f.seen ? "" : " class=\"unread\"") +
			        (nums[i] == open ? " aria-current=\"true\"" : "") + ">";
			body += RawHtml(row(nums[i], "", false));
			body += "</li>";
		}
	}
	body += "</ul>";

	// The bulk bar. Every button acts on whatever is ticked above it.
	std::vector<std::pair<std::string, std::string>> folders;
	for (auto &f : MailFolders(ctx)) {
		if (f.room_num != room.room_num) {
			folders.push_back({f.display_name, f.display_name});
		}
	}
	body += "<div class=\"toolbar bulk\">";
	body += "<span class=\"muted\">" + T(Tr(ctx, "mailbox.with_selected")) + "</span>";
	if (!folders.empty()) {
		body += Select("folder", folders, "");
		body += Button(Tr(ctx, "mailbox.move"), "sec");
	}
	body += ButtonGroup(
	    "<button class=\"secondary\" formaction=\"/mail/flag\" name=\"set\" value=\"seen\">" +
	    T(Tr(ctx, "mailbox.mark_read")) + "</button>"
	    "<button class=\"secondary\" formaction=\"/mail/flag\" name=\"set\" value=\"unseen\">" +
	    T(Tr(ctx, "mailbox.mark_unread")) + "</button>"
	    "<button class=\"secondary\" formaction=\"/mail/flag\" name=\"set\" value=\"flagged\">" +
	    T(Tr(ctx, "mailbox.flag")) + "</button>"
	    "<button class=\"secondary\" formaction=\"/mail/flag\" name=\"set\" value=\"unflagged\">" +
	    T(Tr(ctx, "mailbox.clear_flag")) + "</button>");
	body += "<button class=\"outline danger\" formaction=\"/mail/delete\" data-key=\"trash\">" +
	        T(Tr(ctx, room.display_name == "Trash" ? "mailbox.delete_forever" : "mailbox.to_trash")) +
	        "</button>";
	body += "</div>";
	body += FormEnd();

	body += PagerHtml(ctx, room, filter, page, per, total_pages);

	std::string page_html = "<div class=\"panes" + std::string(opened ? " open" : "") + "\">";
	page_html += "<div class=\"list\">" + RawHtml(listhead) + RawHtml(body) + "</div>";
	page_html += "<div class=\"reader\" id=\"reader\">" + RawHtml(reader) + "</div>";
	page_html += "</div>";

	Render(ctx, heading, page_html, opts);
}

} // namespace

std::vector<Room> MailFoldersFrom(const std::vector<Room> &rooms) {
	// The groupware rooms are personal rooms too, and they are emphatically not
	// mail folders: every message in one is a format_type 4 object the calendar
	// or address book parses, so filing a letter into Calendar would put
	// something there that the view has to skip and the owner cannot see.
	static const char *kNotFolders[] = {"Calendar", "Contacts", "Tasks", "Notes"};
	// Citadel's own provisioning order first, so the four a person expects lead
	// the list; anything else they have — a Sieve fileinto target — follows.
	static const char *kOrder[] = {"Mail", "Sent Items", "Drafts", "Trash"};

	std::vector<Room> out;
	for (auto *want : kOrder) {
		for (auto &r : rooms) {
			if (r.mailbox_owner != 0 && r.display_name == want) {
				out.push_back(r);
			}
		}
	}
	for (auto &r : rooms) {
		if (r.mailbox_owner == 0) {
			continue;
		}
		bool skip = false;
		for (auto *g : kNotFolders) {
			skip = skip || r.display_name == g;
		}
		for (auto &f : out) {
			skip = skip || f.room_num == r.room_num;
		}
		if (!skip) {
			out.push_back(r);
		}
	}
	return out;
}

std::vector<Room> MailFolders(const Ctx &ctx) {
	return MailFoldersFrom(quackmail::citadel::ListRooms(ctx.con, ctx.username, -1, "all"));
}

std::vector<int64_t> UnseenCounts(const Ctx &ctx, const std::vector<int64_t> &room_nums) {
	// What a mail folder means by unread, for the sidebar — the same \Seen the
	// listing bolds a row on. Counting with the Citadel last-read pointer here
	// instead would let the two disagree the moment somebody reads anything but
	// the newest message, because a high-water mark cannot skip.
	std::vector<int64_t> out(room_nums.size(), 0);
	if (room_nums.empty()) {
		return out;
	}
	std::string list;
	for (auto n : room_nums) {
		if (!list.empty()) {
			list += ",";
		}
		list += std::to_string(n);
	}
	auto r = Exec(ctx.con,
	              "SELECT rm.room_num, count(*) FROM citadel_room_msgs rm "
	              "WHERE rm.room_num IN (" +
	                  list +
	                  ") AND NOT EXISTS (SELECT 1 FROM citadel_msg_flags f "
	                  "WHERE f.msgnum = rm.msgnum AND f.username = $1 AND f.flag = $2) "
	                  "GROUP BY rm.room_num",
	              {Value(ctx.username), Value("\\Seen")});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		int64_t num = mat.GetValue(0, i).GetValue<int64_t>();
		int64_t n = mat.GetValue(1, i).GetValue<int64_t>();
		for (size_t j = 0; j < room_nums.size(); j++) {
			if (room_nums[j] == num) {
				out[j] = n;
			}
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
