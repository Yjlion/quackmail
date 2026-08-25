#include "web_views.hpp"

#include "quackmail/citadel_msg.hpp"
#include "quackmail/diff.hpp"
#include "quackmail/html_sanitize.hpp"
#include "quackmail/markdown.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/util.hpp"
#include "quackmail/wiki.hpp"

#include <algorithm>
#include <set>

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Message;
using quackmail::citadel::Room;
namespace wiki = quackmail::wiki;
namespace mime = quackmail::mime;

namespace {

const char *kHtmlType = "text/html";
const char *kMarkdownType = "text/x-markdown";

// A page's URL. The name is a *query parameter* rather than a path segment
// because NormalizeName leaves '/' alone — a page really can be called
// "Protocols/NNTP" — and http::NormalizePath percent-decodes before the router
// splits on '/', so an encoded slash cannot survive as one capture. WebCit
// addresses pages the same way, for the same reason.
std::string WikiHref(const Room &room, const std::string &page, const char *suffix = "") {
	std::string href = RoomHref(room, std::string("/wiki") + suffix);
	if (!page.empty()) {
		href += "?page=" + quackmail::http::PercentEncode(page);
	}
	return href;
}

struct Loaded {
	int64_t msgnum = 0;
	std::string euid;
	std::string title;
	std::string body;         // the page source
	std::string content_type; // kHtmlType or kMarkdownType
	std::string author;
	int64_t msgtime = 0;
	std::string raw;
};

// Pull the page body out of the stored message. A wiki page is an ordinary
// format_type 4 message wrapping one part, exactly as a contact or an event is.
bool Decompose(const Message &msg, Loaded &out) {
	out.msgnum = msg.msgnum;
	out.euid = msg.euid;
	out.title = msg.subject.empty() ? msg.euid : msg.subject;
	out.author = msg.author;
	out.msgtime = msg.msgtime;
	out.raw = msg.raw;

	std::string md = qmweb::ObjectBody(msg, kMarkdownType);
	if (!md.empty()) {
		out.body = md;
		out.content_type = kMarkdownType;
		return true;
	}
	std::string html = qmweb::ObjectBody(msg, kHtmlType);
	if (!html.empty()) {
		out.body = html;
		out.content_type = kHtmlType;
		return true;
	}
	// A page written by something that did not use a part we recognise — or by
	// a Citadel client — is still a page. Serve its text rather than a 404.
	out.body = quackmail::citadel::BodyText(msg);
	out.content_type = kHtmlType;
	return !out.body.empty();
}

bool LoadPage(Ctx &ctx, const Room &room, const std::string &euid, Loaded &out) {
	int64_t msgnum = quackmail::citadel::FindByEuid(ctx.con, room.room_num, euid);
	if (msgnum <= 0) {
		return false;
	}
	Message msg;
	if (!quackmail::citadel::LoadMessage(ctx.con, msgnum, msg)) {
		return false;
	}
	return Decompose(msg, out);
}

std::string PageParam(Ctx &ctx) {
	return wiki::NormalizeName(ctx.req.Param("page"));
}

// Render a page body to safe HTML.
//
// Markdown is rendered here and HTML is sanitized here, and *both* go through
// SanitizeForCompose: what the Markdown renderer emits is generated markup, and
// generated markup gets the allow-list on the same terms as anything else this
// server stores and re-serves from its own origin.
std::string RenderBody(Ctx &ctx, const Room &room, const Loaded &page,
                       const std::set<std::string> &existing) {
	if (page.content_type != kMarkdownType) {
		return quackmail::html::SanitizeForCompose(page.body);
	}
	// Wiki links are **absolute**. SanitizeForCompose keeps href only for
	// http/https/mailto — it exists for markup arriving in mail, where a
	// relative URL means nothing — so a relative link would be stripped on the
	// way out and the wiki would have no working links at all.
	const std::string base = SelfBaseUrl(ctx);

	quackmail::markdown::Options opts;
	// A link to a page nobody has written yet points at the *create form*
	// rather than at a page that will 404 — following a red link should start
	// writing it, which is most of what makes a wiki a wiki.
	opts.link = [&room, &existing, &base](const std::string &name) {
		const std::string euid = wiki::NormalizeName(name);
		return base + (existing.count(euid) > 0 ? WikiHref(room, euid)
		                                        : WikiHref(room, euid, "/edit"));
	};
	opts.exists = [&existing](const std::string &name) {
		return existing.count(wiki::NormalizeName(name)) > 0;
	};
	std::string out =
	    quackmail::html::SanitizeForCompose(quackmail::markdown::Render(page.body, opts));

	// The sanitizer drops `class`, and rightly so — it is an allow-list for
	// markup a user typed, and site class names are not a user's to claim. So
	// the styling hook is added afterwards, as a targeted attribute rewrite over
	// already-sanitized markup, exactly as RewriteCidUrls does. The marker is
	// the destination, which we chose above and which no page body can forge:
	// a wanted link is one that goes to the create form.
	const std::string needle = "<a href=\"" + A(base + RoomHref(room, "/wiki/edit"));
	const std::string attr = " class=\"wanted\"";
	size_t at = 0;
	while ((at = out.find(needle, at)) != std::string::npos) {
		// After "<a", so the result is `<a class="wanted" href=...`. Inserting
		// without the leading space glues the attribute to the tag name and
		// produces an element the sanitizer then drops entirely.
		out.insert(at + 2, attr);
		at += needle.size() + attr.size();
	}
	return out;
}

std::set<std::string> PageNames(Ctx &ctx, const Room &room) {
	std::set<std::string> names;
	for (const wiki::Page &p : wiki::ListPages(ctx.con, room.room_num)) {
		names.insert(p.euid);
	}
	return names;
}

std::string Toolbar(Ctx &ctx, const Room &room, const std::string &page, bool exists) {
	const bool may_post = quackmail::citadel::CanPost(ctx.con, ctx.username, room);
	std::string bar = "<div class=\"actions\">";
	bar += Link(RoomHref(room), "All pages", "btn sec");
	if (!page.empty()) {
		if (may_post) {
			bar += Link(WikiHref(room, page, "/edit"), exists ? "Edit" : "Create", "btn");
		}
		if (exists) {
			bar += Link(WikiHref(room, page, "/history"), "History", "btn sec");
		}
	} else if (may_post) {
		bar += Link(RoomHref(room, "/wiki/edit"), "New page", "btn");
	}
	bar += Link(RoomHref(room) + "?view=raw", "View as messages", "btn sec");
	bar += "</div>";
	return bar;
}

// ---- the page index --------------------------------------------------------

void Index(Ctx &ctx, const Room &room) {
	std::vector<wiki::Page> pages = wiki::ListPages(ctx.con, room.room_num);

	std::string body;
	if (pages.empty()) {
		body += "<p class=\"muted\">This wiki has no pages yet.</p>";
		if (quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
			body += "<p>" + Link(WikiHref(room, "home", "/edit"), "Start the front page", "btn") +
			        "</p>";
		}
	} else {
		// Recently changed first, because that is what a reader coming back
		// wants; the alphabetical index is below it.
		std::vector<wiki::Page> recent = pages;
		std::sort(recent.begin(), recent.end(),
		          [](const wiki::Page &a, const wiki::Page &b) { return a.msgnum > b.msgnum; });
		if (recent.size() > 5) {
			recent.resize(5);
		}
		body += "<h2>Recently changed</h2><table class=\"list\"><tbody>";
		for (const wiki::Page &p : recent) {
			body += "<tr><td>" + Link(WikiHref(room, p.euid), p.title) + "</td>";
			body += Cell(FormatTime(ctx, p.msgtime), "num");
			body += Cell(p.author) + "</tr>";
		}
		body += "</tbody></table>";

		body += "<h2>All pages</h2><ul class=\"pagelist\">";
		for (const wiki::Page &p : pages) {
			body += "<li>" + Link(WikiHref(room, p.euid), p.title) + "</li>";
		}
		body += "</ul>";
	}

	PageOpts opts;
	opts.active = "bbs";
	opts.view = (int)room.default_view;
	opts.toolbar = Toolbar(ctx, room, "", false);
	Render(ctx, room.display_name, body, opts);
}

// ---- one page --------------------------------------------------------------

void Show(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, (int64_t)std::strtoll(ctx.Cap(0).c_str(), nullptr, 10), room)) {
		return;
	}
	if (!wiki::IsWikiView(room.default_view)) {
		NotFound(ctx);
		return;
	}
	const std::string page = PageParam(ctx);
	Loaded loaded;
	const bool exists = LoadPage(ctx, room, page, loaded);

	std::string body;
	if (!exists) {
		body += "<p class=\"muted\">There is no page called <code>" + T(page) +
		        "</code> yet.</p>";
		if (quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
			body += "<p>" + Link(WikiHref(room, page, "/edit"), "Write it", "btn") + "</p>";
		}
	} else {
		body += "<div class=\"wiki\">" + RawHtml(RenderBody(ctx, room, loaded, PageNames(ctx, room))) +
		        "</div>";
		body += "<p class=\"muted\">Last edited by " + T(loaded.author) + " on " +
		        T(FormatTime(ctx, loaded.msgtime)) + ".</p>";
	}

	PageOpts opts;
	opts.active = "bbs";
	opts.view = (int)room.default_view;
	opts.toolbar = Toolbar(ctx, room, page, exists);
	Render(ctx, exists ? loaded.title : page, body, opts, exists ? 200 : 404);
}

