#include "web.hpp"

#include "quackmail/mime.hpp"

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

// One stylesheet, inlined. There are no external assets at all: no CDN, no
// build step, and one request per page — which is also why the connection is
// closed after every response.
const char *kCss = R"CSS(
:root { color-scheme: light dark;
  --bg:#fbfbfa; --fg:#1c1b19; --muted:#6b6a66; --line:#e0dfdb;
  --panel:#ffffff; --accent:#8a5a2b; --accent-fg:#ffffff; --warn:#8c2f00; --ok:#1f6b3a; }
@media (prefers-color-scheme: dark) { :root {
  --bg:#17171a; --fg:#e9e8e4; --muted:#9d9c97; --line:#2e2e33;
  --panel:#1f1f23; --accent:#c98d54; --accent-fg:#17171a; --warn:#ff9b7a; --ok:#7ecf9a; } }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--fg); font:15px/1.55 ui-sans-serif,system-ui,"Segoe UI",Roboto,sans-serif; }
a { color:var(--accent); }
header.top { display:flex; flex-wrap:wrap; align-items:baseline; gap:.75rem 1.25rem;
  padding:.7rem 1.1rem; border-bottom:1px solid var(--line); background:var(--panel); }
header.top .brand { font-weight:700; letter-spacing:.02em; }
header.top nav { display:flex; flex-wrap:wrap; gap:.9rem; }
header.top nav a { text-decoration:none; }
header.top .who { margin-left:auto; color:var(--muted); font-size:.9em; display:flex; gap:.6rem; align-items:baseline; }
main { max-width:62rem; margin:0 auto; padding:1.2rem 1.1rem 4rem; }
h1 { font-size:1.35rem; margin:.2rem 0 1rem; }
h2 { font-size:1.05rem; margin:1.6rem 0 .6rem; }
.flash { padding:.6rem .8rem; border-left:3px solid var(--ok); background:var(--panel); margin-bottom:1rem; }
.err { border-left-color:var(--warn); }
.muted { color:var(--muted); }
.warnbar { padding:.5rem .8rem; background:var(--panel); border-left:3px solid var(--warn); margin-bottom:1rem; font-size:.92em; }
table { width:100%; border-collapse:collapse; margin:.4rem 0 1rem; }
th,td { text-align:left; padding:.42rem .55rem; border-bottom:1px solid var(--line); vertical-align:top; }
th { font-size:.78em; text-transform:uppercase; letter-spacing:.06em; color:var(--muted); font-weight:600; }
tr.unread td { font-weight:600; }
td.num,th.num { text-align:right; font-variant-numeric:tabular-nums; }
.wrap { overflow-x:auto; }
form { margin:.5rem 0; }
form.inline { display:inline; }
input[type=text],input[type=password],input[type=number],input[type=email],textarea,select {
  font:inherit; padding:.4rem .5rem; border:1px solid var(--line); border-radius:3px;
  background:var(--bg); color:var(--fg); max-width:100%; }
textarea { width:100%; font-family:ui-monospace,SFMono-Regular,Menlo,monospace; font-size:.92em; }
label.field { display:block; margin:.55rem 0; }
label.field span { display:block; font-size:.8em; color:var(--muted); margin-bottom:.15rem; }
label.chk { display:inline-flex; gap:.35rem; align-items:center; margin:.2rem 1rem .2rem 0; }
.btn { font:inherit; padding:.4rem .8rem; border:1px solid var(--accent); border-radius:3px;
  background:var(--accent); color:var(--accent-fg); cursor:pointer; }
.btn.sec { background:transparent; color:var(--accent); }
.btn.danger { background:transparent; color:var(--warn); border-color:var(--warn); }
pre.body { white-space:pre-wrap; word-wrap:break-word; font-family:ui-monospace,SFMono-Regular,Menlo,monospace;
  font-size:.92em; background:var(--panel); padding:.8rem; border:1px solid var(--line); border-radius:3px; }
iframe.htmlpart { width:100%; min-height:24rem; border:1px solid var(--line); border-radius:3px; background:#fff; }
.msghead { background:var(--panel); border:1px solid var(--line); border-radius:3px; padding:.7rem .8rem; margin-bottom:.8rem; }
.msghead dl { display:grid; grid-template-columns:max-content 1fr; gap:.15rem .7rem; margin:0; }
.msghead dt { color:var(--muted); font-size:.85em; }
.msghead dd { margin:0; }
.actions { display:flex; flex-wrap:wrap; gap:.5rem; align-items:center; margin:.8rem 0; }
.roomgrid { display:grid; grid-template-columns:repeat(auto-fill,minmax(15rem,1fr)); gap:.4rem .9rem; }
.pager { display:flex; gap:.8rem; align-items:baseline; margin:.6rem 0; }
.login { max-width:22rem; margin:3rem auto; }
code { font-family:ui-monospace,SFMono-Regular,Menlo,monospace; font-size:.92em; }
)CSS";

std::string NavFor(const Ctx &ctx) {
	if (!ctx.Authed()) {
		return std::string();
	}
	std::string nav = "<nav>";
	nav += Link("/mail/", "Mail");
	nav += Link("/bbs/", "Rooms");
	nav += Link("/bbs/who", "Who");
	nav += Link("/prefs", "Preferences");
	if (ctx.IsAide() && ConfigBool(ctx.con, "qm_web_admin_enabled", false)) {
		nav += Link("/admin/", "Admin");
	}
	nav += "</nav>";
	return nav;
}

} // namespace

void SecurityHeaders(Ctx &ctx, const std::string &csp) {
	// default-src 'none' plus a per-response nonce, rather than 'unsafe-inline'.
	// The nonce is three lines of work and it is the difference between a
	// reflected-XSS bug being inert and being execution.
	std::string policy = csp;
	if (policy.empty()) {
		policy = "default-src 'none'; script-src 'nonce-" + ctx.nonce + "'; style-src 'nonce-" + ctx.nonce +
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
	SecurityHeaders(ctx);

	std::string node = ConfigStr(ctx.con, "c_humannode", "QuackCit");
	std::string page = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">";
	page += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
	page += "<title>" + T(title.empty() ? node : title + " — " + node) + "</title>";
	// The palette goes inside this same element: style-src is nonce-only with no
	// 'unsafe-inline', so a second, unnonced <style> would be dropped silently.
	page += "<style nonce=\"" + A(ctx.nonce) + "\">" + RawHtml(kCss) + RawHtml(ThemeCss(ctx)) +
	        "</style></head><body>";

	page += "<header class=\"top\"><span class=\"brand\">" + T(node) + "</span>";
	page += NavFor(ctx);
	if (ctx.Authed()) {
		page += "<span class=\"who\">" + T(ctx.username);
		page += FormStart(ctx, "/logout", "inline") + Button("Sign out", "sec") + FormEnd();
		page += "</span>";
	}
	page += "</header><main>";

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
	page += RawHtml(body);
	page += "</main></body></html>";

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
		return false;
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

std::string FormatTime(int64_t epoch_seconds) {
	if (epoch_seconds <= 0) {
		return "—";
	}
	std::time_t t = (std::time_t)epoch_seconds;
	struct tm tm {};
	localtime_r(&t, &tm);
	char buf[40];
	if (std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm) == 0) {
		return "—";
	}
	return buf;
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
