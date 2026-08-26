#include "web.hpp"
#include "web_assets.hpp"
#include "web_i18n.hpp"

#include "quackmail/mime.hpp"
#include "quackmail/tz.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Message;
using quackmail::citadel::Room;

Ctx::Ctx(Connection &c, const http::Request &rq, http::Response &rs) : con(c), req(rq), resp(rs) {
}

std::string Ctx::Cap(size_t i) const {
	return i < captures.size() ? captures[i] : std::string();
}

namespace {

int64_t ToInt(const std::string &s, int64_t dflt) {
	if (s.empty()) {
		return dflt;
	}
	char *end = nullptr;
	long long v = std::strtoll(s.c_str(), &end, 10);
	if (end == s.c_str() || (end && *end != '\0')) {
		return dflt;
	}
	return (int64_t)v;
}

} // namespace

int64_t Ctx::ParamInt(const std::string &name, int64_t dflt) const {
	return ToInt(req.Param(name), dflt);
}

int64_t Ctx::FormInt(const std::string &name, int64_t dflt) const {
	return ToInt(req.Form(name), dflt);
}

unique_ptr<QueryResult> Exec(Connection &con, const std::string &sql, vector<Value> params) {
	auto stmt = con.Prepare(sql);
	if (stmt->HasError()) {
		return nullptr;
	}
	// `params` is a named lvalue here, which is what Execute's non-const
	// reference parameter needs — that is the whole reason for this wrapper.
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		return nullptr;
	}
	return r;
}

// ---- configuration -------------------------------------------------------

std::string ConfigStr(Connection &con, const char *name, const std::string &dflt) {
	return quackmail::citadel::GetConfig(con, name, dflt);
}

bool ConfigBool(Connection &con, const char *name, bool dflt) {
	std::string v = quackmail::citadel::GetConfig(con, name, dflt ? "1" : "0");
	return v == "1" || v == "true" || v == "yes" || v == "on";
}

// ---- escaping ------------------------------------------------------------

std::string T(const std::string &text) {
	return http::EscapeHtml(text);
}

std::string A(const std::string &value) {
	return http::EscapeAttr(value);
}

std::string RawHtml(const std::string &html) {
	return html;
}

// ---- small builders ------------------------------------------------------

std::string Cell(const std::string &text, const std::string &css_class) {
	if (css_class.empty()) {
		return "<td>" + T(text) + "</td>";
	}
	return "<td class=\"" + A(css_class) + "\">" + T(text) + "</td>";
}

std::string Head(const std::string &text) {
	return "<th>" + T(text) + "</th>";
}

std::string Link(const std::string &href, const std::string &label, const std::string &css_class) {
	std::string out = "<a href=\"" + A(href) + "\"";
	if (!css_class.empty()) {
		out += " class=\"" + A(css_class) + "\"";
	}
	return out + ">" + T(label) + "</a>";
}

std::string TextInput(const std::string &name, const std::string &value, const std::string &type,
                      const std::string &placeholder) {
	std::string out = "<input type=\"" + A(type) + "\" name=\"" + A(name) + "\" value=\"" + A(value) + "\"";
	if (!placeholder.empty()) {
		out += " placeholder=\"" + A(placeholder) + "\"";
	}
	return out + ">";
}

std::string TextArea(const std::string &name, const std::string &value, int rows) {
	return "<textarea name=\"" + A(name) + "\" rows=\"" + std::to_string(rows) + "\">" + T(value) +
	       "</textarea>";
}

std::string Hidden(const std::string &name, const std::string &value) {
	return "<input type=\"hidden\" name=\"" + A(name) + "\" value=\"" + A(value) + "\">";
}

std::string Checkbox(const std::string &name, bool checked, const std::string &label) {
	return "<label class=\"chk\"><input type=\"checkbox\" name=\"" + A(name) + "\" value=\"1\"" +
	       (checked ? " checked" : "") + "> " + T(label) + "</label>";
}

std::string Select(const std::string &name, const std::vector<std::pair<std::string, std::string>> &options,
                   const std::string &selected) {
	std::string out = "<select name=\"" + A(name) + "\">";
	for (auto &opt : options) {
		out += "<option value=\"" + A(opt.first) + "\"" + (opt.first == selected ? " selected" : "") + ">" +
		       T(opt.second) + "</option>";
	}
	return out + "</select>";
}