// ---- editing ---------------------------------------------------------------

void Edit(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, (int64_t)std::strtoll(ctx.Cap(0).c_str(), nullptr, 10), room)) {
		return;
	}
	if (!wiki::IsWikiView(room.default_view)) {
		NotFound(ctx);
		return;
	}
	if (!quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "You cannot add anything to this room.");
		return;
	}
	const std::string requested = ctx.req.Param("page");
	const std::string page = requested.empty() ? std::string() : wiki::NormalizeName(requested);
	Loaded loaded;
	const bool exists = !page.empty() && LoadPage(ctx, room, page, loaded);

	std::string body = FormStart(ctx, RoomHref(room, "/wiki/save"));
	body += "<label class=\"field\"><span>Page name</span>" +
	        TextInput("page", exists ? loaded.title : requested) + "</label>";
	body += "<p class=\"muted\">The name is lower-cased to form the page's identifier, so "
	        "<code>Front Page</code> and <code>front page</code> are the same page.</p>";
	body += "<label class=\"field\"><span>Format</span>" +
	        Select("format",
	               {{kHtmlType, "Formatted text (HTML)"}, {kMarkdownType, "Markdown"}},
	               exists ? loaded.content_type : std::string(kMarkdownType)) +
	        "</label>";
	// The *source* goes into the textarea, never the rendered HTML: round-
	// tripping generated markup back through the editor is how a page slowly
	// stops being the thing its author wrote.
	body += "<label class=\"field\"><span>Page</span>" + TextArea("body", loaded.body, 24) +
	        "</label>";
	body += "<p>" + Button(exists ? "Save" : "Create page") + " " +
	        Link(page.empty() ? RoomHref(room) : WikiHref(room, page), "Cancel") + "</p>";
	body += FormEnd();

	PageOpts opts;
	opts.active = "bbs";
	opts.view = (int)room.default_view;
	Render(ctx, exists ? "Edit " + loaded.title : "New page", body, opts);
}

