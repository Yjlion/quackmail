#pragma once

#include "duckdb.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/http.hpp"
#include "quackmail/websession.hpp"

#include <string>
#include <utility>
#include <vector>

// The shared contract for the web front-end.
//
// Unlike every other module, this one spans several translation units, so the
// handlers cannot live in an anonymous namespace. They live in
// `duckdb::qmweb` instead — a named namespace unique to this module. That is
// not a style choice: build_static_extension links every extension's static
// library into DuckDB's unittest binary (the same reason core is compiled
// exactly once), so a generic external symbol like `duckdb::Page` would
// collide with another extension's.

namespace duckdb {
namespace qmweb {

namespace http = quackmail::http;

// What a route requires of the caller. Enforced by the router, not by handlers,
// so a newly added admin route physically cannot forget its gate.
enum class Role {
	Anon, // no session needed
	User, // any signed-in user
	Aide, // axlevel >= 6, plus the admin-area TLS and enable gates
	// A programmatic client rather than a browser: CalDAV, CardDAV, JMAP. Same
	// credentials as IMAP, presented as HTTP Basic, and answered with 401 rather
	// than a redirect to the login page. Exempt from CSRF, which a client that
	// has never seen an HTML form cannot possibly satisfy — and does not need
	// to, because Basic credentials are not ambient authority the way a cookie
	// is, so there is nothing for a cross-site request to ride.
	Api,
};

struct Ctx {
	Connection &con;
	const http::Request &req;
	http::Response &resp;

	std::string username; // "" when anonymous
	int64_t axlevel = 0;
	std::string session_hash; // sha256 of the session token, "" when anonymous
	std::string csrf;         // raw CSRF token, for rendering into forms
	std::string nonce;        // per-response CSP nonce
	bool tls = false;         // request arrived over TLS (or a trusted proxy said so)
	std::vector<std::string> captures;

	Ctx(Connection &c, const http::Request &rq, http::Response &rs);

	bool Authed() const {
		return !username.empty();
	}
	bool IsAide() const {
		return axlevel >= 6;
	}
	// Path capture by index; "" when absent, so a handler never indexes past
	// the end of a pattern that changed underneath it.
	std::string Cap(size_t i) const;
	// A query parameter parsed as an integer, with a default.
	int64_t ParamInt(const std::string &name, int64_t dflt) const;
	int64_t FormInt(const std::string &name, int64_t dflt) const;
};

using Handler = void (*)(Ctx &);

struct Route {
	// The HTTP method, or "*" for a route that takes every verb and dispatches
	// them itself. Only the DAV subtree uses "*": ten verbs would otherwise be
	// ten entries in a table that is scanned linearly on every request.
	const char *method;
	const char *pattern;
	Role role;
	Handler fn;
};

// Each source file contributes its routes; web_router.cpp assembles them.
void RegisterStaticRoutes(std::vector<Route> &out);
void RegisterAuthRoutes(std::vector<Route> &out);
void RegisterBbsRoutes(std::vector<Route> &out);
// Per-room management and self-serve creation. Ordinary user routes, gated on
// citadel::CanAdminister rather than on the Aide role — see web_rooms.cpp.
void RegisterRoomAdminRoutes(std::vector<Route> &out);
void RegisterMailRoutes(std::vector<Route> &out);
// Message search across every room the caller can read. Defined in
// web_search.cpp.
void RegisterSearchRoutes(std::vector<Route> &out);
void RegisterPrefsRoutes(std::vector<Route> &out);
void RegisterAdminUserRoutes(std::vector<Route> &out);
void RegisterAdminPolicyRoutes(std::vector<Route> &out);
void RegisterAdminOpsRoutes(std::vector<Route> &out);
// Also contributes the anonymous /lists self-service pages, which live beside
// the admin ones because they share the same model.
void RegisterAdminListRoutes(std::vector<Route> &out);
// CalDAV and CardDAV over the groupware rooms, plus the .well-known redirects
// that let a client find them. Defined in dav_router.cpp.
void RegisterDavRoutes(std::vector<Route> &out);
// JMAP: the Session resource, the method-call endpoint and blob download.
// Defined in jmap_session.cpp.
void RegisterJmapRoutes(std::vector<Route> &out);

// Handle one already-parsed request: resolve the session, apply the transport
// and role gates, verify CSRF, dispatch. Always fills `resp`.
void Dispatch(Connection &con, const http::Request &req, http::Response &resp);

// Run a parameterized statement; nullptr on error. Params are taken **by
// value** on purpose: PreparedStatement::Execute binds a non-const reference,
// so a brace list at the call site would not compile against it directly.
// Same shape as the ExecP helpers each core source file keeps.
unique_ptr<QueryResult> Exec(Connection &con, const std::string &sql, vector<Value> params);

// ---- configuration -------------------------------------------------------
// All of these live in citadel_config, so `quackcitadm.sh config set` is the
// operator interface for them.
std::string ConfigStr(Connection &con, const char *name, const std::string &dflt);
bool ConfigBool(Connection &con, const char *name, bool dflt);

constexpr const char *kSessionCookie = "qcsid";

// ---- page chrome ---------------------------------------------------------
//
// Escaping is the default and the unsafe path has the alarming name. Message
// bodies and headers arrive from inbound SMTP — that is, from anyone on the
// internet — so forgetting to escape must require typing something that looks
// wrong in review.

// Escape text for element content / attribute values.
std::string T(const std::string &text);
std::string A(const std::string &value);
// Pass HTML through unescaped. Only for markup this module generated itself.
std::string RawHtml(const std::string &html);

// Small builders. Every one of these escapes its arguments.
std::string Cell(const std::string &text, const std::string &css_class = "");
std::string Head(const std::string &text);
std::string Link(const std::string &href, const std::string &label, const std::string &css_class = "");
std::string TextInput(const std::string &name, const std::string &value, const std::string &type = "text",
                      const std::string &placeholder = "");
std::string TextArea(const std::string &name, const std::string &value, int rows = 12);
std::string Hidden(const std::string &name, const std::string &value);
std::string Checkbox(const std::string &name, bool checked, const std::string &label);
std::string Select(const std::string &name, const std::vector<std::pair<std::string, std::string>> &options,
                   const std::string &selected);
// A <form method="post"> with the session's CSRF token already inside it.
std::string FormStart(const Ctx &ctx, const std::string &action, const std::string &css_class = "");
std::string FormEnd();
std::string Button(const std::string &label, const std::string &css_class = "");

// How the shell should present a page. Everything is optional; the default is
// what every page rendered before this struct existed.
struct PageOpts {
	// Sidebar item to mark with aria-current, e.g. "mail", "bbs", "prefs".
	std::string active;
	// A RoomView code, surfaced as a body class so a view can be styled without
	// its handler having to emit layout. -1 for pages that are not a room.
	int view = -1;
	// Drop the 62rem measure. Message lists and calendars want the width; prose
	// and forms do not.
	bool wide = false;
	// A per-page action strip rendered by the shell, above `body`.
	std::string toolbar;
	// An extra script from /static, by logical name ("qc-compose.js"), loaded
	// deferred after the shared one. Pages that need no script leave it empty,
	// which is most of them.
	std::string script;

