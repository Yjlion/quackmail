#include "web.hpp"
#include "web_i18n.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/util.hpp"

#include <cstdlib>

namespace duckdb {
namespace qmweb {

// The web's view of instant messages.
//
// Until this existed the web could *page* people and never be paged: it called
// SendExpress and never PendingExpress, so a message sent to a browser user sat
// in citadel_express unread forever. This is the reader half, and it is a
// transcript rather than a notification because citadel_express rows were always
// retained — MarkExpressDelivered has always been an UPDATE, not a DELETE — so
// the history was there all along with nothing looking at it.

namespace {

// How far back a conversation is shown, and how many lines at most. The
// retention sweep (PruneExpress, qm_express_days) is the outer bound; this is
// just what one screen is worth.
constexpr int64_t kWindowSeconds = 7 * 24 * 3600;
constexpr int64_t kMaxLines = 200;

int64_t PollSeconds(Ctx &ctx) {
	int64_t n = (int64_t)std::strtoll(ConfigStr(ctx.con, "qm_chat_poll_secs", "5").c_str(), nullptr, 10);
	return n > 0 ? n : 5;
}

// The log itself, plus the attributes that keep it polling. Emitted as one
// element and swapped with outerHTML, so the cursor in the hx-get travels with
// the content it describes rather than being duplicated by the caller.
std::string RenderLog(Ctx &ctx, const std::string &with, int64_t token) {
	auto lines = quackmail::citadel::ExpressHistory(ctx.con, ctx.username, with, kWindowSeconds,
	                                                kMaxLines);

	std::string feed = "/chat/feed?since=" + std::to_string(token);
	if (!with.empty()) {
		feed += "&with=" + http::PercentEncode(with);
	}

	std::string out = "<div id=\"chatlog\" class=\"chatlog\" hx-get=\"" + A(feed) + "\" hx-trigger=\"every " +
	                  std::to_string(PollSeconds(ctx)) + "s\" hx-swap=\"outerHTML\">";
	if (lines.empty()) {
		out += "<p class=\"muted\">" + T(Tr(ctx, "chat.empty")) + "</p>";
	}
	for (const auto &e : lines) {
		// Whose line this is, decided by the sender rather than the recipient: a
		// message you sent to yourself is still yours.
		bool mine = quackmail::util::Lower(e.from_user) == quackmail::util::Lower(ctx.username);
		out += "<div class=\"chatline " + std::string(mine ? "mine" : "theirs") + "\">";
		out += "<span class=\"who\">" + T(mine ? ctx.username : e.from_user) + "</span>";
		out += "<span class=\"when\">" + T(FormatTime(ctx, e.sent_at)) + "</span>";
		// Escaped, not sanitized: an instant message is text, and it arrives from
		// XMPP and telnet as well as from here.
		out += "<p>" + T(e.text) + "</p>";
		out += "</div>";
	}
	return out + "</div>";
}

void GetChat(Ctx &ctx) {
	std::string with = ctx.req.Param("with");
	int64_t token = quackmail::citadel::ExpressChangeToken(ctx.con, ctx.username);

	std::string body = "<div class=\"chat\">";

	// Correspondents, and the people who are online but have not written yet —
	// a chat page with nobody to talk to is not a chat page.
	body += "<aside class=\"chatpeople\"><h2>" + T(Tr(ctx, "chat.people")) + "</h2><ul>";
	body += "<li>" + Link("/chat", Tr(ctx, "chat.everyone"), with.empty() ? "current" : "") + "</li>";
	std::vector<std::string> people =
	    quackmail::citadel::ExpressCorrespondents(ctx.con, ctx.username, kWindowSeconds);
	for (const auto &s : quackmail::citadel::ListSessions(ctx.con)) {
		if (s.username.empty() || quackmail::util::Lower(s.username) == quackmail::util::Lower(ctx.username)) {
			continue;
		}
		bool known = false;
		for (const auto &p : people) {
			if (quackmail::util::Lower(p) == quackmail::util::Lower(s.username)) {
				known = true;
				break;
			}
		}
		if (!known) {
			people.push_back(s.username);
		}
	}
	for (const auto &p : people) {
		bool current = quackmail::util::Lower(p) == quackmail::util::Lower(with);
		body += "<li>" + Link("/chat?with=" + http::PercentEncode(p), p, current ? "current" : "") + "</li>";
	}
	body += "</ul></aside>";

	body += "<div class=\"chatmain\">";
	body += RawHtml(RenderLog(ctx, with, 0));
	body += FormStart(ctx, "/chat/send", "chatsend");
	if (with.empty()) {
		body += "<label class=\"field\"><span>" + T(Tr(ctx, "chat.to")) + "</span>" +
		        TextInput("to", "") + "</label>";
	} else {
		body += Hidden("to", with);
	}
	body += "<label class=\"field\"><span>" + T(Tr(ctx, "chat.message")) + "</span>" +
	        TextInput("text", "") + "</label>";
	body += "<p>" + Button(Tr(ctx, "chat.send")) + "</p>";
	body += FormEnd();
	body += "</div></div>";

	body += "<p class=\"muted\">" + T(Tr(ctx, "chat.note")) + "</p>";

	// Rendering the log delivered it, the same way GEXP does for a Citadel
	// client and ShowPendingExpress does for telnet.
	quackmail::citadel::MarkExpressDeliveredThrough(ctx.con, ctx.username, token);

	PageOpts opts;
	opts.active = "chat";
	opts.wide = true;
	Render(ctx, Tr(ctx, "chat.title"), body, opts);
}

void GetChatFeed(Ctx &ctx) {
	int64_t since = ctx.ParamInt("since", 0);
	int64_t token = quackmail::citadel::ExpressChangeToken(ctx.con, ctx.username);
	if (token <= since) {
		// One indexed max() and an empty body. htmx 2 treats 204 as "no swap" by
		// default, so a quiet poll costs the browser nothing and leaves the DOM —
		// and the caret in the message box — exactly where it was.
		SecurityHeaders(ctx);
		ctx.resp.status = 204;
		ctx.resp.body.clear();
		return;
	}
	std::string with = ctx.req.Param("with");
	std::string html = RenderLog(ctx, with, token);
	// A GET that writes, which is normally wrong — but it is exactly what GEXP
	// does, it is idempotent, and it is not a state change the user did not ask
	// for: they opened the page these messages are on. Only the changed branch
	// writes; the 204 above touches nothing.
	quackmail::citadel::MarkExpressDeliveredThrough(ctx.con, ctx.username, token);
	SecurityHeaders(ctx);
	ctx.resp.Html(html);
}

void PostChatSend(Ctx &ctx) {
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
	// The same HX-Request convention the mailbox uses: a fragment for htmx, a
	// redirect for a plain browser, so the page works without script.
	if (ctx.req.HasHeader("HX-Request")) {
		int64_t token = quackmail::citadel::ExpressChangeToken(ctx.con, ctx.username);
		SecurityHeaders(ctx);
		ctx.resp.Html(RenderLog(ctx, to, token));
		return;
	}
	RedirectTo(ctx, to.empty() ? "/chat" : "/chat?with=" + http::PercentEncode(to), "sent");
}

} // namespace

void RegisterChatRoutes(std::vector<Route> &out) {
	// The correspondent is a query parameter rather than a path capture because
	// a Citadel user name is a display name — "Citadel Sysop" has a space in it —
	// and http::MatchPath splits on '/'.
	out.push_back({"GET", "/chat", Role::User, GetChat});
	out.push_back({"GET", "/chat/feed", Role::User, GetChatFeed});
	out.push_back({"POST", "/chat/send", Role::User, PostChatSend});
}

} // namespace qmweb
} // namespace duckdb