void Save(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, (int64_t)std::strtoll(ctx.Cap(0).c_str(), nullptr, 10), room)) {
		return;
	}
	if (!wiki::IsWikiView(room.default_view)) {
		NotFound(ctx);
		return;
	}
	const std::string typed = ctx.req.Form("page");
	const std::string page = wiki::NormalizeName(typed);
	if (wiki::IsHistoryEuid(page)) {
		BadRequest(ctx, "A page cannot be called that: names ending in _HISTORY_ are reserved.");
		return;
	}
	std::string source = ctx.req.Form("body");
	std::string format = ctx.req.Form("format");
	if (format != kMarkdownType) {
		format = kHtmlType;
		// HTML is sanitized *before* it is stored, so what is kept is already
		// safe rather than depending on every future reader to clean it.
		source = quackmail::html::SanitizeForCompose(source);
	}
	if (source.empty()) {
		BadRequest(ctx, "A page needs some text.");
		return;
	}

	int status = 200;
	std::string err;
	// The title is what the author typed, so a page can be "Front Page" while
	// its identifier is "front page" — exactly what Citadel's save hook does
	// when it copies the euid into the subject.
	const std::string title = typed.empty() ? page : typed;
	if (SaveObjectRaw(ctx, room, page, title, format, source, status, err) < 0) {
		if (err == "no changes") {
			// Citadel refuses a save that changes nothing rather than storing an
			// empty change set. Say so instead of reporting it as a failure.
			RedirectTo(ctx, WikiHref(room, page), "unchanged");
			return;
		}
		if (status == 403) {
			Forbidden(ctx, err);
		} else {
			BadRequest(ctx, err);
		}
		return;
	}
	RedirectTo(ctx, WikiHref(room, page), "saved");
}

// ---- history, revisions, diffs ---------------------------------------------