std::string FormStart(const Ctx &ctx, const std::string &action, const std::string &css_class) {
	std::string out = "<form method=\"post\" action=\"" + A(action) + "\"";
	if (!css_class.empty()) {
		out += " class=\"" + A(css_class) + "\"";
	}
	out += ">";
	// Every form carries the synchronizer token; the router refuses any POST
	// without it, so leaving it out here would break the form loudly rather
	// than silently opening a CSRF hole.
	out += Hidden("_csrf", ctx.csrf);
	return out;
}

std::string FormEnd() {
	return "</form>";
}

std::string Button(const std::string &label, const std::string &css_class) {
	std::string cls = css_class.empty() ? std::string("btn") : "btn " + css_class;
	return "<button class=\"" + A(cls) + "\">" + T(label) + "</button>";
}

// ---- flash ---------------------------------------------------------------

std::string FlashText(const std::string &slug) {
	// A fixed table, never free text out of the query string: reflecting a
	// parameter into a page is a text-injection vector even when escaped, and
	// one missed escape would make it XSS.
	if (slug == "saved") {
		return "Saved.";
	}
	if (slug == "created") {
		return "Created.";
	}
	if (slug == "deleted") {
		return "Deleted.";
	}
	if (slug == "sent") {
		return "Message sent.";
	}
	if (slug == "posted") {
		return "Message posted.";
	}
	if (slug == "moved") {
		return "Message moved.";
	}
	if (slug == "zapped") {
		return "Room forgotten. It will not appear in your room list again until you visit it.";
	}
	if (slug == "unzapped") {
		return "Room restored.";
	}
	if (slug == "marked") {
		return "Marked as read.";
	}
	if (slug == "flagged") {
		return "Flag updated.";
	}
	if (slug == "nothing") {
		return "Nothing was selected, so nothing happened.";
	}
	if (slug == "revoked") {
		return "Session revoked.";
	}
	if (slug == "password") {
		return "Password changed. Every other session for this account has been signed out.";
	}
	if (slug == "paged") {
		return "Message delivered.";
	}
	if (slug == "keygen") {
		return "Key generated. Publish the DNS record below before signing with it.";
	}
	if (slug == "queued") {
		return "Queued for another delivery attempt.";
	}
	if (slug == "activated") {
		return "Script activated.";
	}
	if (slug == "distributed") {
		return "Spooler run. Anything new in a list room is on the outbound queue.";
	}
	if (slug == "approved") {
		return "Approved and posted. The spooler will distribute it on its next pass.";
	}
	if (slug == "rejected") {
		return "Rejected. Nothing was sent.";
	}
	if (slug == "fetched") {
		return "Polled. Anything new is in the target room.";
	}
	if (slug == "feed_ok") {
		return "Connected and authenticated successfully.";
	}
	if (slug == "feed_failed") {
		return "The feed could not be reached — see its status in the table.";
	}
	if (slug == "room_created") {
		return "Room created. You administer it: the settings below decide who else can see it, read it "
		       "and post to it.";
	}
	if (slug == "invited") {
		return "Invitation sent. That address joins the list once somebody reading it follows the link.";
	}
	if (slug == "confirm_sent") {
		// Deliberately says nothing about whether the list or the subscription
		// exists: this page is anonymous, and a more helpful message would let
		// anyone test whether a given address is on a given list.
		return "If that list exists, a confirmation has been e-mailed to the address you gave. "
		       "Nothing changes until you follow the link in it.";
	}
	return std::string();
}

// ---- page shell ----------------------------------------------------------

