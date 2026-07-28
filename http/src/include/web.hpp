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
	const char *method;
	const char *pattern;
	Role role;
	Handler fn;
};

// Each source file contributes its routes; web_router.cpp assembles them.
void RegisterAuthRoutes(std::vector<Route> &out);
void RegisterBbsRoutes(std::vector<Route> &out);
void RegisterMailRoutes(std::vector<Route> &out);
void RegisterPrefsRoutes(std::vector<Route> &out);
void RegisterAdminUserRoutes(std::vector<Route> &out);
void RegisterAdminPolicyRoutes(std::vector<Route> &out);
void RegisterAdminOpsRoutes(std::vector<Route> &out);

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

// Render a complete page: security headers, the shell, nav, flash, and `body`.
void Render(Ctx &ctx, const std::string &title, const std::string &body, int status = 200);
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

// The canonical URL of a room, and the rendered view of one message (headers,
// body, sandboxed HTML alternative, attachment list). Both defined in
// web_bbs.cpp and reused by webmail.
std::string RoomHref(const quackmail::citadel::Room &room, const std::string &suffix = "");
std::string RenderMessage(Ctx &ctx, const quackmail::citadel::Room &room,
                          const quackmail::citadel::Message &msg);

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

// Human-readable helpers used across pages.
std::string FormatTime(int64_t epoch_seconds);
std::string FormatBytes(int64_t bytes);
// The subject/author of a message, RFC 2047-decoded. Decode first, escape
// second: the other order lets "=?utf-8?B?PHNjcmlwdD4=?=" turn back into a tag
// after escaping has already run.
std::string DecodeHeader(const std::string &raw);

} // namespace qmweb
} // namespace duckdb