void History(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, (int64_t)std::strtoll(ctx.Cap(0).c_str(), nullptr, 10), room)) {
		return;
	}
	if (!wiki::IsWikiView(room.default_view)) {
		NotFound(ctx);
		return;
	}
	const std::string page = PageParam(ctx);
	Loaded current;
	if (!LoadPage(ctx, room, page, current)) {
		NotFound(ctx);
		return;
	}
	std::vector<wiki::Revision> revs = wiki::History(ctx.con, room.room_num, page);

	std::string body = "<table class=\"list\"><thead><tr>" + Head("When") + Head("Who") +
	                   Head("") + "</tr></thead><tbody>";
	body += "<tr><td>" + T(FormatTime(ctx, current.msgtime)) + "</td><td>" + T(current.author) +
	        "</td><td>" + Link(WikiHref(room, page), "current") + "</td></tr>";
	for (const wiki::Revision &r : revs) {
		const std::string rev = std::to_string(r.rev);
		body += "<tr><td>" + T(FormatTime(ctx, r.timestamp)) + "</td><td>" + T(r.author) + "</td><td>";
		body += Link(WikiHref(room, page, "/rev") + "&rev=" + rev, "view");
		body += " " + Link(WikiHref(room, page, "/diff") + "&rev=" + rev, "changes");
		if (quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
			body += " " + FormStart(ctx, RoomHref(room, "/wiki/revert"), "inline") +
			        Hidden("page", page) + Hidden("rev", rev) +
			        "<button class=\"btn sec\" data-confirm=\"Restore this revision?\">restore"
			        "</button>" +
			        FormEnd();
		}
		body += "</td></tr>";
	}
	body += "</tbody></table>";
	if (revs.empty()) {
		body += "<p class=\"muted\">This page has not been edited since it was created.</p>";
	}

	PageOpts opts;
	opts.active = "bbs";
	opts.view = (int)room.default_view;
	opts.toolbar = Toolbar(ctx, room, page, true);
	Render(ctx, "History of " + current.title, body, opts);
}

// Reconstruct one revision and hand back the page it held.
bool LoadRevision(Ctx &ctx, const Room &room, const std::string &page, int64_t rev, Loaded &out,
                  std::string &err) {
	std::string raw;
	if (!wiki::RevisionRaw(ctx.con, room.room_num, page, rev, raw, err)) {
		return false;
	}
	Message msg;
	msg.raw = raw;
	msg.format_type = 4;
	msg.euid = page;
	// The reconstructed bytes are a whole RFC822 message, so the subject and
	// author come from its own headers rather than from the current version.
	mime::MimeEntity entity = mime::ParseEntity(raw);
	for (const auto &h : entity.headers) {
		const std::string name = quackmail::util::Lower(h.first);
		if (name == "subject") {
			msg.subject = h.second;
		} else if (name == "from") {
			msg.author = h.second;
		}
	}
	Decompose(msg, out);
	return true;
}

void Revision(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, (int64_t)std::strtoll(ctx.Cap(0).c_str(), nullptr, 10), room)) {
		return;
	}
	if (!wiki::IsWikiView(room.default_view)) {
		NotFound(ctx);
		return;
	}
	const std::string page = PageParam(ctx);
	const int64_t rev = ctx.ParamInt("rev", 0);
	Loaded old;
	std::string err;
	if (!LoadRevision(ctx, room, page, rev, old, err)) {
		NotFound(ctx);
		return;
	}
	std::string body =
	    "<div class=\"warnbar\">This is an old revision of this page. " +
	    Link(WikiHref(room, page), "See the current one") + ".</div>";
	body += "<div class=\"wiki\">" + RawHtml(RenderBody(ctx, room, old, PageNames(ctx, room))) +
	        "</div>";

	PageOpts opts;
	opts.active = "bbs";
	opts.view = (int)room.default_view;
	opts.toolbar = Toolbar(ctx, room, page, true);
	Render(ctx, old.title + " (revision " + std::to_string(rev) + ")", body, opts);
}