namespace {

// The critical stylesheet, inlined into every page. Everything else lives in
// http/assets/qc.css and is served from /static.
//
// The split is not arbitrary. What stays here is exactly what a page cannot be
// read without: the ten custom properties (which every rule in the external
// sheet reads), the reset, and the layout skeleton. That means a page renders
// legibly on the very first packet — no flash of unstyled content — and it
// stays legible if /static is unreachable behind a misconfigured proxy. It is
// also why the split has to be "critical inline plus the rest external" rather
// than "all external": the per-user theme below is a :root override that cannot
// be a cacheable asset, and it has to arrive with the page.
const char *kCriticalCss = R"CSS(
:root { color-scheme: light dark;
  --bg:#fbfbfa; --fg:#1c1b19; --muted:#6b6a66; --line:#e0dfdb;
  --panel:#ffffff; --accent:#8a5a2b; --accent-fg:#ffffff; --warn:#8c2f00; --ok:#1f6b3a;
  --s1:.25rem; --s2:.5rem; --s3:.8rem; --s4:1.1rem; --s5:2rem; --r1:3px;
  --mono:ui-monospace,SFMono-Regular,Menlo,monospace; }
@media (prefers-color-scheme: dark) { :root {
  --bg:#17171a; --fg:#e9e8e4; --muted:#9d9c97; --line:#2e2e33;
  --panel:#1f1f23; --accent:#c98d54; --accent-fg:#17171a; --warn:#ff9b7a; --ok:#7ecf9a; } }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--fg);
  font:15px/1.55 ui-sans-serif,system-ui,"Segoe UI",Roboto,sans-serif; }
.navtoggle { position:absolute; opacity:0; pointer-events:none; }
.app { display:grid; grid-template-columns:15rem 1fr; grid-template-rows:auto 1fr;
  min-height:100vh; }
header.top { grid-column:1 / -1; display:flex; flex-wrap:wrap; align-items:baseline;
  gap:.75rem 1.25rem; padding:.7rem 1.1rem; border-bottom:1px solid var(--line);
  background:var(--panel); }
main { padding:1.2rem 1.1rem 4rem; min-width:0; }
main > .inner { max-width:62rem; }
body.wide main > .inner { max-width:none; }
/* Signed out, SidebarFor() emits no <nav> at all, so without this the grid's
   first (15rem) column is the only free cell and main — login form included —
   collapses into that narrow gutter instead of the full page. */
body.anon .app { grid-template-columns: 1fr; }
body.anon main { display:flex; align-items:center; justify-content:center; }
)CSS";

// The sidebar. Grouped, because a flat list stopped scaling somewhere around
// the fifteenth page and this module now has roughly thirty.
//
// `active` marks the current item with aria-current, which is both the
// accessible answer and the hook the stylesheet colours.
std::string SidebarFor(const Ctx &ctx, const std::string &active) {
	if (!ctx.Authed()) {
		return std::string();
	}
	std::string out = "<nav class=\"sidebar\" aria-label=\"Sections\">";

	// `label_key` is a Tr() catalog key, not literal text — every static nav
	// label in this sidebar goes through the message catalog.
	auto item = [&](const char *href, const char *label_key, const char *key) {
		std::string extra = (active == key) ? " aria-current=\"page\"" : "";
		out += "<a href=\"" + A(href) + "\"" + extra + "><span>" + T(Tr(ctx, label_key)) + "</span></a>";
	};
	auto group = [&](const char *label_key) {
		out += "<div class=\"group\"><span class=\"label\">" + T(Tr(ctx, label_key)) + "</span>";
	};
	auto endgroup = [&]() { out += "</div>"; };

	// A room's own link, with its unread count riding along on the right — the
	// `.count` span the stylesheet has always described and nothing emitted.
	// Marked current by room number, so the folder you are reading is the one
	// highlighted rather than the section it belongs to.
	std::string room_key;
	auto room_link = [&](const Room &room, const std::string &label, int64_t unread) {
		std::string href = "/bbs/room/" + std::to_string(room.room_num);
		std::string key = "room:" + std::to_string(room.room_num);
		std::string extra = (active == key) ? " aria-current=\"page\"" : "";
		if (!extra.empty()) {
			room_key = key;
		}
		out += "<a href=\"" + A(href) + "\"" + extra + "><span>" + T(label) + "</span>";
		if (unread > 0) {
			out += "<span class=\"count\">" + std::to_string(unread) + "</span>";
		}
		out += "</a>";
	};

	// One listing for the whole sidebar. This replaces the four FindUserRoom
	// lookups the groupware group used to cost — those rooms are personal rooms
	// and are already in here — so the counts below arrive for fewer queries
	// than the sidebar ran before them, not more.
	auto rooms = quackmail::citadel::ListRooms(ctx.con, ctx.username, -1, "all");

	static const char *kGroupwareRooms[] = {"Calendar", "Contacts", "Tasks", "Notes"};

	// One definition of "a mail folder", shared with the move targets and the
	// listing itself — see MailFoldersFrom.
	std::vector<Room> folders = MailFoldersFrom(rooms);

	group("nav.mail");
	item("/mail/compose", "nav.compose", "compose");
	{
		std::vector<int64_t> nums;
		for (auto &f : folders) {
			nums.push_back(f.room_num);
		}
		// \Seen, not the last-read pointer: this has to be the same count the
		// folder's own listing shows in bold, and a high-water mark cannot skip
		// a message somebody left unread behind one they opened.
		auto unseen = UnseenCounts(ctx, nums);
		for (size_t i = 0; i < folders.size(); i++) {
			// "Mail" is what the store calls it and "Inbox" is what a person
			// does. The other folders are already named the way they read — and
			// are room names, not UI copy, so they do not go through Tr().
			std::string label = folders[i].display_name == "Mail" ? Tr(ctx, "nav.inbox") : folders[i].display_name;
			room_link(folders[i], label, unseen[i]);
		}
	}
	item("/mail/", "nav.all_folders", "mail");
	endgroup();

	// The user's own groupware rooms, linked by number because that is how rooms
	// are addressed. EnsureUserRooms provisions these at first login; one that
	// somehow does not exist is simply omitted rather than linked to a 404.
	{
		std::string groupware;
		static const char *kKeys[] = {"calendar", "contacts", "tasks", "notes"};
		for (size_t g = 0; g < 4; g++) {
			for (auto &r : rooms) {
				if (r.mailbox_owner == 0 || r.display_name != kGroupwareRooms[g]) {
					continue;
				}
				std::string href = "/bbs/room/" + std::to_string(r.room_num);
				std::string key = "room:" + std::to_string(r.room_num);
				bool current = active == kKeys[g] || active == key;
				if (current) {
					room_key = key;
				}
				groupware += "<a href=\"" + A(href) + "\"" +
				             (current ? " aria-current=\"page\"" : "") + "><span>" +
				             T(r.display_name) + "</span></a>";
				break;
			}
		}
		if (!groupware.empty()) {
			group("nav.groupware");
			out += groupware;
			endgroup();
		}
	}

	group("nav.rooms");
	{
		// Rooms with something new in them, most unread first. Capped, because
		// this is rendered on every page: `qm_web_sidebar_rooms` is the ceiling
		// and 0 turns the listing — and the query behind it — off entirely.
		int64_t limit = (int64_t)std::strtoll(ConfigStr(ctx.con, "qm_web_sidebar_rooms", "10").c_str(),
		                                      nullptr, 10);
		std::vector<std::pair<Room, int64_t>> unread;
		if (limit > 0) {
			std::vector<Room> public_rooms;
			std::vector<int64_t> nums;
			for (auto &r : rooms) {
				if (r.mailbox_owner == 0) {
					public_rooms.push_back(r);
					nums.push_back(r.room_num);
				}
			}
			auto stats = quackmail::citadel::RoomStatsBulk(ctx.con, ctx.username, nums);
			for (size_t i = 0; i < public_rooms.size(); i++) {
				if (stats[i].new_count > 0) {
					unread.push_back({public_rooms[i], stats[i].new_count});
				}
			}
			std::sort(unread.begin(), unread.end(),
			          [](const std::pair<Room, int64_t> &a, const std::pair<Room, int64_t> &b) {
				          return a.second > b.second;
			          });
			if ((int64_t)unread.size() > limit) {
				unread.resize((size_t)limit);
			}
		}
		// "All rooms" also stands in for a room that is not itself listed, so a
		// room page never leaves the whole sidebar unmarked.
		bool in_a_room = active.rfind("room:", 0) == 0;
		bool listed = false;
		for (auto &u : unread) {
			listed = listed || active == "room:" + std::to_string(u.first.room_num);
		}
		std::string extra =
		    (active == "bbs" || (in_a_room && !listed && room_key.empty())) ? " aria-current=\"page\"" : "";
		out += "<a href=\"/bbs/\"" + extra + "><span>" + T(Tr(ctx, "nav.all_rooms")) + "</span></a>";
		for (auto &u : unread) {
			room_link(u.first, u.first.display_name, u.second);
		}
	}
	item("/search", "nav.search", "search");
	if (MayCreateRooms(ctx)) {
		item("/bbs/new", "nav.create_room", "newroom");
	}
	item("/bbs/who", "nav.who_online", "who");
	endgroup();

	group("nav.you");
	item("/prefs", "nav.preferences", "prefs");
	item("/prefs/sieve", "nav.filters", "sieve");
	item("/prefs/sessions", "nav.sessions", "sessions");
	endgroup();

	// Same gate as the router applies to every /admin route, so the link never
	// points at a 403.
	if (ctx.IsAide() && ConfigBool(ctx.con, "qm_web_admin_enabled", false)) {
		group("nav.system");
		item("/admin/", "nav.admin", "admin");
		endgroup();
	}

	out += "</nav>";
	return out;
}

} // namespace