	PageOpts();
};

// Render a complete page: security headers, the shell, nav, flash, and `body`.
// The short form is the whole-page default and is what most handlers want; the
// PageOpts form is for pages that need to say more about themselves.
void Render(Ctx &ctx, const std::string &title, const std::string &body, int status = 200);
void Render(Ctx &ctx, const std::string &title, const std::string &body, const PageOpts &opts,
            int status = 200);
// Apply the standard security headers to a response that is not a full page
// (a redirect, an attachment, an error). `csp` overrides the default policy.
void SecurityHeaders(Ctx &ctx, const std::string &csp = "");

// Redirect to a same-site path, optionally with a flash slug.
void RedirectTo(Ctx &ctx, const std::string &path, const std::string &flash = "");
// The human text for a flash slug, or "" when the slug is not one of ours.
// Flashes are slugs and never free text: reflecting a query parameter into a
// page is a text-injection vector even when escaped.
std::string FlashText(const std::string &slug);

void NotFound(Ctx &ctx);
void Forbidden(Ctx &ctx, const std::string &why);
void BadRequest(Ctx &ctx, const std::string &why);
void ErrorPage(Ctx &ctx, int status, const std::string &title, const std::string &detail);

// ---- shared helpers ------------------------------------------------------

// Resolve a room the signed-in user is allowed to see. Always use this rather
// than GetRoomByNum on a path parameter: ResolveRoom applies the private-room
// visibility rules, GetRoomByNum does not.
bool ResolveRoomFor(Ctx &ctx, const std::string &name, quackmail::citadel::Room &out);
// The same, for a room number out of a URL. Rooms are addressed by number in
// URLs rather than by name because a Citadel room name may contain a '/', which
// no amount of percent-encoding survives once the path has been split.
bool ResolveRoomNumFor(Ctx &ctx, int64_t room_num, quackmail::citadel::Room &out);
// True when the room is readable — i.e. it is not a passworded room the user
// has yet to unlock. Renders the unlock form into `resp` and returns false when
// it is not, so callers can simply `if (!RequireUnlocked(...)) return;`.
bool RequireUnlocked(Ctx &ctx, const quackmail::citadel::Room &room, const std::string &back_path);
// Load a message *after* confirming it is pointed into `room`. LoadMessage has
// no notion of ownership, so skipping this check is a direct IDOR.
bool LoadMessageIn(Ctx &ctx, const quackmail::citadel::Room &room, int64_t msgnum,
                   quackmail::citadel::Message &out);

// May this user create rooms of their own? `qm_room_create_axlevel` names the
// access level it takes, defaulting to the aide level so nothing changes on an
// existing server until an operator lowers it. Defined in web_rooms.cpp and
// used by the sidebar as well as by the routes themselves.
bool MayCreateRooms(const Ctx &ctx);

// The canonical URL of a room, and the rendered view of one message (headers,
// body, sandboxed HTML alternative, attachment list). Both defined in
// web_bbs.cpp and reused by webmail.
std::string RoomHref(const quackmail::citadel::Room &room, const std::string &suffix = "");
std::string RenderMessage(Ctx &ctx, const quackmail::citadel::Room &room,
                          const quackmail::citadel::Message &msg);

// The signed-in user's mail folders — what a move may target, what the sidebar
// lists. Personal rooms minus the groupware four, in Citadel's provisioning
// order. Defined in web_mailbox.cpp and shared with the read pane and the
// sidebar, so no two of them can disagree about what a folder is.
std::vector<quackmail::citadel::Room> MailFolders(const Ctx &ctx);
// The same filter over a listing already in hand, for a caller that has one.
std::vector<quackmail::citadel::Room>
MailFoldersFrom(const std::vector<quackmail::citadel::Room> &rooms);
// Messages without \Seen, one count per room, in a single query. This is what a
// mail folder means by unread — the same flag the listing bolds a row on.
std::vector<int64_t> UnseenCounts(const Ctx &ctx, const std::vector<int64_t> &room_nums);

// Strip scripting out of a sender-supplied text/html part. Defence in depth
// only — the actual boundary is the sandboxed frame it is served into and that
// route's own `default-src 'none'` policy. Defined in web_mail.cpp.
std::string SanitizeHtmlPart(const std::string &html);

// ---- admin chrome --------------------------------------------------------
// Defined in web_admin_users.cpp and used by the other two admin files.

// Re-authenticate the signed-in aide against the form's `admin_password`. The
// sharpest actions require it, so a stolen session alone cannot mint
// credentials, rewrite configuration or generate signing keys.
bool ReAuth(Ctx &ctx);
std::string ReAuthField();
void ReAuthFailed(Ctx &ctx);
std::string AdminNav();
void AdminPage(Ctx &ctx, const std::string &title, const std::string &body);

// The named colour themes, as (value, label) pairs for a <select>. The theme in
// force is applied by Render; this is only for building the pickers.
std::vector<std::pair<std::string, std::string>> ThemeOptions();

// Record an administrative action in the Aide room, attributed to the operator
// who performed it. Call it only after the action has actually succeeded.
void AideLog(Ctx &ctx, const std::string &subject, const std::string &detail);

// Sieve script editing and the browser-session table, shared between the user's
// own preferences and the admin console's view of any user. Defined in
// web_prefs.cpp.
std::string SieveSection(Ctx &ctx, const std::string &user, const std::string &action_prefix);
bool SieveSave(Ctx &ctx, const std::string &user, const std::string &name, const std::string &script,
               std::string &err);
void SieveActivate(Ctx &ctx, const std::string &user, const std::string &name);
std::string WebSessionTable(Ctx &ctx, const std::vector<quackmail::web::SessionRow> &rows,
                            const std::string &action, bool show_user);

// The time zone this user's times should be rendered in: their `web_tz`
// preference, else the site's `qm_default_tz`, else UTC. Never empty and always
// a zone the bundled database knows.
std::string EffectiveTz(Ctx &ctx);

// Human-readable helpers used across pages.
//
// Times render in the *viewer's* zone, which is why this needs the Ctx. It used
// to call localtime_r and so showed every timestamp in whatever zone the server
// process happened to be in — invisible to an operator whose server and desk are
// in the same place, and wrong for everyone else.
std::string FormatTime(Ctx &ctx, int64_t epoch_seconds);
// The same, in an explicit zone. Pass "" for UTC.
std::string FormatTimeIn(int64_t epoch_seconds, const std::string &tzid);
std::string FormatBytes(int64_t bytes);
// The subject/author of a message, RFC 2047-decoded. Decode first, escape
// second: the other order lets "=?utf-8?B?PHNjcmlwdD4=?=" turn back into a tag
// after escaping has already run.
std::string DecodeHeader(const std::string &raw);

} // namespace qmweb
} // namespace duckdb
