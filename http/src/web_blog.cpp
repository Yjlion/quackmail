#include "web_views.hpp"

#include "quackmail/citadel_msg.hpp"

#include <algorithm>
#include <cstdlib>

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Message;
using quackmail::citadel::Room;

namespace {

// Blog and journal rooms hold **ordinary messages**, not groupware objects.
// There is no vCard or iCalendar here: a post is a post, and what makes the view
// different is that it shows whole entries newest-first instead of a list of
// subjects. So this reuses the message store and RenderMessage rather than the
// object machinery the other views share.
//
// VIEW_BLOG and VIEW_JOURNAL get the same renderer. Citadel distinguishes them
// by intent — one public, one personal — not by storage or layout.

constexpr int64_t kPostsPerPage = 10;

std::string PostHref(const Room &room, int64_t msgnum) {
	// A permalink, and deliberately the ordinary message URL: an entry is a
	// message, so the two should not be different pages.
	return RoomHref(room, "/msg/" + std::to_string(msgnum));
}

void Index(Ctx &ctx, const Room &room) {
	auto nums = quackmail::citadel::RoomMessages(ctx.con, room.room_num, "all", 0, 0);
	std::reverse(nums.begin(), nums.end()); // newest first

	int64_t page = std::max<int64_t>(ctx.ParamInt("p", 1), 1);
	int64_t total_pages = nums.empty() ? 1 : (int64_t)((nums.size() + kPostsPerPage - 1) / kPostsPerPage);
	if (page > total_pages) {
		page = total_pages;
	}
	size_t begin = (size_t)((page - 1) * kPostsPerPage);
	size_t end = std::min(nums.size(), begin + (size_t)kPostsPerPage);

	std::string toolbar = "<div class=\"actions\">";
	if (quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		toolbar += Link(RoomHref(room, "/compose"), "Write an entry", "btn");
	}
	toolbar += FormStart(ctx, RoomHref(room, "/markread"), "inline") + Button("Mark all read", "sec") +
	           FormEnd();
	toolbar += Link(RoomHref(room) + "?view=raw", "View as messages", "btn sec");
	toolbar += "</div>";

	std::string body;
	if (!room.info.empty()) {
		body += "<p class=\"muted\">" + T(room.info) + "</p>";
	}
	if (nums.empty()) {
		body += "<p class=\"muted\">Nothing written here yet.</p>";
	}

	for (size_t i = begin; i < end; i++) {
		Message msg;
		if (!quackmail::citadel::LoadMessage(ctx.con, nums[i], msg)) {
			continue;
		}
		std::string subject = DecodeHeader(msg.subject);
		body += "<article class=\"entry\">";
		body += "<h2>" + Link(PostHref(room, msg.msgnum),
		                      subject.empty() ? std::string("(no subject)") : subject) +
		        "</h2>";
		body += "<p class=\"muted byline\">" + T(DecodeHeader(msg.author)) + " · " +
		        T(FormatTime(ctx, msg.msgtime)) + "</p>";
		// The same renderer the read pane uses, so an entry with attachments or
		// an HTML part behaves identically in both places.
		body += RenderMessage(ctx, room, msg);
		body += "</article>";
	}

	if (total_pages > 1) {
		body += "<div class=\"pager\">";
		if (page > 1) {
			body += Link(RoomHref(room) + "?p=" + std::to_string(page - 1), "Newer entries");
		}
		body += "<span class=\"muted\">Page " + T(std::to_string(page)) + " of " +
		        T(std::to_string(total_pages)) + "</span>";
		if (page < total_pages) {
			body += Link(RoomHref(room) + "?p=" + std::to_string(page + 1), "Older entries");
		}
		body += "</div>";
	}

	// Reading a blog room is reading it: mark what was shown.
	if (!nums.empty()) {
		int64_t highest = *std::max_element(nums.begin(), nums.end());
		quackmail::citadel::SetLastRead(ctx.con, ctx.username, room.room_num, highest);
	}

	PageOpts opts;
	opts.active = "bbs";
	opts.view = (int)room.default_view;
	opts.toolbar = toolbar;
	Render(ctx, room.display_name, body, opts);
}

// No item/edit/save/remove: an entry is a message, so the existing
// /bbs/room/:n/msg/:m read pane, /compose and /delete already handle it. Adding
// a second path to the same thing would be two places to keep correct.
const RoomViewHandler kBlog = {
    quackmail::citadel::VIEW_BLOG, "Blog", "entry", Index, nullptr, nullptr, nullptr, nullptr};

const RoomViewHandler kJournal = {
    quackmail::citadel::VIEW_JOURNAL, "Journal", "entry", Index, nullptr, nullptr, nullptr, nullptr};

} // namespace

const RoomViewHandler &BlogView() {
	return kBlog;
}

const RoomViewHandler &JournalView() {
	return kJournal;
}

} // namespace qmweb
} // namespace duckdb