PageOpts::PageOpts() {
}

void SecurityHeaders(Ctx &ctx, const std::string &csp) {
	// default-src 'none' plus a per-response nonce, rather than 'unsafe-inline'.
	// The nonce is three lines of work and it is the difference between a
	// reflected-XSS bug being inert and being execution.
	std::string policy = csp;
	if (policy.empty()) {
		// 'self' covers /static; the nonce covers the critical CSS and the theme
		// override that have to arrive inline. In CSP3 the two coexist — a nonce
		// does not suppress host sources, only 'strict-dynamic' does — so this
		// is additive rather than a weakening.
		//
		// Note that the CSP for a *message* body (web_mail.cpp's HTML-part
		// route) is passed in explicitly and must never gain 'self': that frame
		// renders markup written by whoever sent the mail.
		policy = "default-src 'none'; script-src 'self' 'nonce-" + ctx.nonce + "'; style-src 'self' 'nonce-" +
		         ctx.nonce +
		         "'; img-src 'self' data:; frame-src 'self'; form-action 'self'; "
		         "frame-ancestors 'none'; base-uri 'none'";
	}
	ctx.resp.SetHeader("Content-Security-Policy", policy);
	ctx.resp.SetHeader("X-Content-Type-Options", "nosniff");
	ctx.resp.SetHeader("X-Frame-Options", "DENY");
	ctx.resp.SetHeader("Referrer-Policy", "no-referrer");
	// Mail must not survive in a shared or kiosk browser's disk cache.
	ctx.resp.SetHeader("Cache-Control", "private, no-store");
	if (ctx.tls && ConfigBool(ctx.con, "qm_web_hsts", false)) {
		ctx.resp.SetHeader("Strict-Transport-Security", "max-age=31536000");
	}
}