void Changes(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, (int64_t)std::strtoll(ctx.Cap(0).c_str(), nullptr, 10), room)) {
		return;
	}
	if (!wiki::IsWikiView(room.default_view)) {
		NotFound(ctx);
		return;
	}
	const std::string page = PageParam(ctx);
	const int64_t rev = ctx.ParamInt("rev", 0);
	Loaded current;
	Loaded old;
	std::string err;
	if (!LoadPage(ctx, room, page, current) || !LoadRevision(ctx, room, page, rev, old, err)) {
		NotFound(ctx);
		return;
	}

	// Forwards, for reading: the stored diffs run backwards because that is how
	// the chain reconstructs, but "what changed since then" is old -> new.
	const std::string patch = quackmail::diff::Unified(old.body, current.body);
	std::string body = "<pre class=\"diff\">";
	size_t at = 0;
	while (at < patch.size()) {
		size_t nl = patch.find('\n', at);
		std::string line = patch.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
		const char *cls = "ctx";
		if (!line.empty() && line[0] == '+') {
			cls = "add";
		} else if (!line.empty() && line[0] == '-') {
			cls = "del";
		} else if (line.compare(0, 2, "@@") == 0) {
			cls = "hunk";
		}
		// No newline after the span: it is display:block inside a <pre>, so a
		// literal newline would be a second line break on every row.
		body += "<span class=\"" + std::string(cls) + "\">" + T(line) + "</span>";
		if (nl == std::string::npos) {
			break;
		}
		at = nl + 1;
	}
	body += "</pre>";
	if (patch.empty()) {
		body = "<p class=\"muted\">Nothing changed between that revision and the current one.</p>";
	}

	PageOpts opts;
	opts.active = "bbs";
	opts.view = (int)room.default_view;
	opts.toolbar = Toolbar(ctx, room, page, true);
	Render(ctx, "Changes to " + current.title, body, opts);
}

void Revert(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, (int64_t)std::strtoll(ctx.Cap(0).c_str(), nullptr, 10), room)) {
		return;
	}
	if (!wiki::IsWikiView(room.default_view)) {
		NotFound(ctx);
		return;
	}
	const std::string page = wiki::NormalizeName(ctx.req.Form("page"));
	const int64_t rev = ctx.FormInt("rev", 0);
	Loaded old;
	std::string err;
	if (!LoadRevision(ctx, room, page, rev, old, err)) {
		BadRequest(ctx, err.empty() ? "That revision could not be reconstructed." : err);
		return;
	}
	int status = 200;
	// Saved as a new revision rather than by rewriting history: a restore is an
	// edit like any other, and it must be undoable in turn.
	if (SaveObjectRaw(ctx, room, page, old.title, old.content_type, old.body, status, err) < 0) {
		if (err == "no changes") {
			RedirectTo(ctx, WikiHref(room, page), "unchanged");
			return;
		}
		if (status == 403) {
			Forbidden(ctx, err);
		} else {
			BadRequest(ctx, err);
		}
		return;
	}
	RedirectTo(ctx, WikiHref(room, page), "restored");
}

void Remove(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, (int64_t)std::strtoll(ctx.Cap(0).c_str(), nullptr, 10), room)) {
		return;
	}
	if (!wiki::IsWikiView(room.default_view)) {
		NotFound(ctx);
		return;
	}
	if (!quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "You cannot change this room.");
		return;
	}
	const std::string page = wiki::NormalizeName(ctx.req.Form("page"));
	std::string err;
	if (!wiki::DeletePage(ctx.con, room.room_num, page, err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, RoomHref(room), "deleted");
}

// Only `index` is filled. A wiki page is identified by its *name*, and a
// permalink to a message number breaks on every edit because UpsertByEuid mints
// a new one — so the shared /item/:m routes are the wrong address for it.
const RoomViewHandler kWiki = {quackmail::citadel::VIEW_WIKI, "Wiki",  "page",
                               Index,                         nullptr, nullptr,
                               nullptr,                       nullptr};

} // namespace

const RoomViewHandler &WikiView() {
	return kWiki;
}

void RegisterWikiRoutes(std::vector<Route> &out) {
	// Literal segments first: the table is scanned linearly.
	out.push_back({"GET", "/bbs/room/:n/wiki/edit", Role::User, Edit});
	out.push_back({"GET", "/bbs/room/:n/wiki/history", Role::User, History});
	out.push_back({"GET", "/bbs/room/:n/wiki/rev", Role::User, Revision});
	out.push_back({"GET", "/bbs/room/:n/wiki/diff", Role::User, Changes});
	out.push_back({"POST", "/bbs/room/:n/wiki/save", Role::User, Save});
	out.push_back({"POST", "/bbs/room/:n/wiki/revert", Role::User, Revert});
	out.push_back({"POST", "/bbs/room/:n/wiki/delete", Role::User, Remove});
	out.push_back({"GET", "/bbs/room/:n/wiki", Role::User, Show});
}

} // namespace qmweb
} // namespace duckdb