// Named themes. kCss above is a ten-variable custom-property sheet and every
// rule below it reads those variables, so a theme is just a :root override — no
// second stylesheet, no per-theme rules to keep in sync.
//
// "auto" is the sheet as written: light, following the OS at night. The rest
// pin a single appearance, which is what someone who dislikes the automatic
// switch actually wants.
struct Theme {
	const char *name;
	const char *label;
	const char *css; // empty = kCss unmodified
};

const Theme kThemes[] = {
    {"auto", "Follow my system", ""},
    {"light", "Always light",
     ":root{--bg:#fbfbfa;--fg:#1c1b19;--muted:#6b6a66;--line:#e0dfdb;--panel:#fff;"
     "--accent:#8a5a2b;--accent-fg:#fff;--warn:#8c2f00;--ok:#1f6b3a;color-scheme:light}"},
    {"dark", "Always dark",
     ":root{--bg:#17171a;--fg:#e9e8e4;--muted:#9d9c97;--line:#2e2e33;--panel:#1f1f23;"
     "--accent:#c98d54;--accent-fg:#17171a;--warn:#ff9b7a;--ok:#7ecf9a;color-scheme:dark}"},
    {"sepia", "Sepia",
     ":root{--bg:#f4ecd8;--fg:#3b2f2a;--muted:#7a6a5d;--line:#ddd0b5;--panel:#fbf5e6;"
     "--accent:#8a5a2b;--accent-fg:#fbf5e6;--warn:#9c3a12;--ok:#4a6b34;color-scheme:light}"},
    {"slate", "Slate",
     ":root{--bg:#1b2027;--fg:#dfe4ea;--muted:#93a1b0;--line:#2c3440;--panel:#222933;"
     "--accent:#6fa8d6;--accent-fg:#131820;--warn:#e08a6a;--ok:#7fc8a0;color-scheme:dark}"},
    {"amber", "Amber on black",
     ":root{--bg:#0b0b0b;--fg:#ffb642;--muted:#a8752a;--line:#3a2a10;--panel:#121008;"
     "--accent:#ffd08a;--accent-fg:#0b0b0b;--warn:#ff7043;--ok:#b8d94a;color-scheme:dark}"},
};

// The theme in force: the signed-in user's choice, else the site default.
// "auto" (or anything unrecognized) leaves kCss alone.
std::string ThemeCss(Ctx &ctx) {
	std::string want;
	if (ctx.Authed()) {
		want = quackmail::citadel::GetUserPref(ctx.con, ctx.username, "web_theme");
	}
	if (want.empty() || want == "auto") {
		want = ConfigStr(ctx.con, "qm_web_theme", "auto");
	}
	for (auto &t : kThemes) {
		if (want == t.name) {
			return t.css;
		}
	}
	return "";
}

std::vector<std::pair<std::string, std::string>> ThemeOptions() {
	std::vector<std::pair<std::string, std::string>> out;
	for (auto &t : kThemes) {
		out.push_back({t.name, t.label});
	}
	return out;
}

void Render(Ctx &ctx, const std::string &title, const std::string &body, int status) {
	Render(ctx, title, body, PageOpts(), status);
}

void Render(Ctx &ctx, const std::string &title, const std::string &body, const PageOpts &opts,
            int status) {
	SecurityHeaders(ctx);

	std::string node = ConfigStr(ctx.con, "c_humannode", "QuackCit");
	std::string page =
	    "<!doctype html><html lang=\"" + A(EffectiveLocale(ctx)) + "\"><head><meta charset=\"utf-8\">";
	page += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
	page += "<title>" + T(title.empty() ? node : title + " — " + node) + "</title>";
	// Order matters twice over. The external sheet comes first so the inline
	// block below can override it; and the theme goes in the *same* nonced
	// element as the critical CSS, because style-src has no 'unsafe-inline' and
	// a second, unnonced <style> would be dropped in silence.
	page += "<link rel=\"stylesheet\" href=\"" + A(AssetUrl("qc.css")) + "\">";
	page += "<style nonce=\"" + A(ctx.nonce) + "\">" + RawHtml(kCriticalCss) + RawHtml(ThemeCss(ctx)) +
	        "</style>";
	page += "<script nonce=\"" + A(ctx.nonce) + "\" src=\"" + A(AssetUrl("qc.js")) + "\" defer></script>";
	if (!opts.script.empty()) {
		page += "<script nonce=\"" + A(ctx.nonce) + "\" src=\"" + A(AssetUrl(opts.script.c_str())) +
		        "\" defer></script>";
	}
	page += "</head>";

	std::string body_class;
	if (!ctx.Authed()) {
		body_class += " anon";
	}
	if (opts.wide) {
		body_class += " wide";
	}
	if (opts.view >= 0) {
		body_class += " view-" + std::to_string(opts.view);
	}
	// Density is a mail-list thing, not a general page thing — scoping it to
	// mail rooms (rather than a page-wide setting) keeps it from touching
	// Notes/Tasks/Calendar/Wiki, which have their own layouts already.
	if (ctx.Authed() &&
	    (opts.view == quackmail::citadel::VIEW_MAILBOX || opts.view == quackmail::citadel::VIEW_DRAFTS)) {
		std::string layout = quackmail::citadel::GetUserPref(ctx.con, ctx.username, "web_mail_layout");
		if (layout == "compact" || layout == "wide") {
			body_class += " layout-" + layout;
		}
	}
	page += "<body" + (body_class.empty() ? "" : " class=\"" + A(body_class.substr(1)) + "\"") + ">";
	page += "<a class=\"skip\" href=\"#main\">Skip to content</a>";
	// The checkbox precedes .app so the stylesheet can reach the sidebar with a
	// sibling selector: the mobile menu needs no script at all.
	page += "<input type=\"checkbox\" id=\"navtoggle\" class=\"navtoggle\">";
	page += "<div class=\"app\">";

	page += "<header class=\"top\">";
	if (ctx.Authed()) {
		page += "<label for=\"navtoggle\" class=\"navbtn\" title=\"Sections\">&#9776;</label>";
	}
	page += "<span class=\"brand\">" + T(node) + "</span>";
	if (ctx.Authed()) {
		// A GET form, so it carries no CSRF token and needs none — and a search
		// stays linkable and bookmarkable, which a POST would take away. It is
		// the one control that belongs on every page rather than in the sidebar:
		// finding a message is not a section of the site.
		page += "<form method=\"get\" action=\"/search\" class=\"topsearch\" role=\"search\">";
		page += "<label class=\"vh\" for=\"topq\">Search messages</label>";
		page += "<input id=\"topq\" type=\"search\" name=\"q\" value=\"" +
		        A(ctx.req.path == "/search" ? ctx.req.Param("q") : std::string()) +
		        "\" placeholder=\"Search\">";
		page += "</form>";
		page += "<span class=\"who\">" + T(ctx.username);
		page += FormStart(ctx, "/logout", "inline") + Button("Sign out", "sec") + FormEnd();
		page += "</span>";
	}
	page += "</header>";

	page += SidebarFor(ctx, opts.active);
	page += "<main id=\"main\"><div class=\"inner\">";

	// A plaintext session is a real risk worth naming on the page itself, not
	// only in the docs.
	if (!ctx.tls && ctx.Authed()) {
		page += "<div class=\"warnbar\">This connection is not encrypted. Your password and mail are "
		        "visible to anything on the network path.</div>";
	}
	std::string flash = FlashText(ctx.req.Param("ok"));
	if (!flash.empty()) {
		page += "<div class=\"flash\">" + T(flash) + "</div>";
	}
	std::string problem = FlashText(ctx.req.Param("err"));
	if (!problem.empty()) {
		page += "<div class=\"flash err\">" + T(problem) + "</div>";
	}
	if (!title.empty()) {
		page += "<h1>" + T(title) + "</h1>";
	}
	if (!opts.toolbar.empty()) {
		page += RawHtml(opts.toolbar);
	}
	page += RawHtml(body);
	page += "</div></main></div></body></html>";

	ctx.resp.Html(page, status);
}

void RedirectTo(Ctx &ctx, const std::string &path, const std::string &flash) {
	SecurityHeaders(ctx);
	std::string target = path;
	if (!flash.empty()) {
		target += (target.find('?') == std::string::npos ? "?ok=" : "&ok=") + http::PercentEncode(flash);
	}
	// Relative Location values are legal (RFC 7231) and mean the Host header
	// never has to be trusted, which deletes the open-redirect class here.
	ctx.resp.Redirect(target, 303);
}

void ErrorPage(Ctx &ctx, int status, const std::string &title, const std::string &detail) {
	std::string body = "<p>" + T(detail) + "</p>";
	if (!ctx.Authed()) {
		body += "<p>" + Link("/login", "Sign in") + "</p>";
	}
	Render(ctx, title, body, status);
}

void NotFound(Ctx &ctx) {
	ErrorPage(ctx, 404, "Not found", "There is nothing at this address.");
}

void Forbidden(Ctx &ctx, const std::string &why) {
	ErrorPage(ctx, 403, "Not permitted", why);
}

void BadRequest(Ctx &ctx, const std::string &why) {
	ErrorPage(ctx, 400, "Bad request", why);
}

// ---- shared helpers ------------------------------------------------------

bool ResolveRoomFor(Ctx &ctx, const std::string &name, Room &out) {
	if (name.empty() || !ctx.Authed()) {
		return false;
	}
	return quackmail::citadel::ResolveRoom(ctx.con, ctx.username, name, out);
}

bool ResolveRoomNumFor(Ctx &ctx, int64_t room_num, Room &out) {
	if (!ctx.Authed() || room_num < 0) {
		return false;
	}
	if (!quackmail::citadel::GetRoomByNum(ctx.con, room_num, out)) {
		return false;
	}
	// GetRoomByNum applies no visibility rules at all, so they are applied here
	// — the same ones ListRooms uses, or a user could read anyone's mailbox by
	// guessing a room number.
	int64_t usernum = quackmail::citadel::GetOrAssignUserNum(ctx.con, ctx.username);
	if (out.mailbox_owner != 0 && out.mailbox_owner != usernum) {
		return false; // someone else's personal room; not even an aide browses those
	}
	if (!ctx.IsAide() && (out.qr_flags & quackmail::citadel::QR_PRIVATE) && out.mailbox_owner != usernum) {
		// An invitation-only room is reachable by whoever the access list names,
		// which is the same rule ListRooms applies — `l` is RFC 4314's lookup
		// right. Without this a private room would be invisible even to the
		// person who created it and holds every right on it.
		return quackmail::citadel::EffectiveRights(ctx.con, ctx.username, out).find('l') !=
		       std::string::npos;
	}
	return true;
}

bool RequireUnlocked(Ctx &ctx, const Room &room, const std::string &back_path) {
	if (quackmail::citadel::RoomUnlocked(ctx.con, ctx.username, room)) {
		return true;
	}
	std::string body = "<p class=\"muted\">This room is password protected.</p>";
	body += FormStart(ctx, "/bbs/room/" + std::to_string(room.room_num) + "/unlock");
	body += Hidden("next", back_path);
	body += "<label class=\"field\"><span>Room password</span>" + TextInput("password", "", "password") +
	        "</label>";
	body += "<p>" + Button("Enter room") + "</p>";
	body += FormEnd();
	Render(ctx, room.display_name, body, 403);
	return false;
}

bool LoadMessageIn(Ctx &ctx, const Room &room, int64_t msgnum, Message &out) {
	// The membership check is the access control. citadel::LoadMessage takes a
	// bare message number and knows nothing about who may read it, so a handler
	// that skips this reads anyone's mail.
	if (msgnum <= 0 || !quackmail::citadel::MessageInRoom(ctx.con, room.room_num, msgnum)) {
		return false;
	}
	return quackmail::citadel::LoadMessage(ctx.con, msgnum, out);
}

std::string EffectiveTz(Ctx &ctx) {
	std::string want;
	if (ctx.Authed()) {
		want = quackmail::citadel::GetUserPref(ctx.con, ctx.username, "web_tz");
	}
	if (want.empty()) {
		want = ConfigStr(ctx.con, "qm_default_tz", "UTC");
	}
	// A pref naming a zone the bundled database has never heard of — a stale
	// name, a typo set by hand — falls back rather than rendering nothing.
	if (want.empty() || !quackmail::tz::IsKnown(want)) {
		return "UTC";
	}
	return want;
}

std::string EffectiveDateFormat(Ctx &ctx) {
	std::string want;
	if (ctx.Authed()) {
		want = quackmail::citadel::GetUserPref(ctx.con, ctx.username, "web_date_format");
	}
	if (want.empty()) {
		want = ConfigStr(ctx.con, "qm_default_date_format", "iso");
	}
	if (want != "us" && want != "eu") {
		return "iso";
	}
	return want;
}

std::string FormatTimeIn(int64_t epoch_seconds, const std::string &tzid, const std::string &date_format) {
	if (epoch_seconds <= 0) {
		return "—";
	}
	// Break the wall clock down here rather than through localtime_r, which
	// would answer in the server's zone regardless of what was asked for.
	int64_t wall = tzid.empty() ? epoch_seconds : quackmail::tz::FromUtc(tzid, epoch_seconds);
	int64_t days = wall / 86400;
	int64_t rem = wall % 86400;
	if (rem < 0) {
		rem += 86400;
		days -= 1;
	}
	// Days-since-epoch to a civil date, via gmtime_r on a value already shifted
	// into the target zone — so the "UTC" it reports is the local wall clock.
	std::time_t t = (std::time_t)(days * 86400);
	struct tm tm {};
	gmtime_r(&t, &tm);
	int year = tm.tm_year + 1900, mon = tm.tm_mon + 1, day = tm.tm_mday;
	int hour = (int)(rem / 3600), minute = (int)((rem % 3600) / 60);
	char buf[48];
	if (date_format == "us") {
		std::snprintf(buf, sizeof buf, "%02d/%02d/%04d %02d:%02d", mon, day, year, hour, minute);
	} else if (date_format == "eu") {
		std::snprintf(buf, sizeof buf, "%02d/%02d/%04d %02d:%02d", day, mon, year, hour, minute);
	} else {
		std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", year, mon, day, hour, minute);
	}
	return buf;
}

std::string FormatTime(Ctx &ctx, int64_t epoch_seconds) {
	return FormatTimeIn(epoch_seconds, EffectiveTz(ctx), EffectiveDateFormat(ctx));
}

std::string FormatBytes(int64_t bytes) {
	char buf[32];
	if (bytes < 1024) {
		std::snprintf(buf, sizeof buf, "%lld B", (long long)bytes);
	} else if (bytes < 1024 * 1024) {
		std::snprintf(buf, sizeof buf, "%.1f KB", bytes / 1024.0);
	} else {
		std::snprintf(buf, sizeof buf, "%.1f MB", bytes / (1024.0 * 1024.0));
	}
	return buf;
}

std::string DecodeHeader(const std::string &raw) {
	// Decode first. Escaping happens later, at interpolation: doing it the
	// other way round lets an encoded-word decode back into a live tag after
	// the escaping has already run.
	return quackmail::mime::DecodeEncodedWords(raw);
}

} // namespace qmweb
} // namespace duckdb
