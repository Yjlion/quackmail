#include "web.hpp"
#include "web_views.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/mailpolicy.hpp"
#include "quackmail/util.hpp"
#include "quackmail/wildmat.hpp"

#include <cstdlib>
#include <utility>

namespace duckdb {
namespace qmweb {

namespace {

const std::vector<Route> &Routes() {
	// Assembled once. Function-local statics are initialized thread-safely, and
	// the table is read-only afterwards, so the connection threads can share it.
	static const std::vector<Route> table = [] {
		std::vector<Route> r;
		// First: it is the most requested path on the site by a wide margin,
		// and matching is a linear scan.
		RegisterStaticRoutes(r);
		RegisterAuthRoutes(r);
		RegisterMailRoutes(r);
		RegisterBbsRoutes(r);
		// After the BBS routes: these share its /bbs/room/:n prefix.
		RegisterRoomAdminRoutes(r);
		RegisterViewRoutes(r);
		RegisterPrefsRoutes(r);
		RegisterAdminUserRoutes(r);
		RegisterAdminPolicyRoutes(r);
		RegisterAdminOpsRoutes(r);
		RegisterAdminListRoutes(r);
		// Last: /dav/ collides with no page above it, and a DAV client is a rare
		// visitor next to a browser loading a page's assets.
		RegisterDavRoutes(r);
		return r;
	}();
	return table;
}

// Does this peer sit behind a proxy we have been told to believe?
bool PeerIsTrustedProxy(Connection &con, const std::string &peer_ip) {
	return quackmail::policy::IpMatchesAny(peer_ip, ConfigStr(con, "qm_web_trusted_proxies", ""));
}

// Whether the request should be treated as encrypted. A forwarded header is
// honoured only from a peer on the configured proxy list — taking it from
// anyone would let a client simply claim TLS and defeat every gate below.
bool EffectiveTls(Connection &con, const http::Request &req) {
	if (req.tls) {
		return true;
	}
	if (!PeerIsTrustedProxy(con, req.peer_ip)) {
		return false;
	}
	std::string proto = quackmail::util::Lower(req.Header("X-Forwarded-Proto"));
	return proto == "https";
}

// Cross-origin form posts. The CSRF token is the real defense; this is an
// opt-in second check, and deliberately does not fail when the header is absent
// (many legitimate requests have no Origin).
//
// It is off unless `qm_web_origins` names the hosts forms may be submitted
// from. There is no fallback to c_fqdn: this server is reached by whatever name
// or address its operator points at it — a LAN address, a container name, a
// tunnel — and pinning the form origin to one configured name meant every
// deployment that had not set c_fqdn to exactly that name rejected all of its
// own POSTs. Setting the allow-list is what turns the check on.
bool OriginAcceptable(Ctx &ctx) {
	std::string origin = ctx.req.Header("Origin");
	if (origin.empty()) {
		origin = ctx.req.Header("Referer");
		if (origin.empty()) {
			return true;
		}
	}
	std::string allowed = ConfigStr(ctx.con, "qm_web_origins", "");
	if (allowed.empty()) {
		return true;
	}
	// Reduce to scheme://host[:port].
	size_t scheme = origin.find("://");
	if (scheme == std::string::npos) {
		return false;
	}
	size_t host_start = scheme + 3;
	size_t slash = origin.find('/', host_start);
	std::string host = origin.substr(host_start, slash == std::string::npos ? std::string::npos
	                                                                       : slash - host_start);
	// An IPv6 literal is bracketed and full of colons, so the port has to come
	// off the brackets rather than the first colon — otherwise `[::1]:8443`
	// reduces to "[" and even loopback fails to match.
	if (!host.empty() && host.front() == '[') {
		size_t close = host.find(']');
		host = close == std::string::npos ? host.substr(1) : host.substr(1, close - 1);
	} else {
		size_t colon = host.find(':');
		if (colon != std::string::npos) {
			host = host.substr(0, colon);
		}
	}
	host = quackmail::util::Lower(host);

	// Loopback is always its own origin.
	if (host == "localhost" || host == "127.0.0.1" || host == "::1") {
		return true;
	}
	size_t pos = 0;
	while (pos <= allowed.size()) {
		size_t comma = allowed.find(',', pos);
		std::string entry = allowed.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
		while (!entry.empty() && entry.front() == ' ') {
			entry.erase(0, 1);
		}
		while (!entry.empty() && entry.back() == ' ') {
			entry.pop_back();
		}
		if (!entry.empty() && quackmail::WildmatMatch(host, quackmail::util::Lower(entry))) {
			return true;
		}
		if (comma == std::string::npos) {
			break;
		}
		pos = comma + 1;
	}
	return false;
}

// The submitted CSRF token. Request::Form parses the body as urlencoded, which
// a file upload is not, so a multipart body has to be cracked open here —
// otherwise every attachment upload would be rejected before its handler ran.
std::string SubmittedCsrf(const http::Request &req) {
	std::string ct = quackmail::util::Lower(req.Header("Content-Type"));
	if (ct.rfind("multipart/form-data", 0) != 0) {
		return req.Form("_csrf");
	}
	std::vector<std::pair<std::string, std::string>> fields;
	std::vector<http::FormFile> files;
	if (!http::ParseMultipart(req.Header("Content-Type"), req.body, fields, files)) {
		return std::string();
	}
	for (auto &f : fields) {
		if (f.first == "_csrf") {
			return f.second;
		}
	}
	return std::string();
}

// Authenticate a DAV client from its `Authorization: Basic` header.
//
// The same credential IMAP and POP3 take, checked the same way, with the same
// per-address failure gate the login form uses — a DAV endpoint that skipped it
// would be a password oracle sitting beside a rate-limited login page. No
// session row is created: a DAV request is self-contained, and minting a
// browser session per PROPFIND would fill the table for nothing.
//
// Cleartext needs no separate guard here. The qm_web_force_https check above
// runs first and redirects the whole request, so Basic over an unencrypted
// socket only happens in the supported reverse-proxy mode or where an operator
// has deliberately turned that gate off.
bool BasicAuth(Ctx &ctx) {
	std::string header = ctx.req.Header("Authorization");
	if (quackmail::util::Lower(header).rfind("basic ", 0) != 0) {
		return false;
	}
	std::string decoded;
	if (!quackmail::util::Base64Decode(header.substr(6), decoded)) {
		return false;
	}
	size_t colon = decoded.find(':');
	if (colon == std::string::npos) {
		return false;
	}
	std::string user = decoded.substr(0, colon);
	std::string pass = decoded.substr(colon + 1);
	if (user.empty()) {
		return false;
	}

	int64_t retry_after = 0;
	if (!quackmail::web::LoginAllowed(ctx.con, ctx.req.peer_ip, retry_after)) {
		return false;
	}
	if (!quackmail::auth::Verify(ctx.con, user, pass)) {
		quackmail::web::RecordLoginFailure(ctx.con, ctx.req.peer_ip);
		return false;
	}
	quackmail::web::ClearLoginFailures(ctx.con, ctx.req.peer_ip);

	// A client may be the first thing this account has ever used, and the
	// collections it is about to ask for are these rooms.
	quackmail::citadel::EnsureUserRooms(ctx.con, user);
	ctx.username = user;
	ctx.axlevel = quackmail::citadel::GetAxLevel(ctx.con, user);
	return true;
}

// Ask for credentials. Deliberately bodiless: the peer is a program, and an
// HTML error page would only be something for it to fail to parse.
void NeedsAuth(Ctx &ctx) {
	std::string realm = ConfigStr(ctx.con, "c_humannode", "QuackCit");
	// A realm is a quoted-string, so a configured value containing a quote or a
	// backslash would break the header rather than merely look odd.
	std::string safe;
	for (char c : realm) {
		if (c != '"' && c != '\\' && (unsigned char)c >= 0x20) {
			safe += c;
		}
	}
	ctx.resp.status = 401;
	ctx.resp.SetHeader("WWW-Authenticate", "Basic realm=\"" + safe + "\", charset=\"UTF-8\"");
	ctx.resp.SetHeader("Cache-Control", "no-store");
	ctx.resp.SetHeader("X-Content-Type-Options", "nosniff");
}

// The admin console can set config, mint credentials and mint DKIM keys — it is
// root-equivalent, and quackcit.conf already says as much about the admin
// socket, which at least is mode 0600 on a Unix path. So it is off until an
// operator turns it on, and then still gated on TLS and an optional network
// allow-list.
bool AdminReachable(Ctx &ctx, std::string &why) {
	if (!ConfigBool(ctx.con, "qm_web_admin_enabled", false)) {
		why = "The web admin console is disabled. An operator can enable it with "
		      "`quackcitadm.sh config set qm_web_admin_enabled 1`.";
		return false;
	}
	if (ConfigBool(ctx.con, "qm_web_admin_require_tls", true) && !ctx.tls) {
		why = "The admin console is only served over HTTPS.";
		return false;
	}
	std::string note;
	if (quackmail::policy::CheckAcl(ctx.con, "webadmin", ctx.req.peer_ip, note) ==
	    quackmail::policy::AclVerdict::Block) {
		why = "The admin console is not reachable from this network.";
		return false;
	}
	return true;
}

} // namespace

void Dispatch(Connection &con, const http::Request &req, http::Response &resp) {
	Ctx ctx(con, req, resp);
	ctx.tls = EffectiveTls(con, req);
	ctx.nonce = quackmail::util::RandomBase64Url(16);

	// /healthz is exempt from everything: it must answer for a load balancer
	// before there is a certificate, a session or a database row.
	if (req.path == "/healthz") {
		resp.Text("ok");
		resp.SetHeader("Cache-Control", "no-store");
		return;
	}

	// HTTPS by default. Turning this off is the supported reverse-proxy mode:
	// the proxy terminates TLS and says so through X-Forwarded-Proto.
	if (!ctx.tls && ConfigBool(con, "qm_web_force_https", true)) {
		std::string fqdn = ConfigStr(con, "c_fqdn", "");
		SecurityHeaders(ctx);
		if (fqdn.empty()) {
			// Never build the redirect out of the client's Host header — that
			// is an open redirect with extra steps.
			ErrorPage(ctx, 403, "HTTPS required",
			          "This server only serves the web interface over HTTPS, and no canonical host name "
			          "is configured to redirect to.");
			return;
		}
		resp.Redirect("https://" + fqdn + req.target, 301);
		return;
	}

	// Resolve the session before routing, so Role gates and page chrome agree.
	std::string cookie = http::CookieValue(req.Header("Cookie"), kSessionCookie);
	if (!cookie.empty()) {
		quackmail::web::Session sess;
		if (quackmail::web::LookupSession(con, cookie, ctx.tls, sess)) {
			ctx.username = sess.username;
			ctx.axlevel = sess.axlevel;
			ctx.session_hash = sess.token_hash;
			ctx.csrf = sess.csrf;
		} else {
			// Stale, revoked or transport-mismatched: clear it so the browser
			// stops presenting it.
			http::Cookie dead;
			dead.name = kSessionCookie;
			dead.value = "";
			dead.max_age = 0;
			dead.secure = ctx.tls;
			resp.AddHeader("Set-Cookie", http::SerializeCookie(dead));
		}
	}
	if (ctx.csrf.empty()) {
		// A visitor with no session still needs a token for the login form.
		ctx.csrf = quackmail::web::AnonCsrfToken(con, req.peer_ip);
	}

	// Housekeeping, roughly once in fifty requests. Cheaper than a background
	// thread, and the same posture as policy::PruneSendLog.
	//
	// Gated on there being a session, because a page is now several requests:
	// without this, every asset fetch would roll the dice too and a browser
	// loading one page would trigger housekeeping several times over.
	if (ctx.Authed()) {
		std::string coin = quackmail::util::RandomHex(1);
		if (!coin.empty() && coin[0] == 'a') {
			quackmail::web::PruneSessions(con);
			// Sync tombstones age out on the same schedule. A client whose token
			// predates the cutoff is answered with a full re-listing, which is
			// what an expired sync-token is defined to mean.
			int64_t days = (int64_t)std::strtoll(ConfigStr(con, "qm_dav_tombstone_days", "30").c_str(),
			                                     nullptr, 10);
			if (days > 0) {
				quackmail::citadel::PruneTombstones(con, days * 86400);
			}
		}
	}

	const auto &routes = Routes();
	bool path_matched = false;
	for (const auto &route : routes) {
		std::vector<std::string> captures;
		if (!http::MatchPath(route.pattern, req.path, captures)) {
			continue;
		}
		path_matched = true;
		bool wildcard = std::string(route.method) == "*";
		bool method_ok = wildcard || req.method == route.method ||
		                 (req.method == "HEAD" && std::string(route.method) == "GET");
		if (!method_ok) {
			continue;
		}
		ctx.captures = captures;

		if (route.role == Role::Dav) {
			// A cookie session is honoured too, so the same URLs can be poked at
			// from a signed-in browser. Anything else has to present Basic.
			if (!ctx.Authed() && !BasicAuth(ctx)) {
				NeedsAuth(ctx);
				return;
			}
		} else if (route.role != Role::Anon && !ctx.Authed()) {
			// Bounce through the login page, remembering where they were going.
			SecurityHeaders(ctx);
			resp.Redirect("/login?next=" + http::PercentEncode(req.path), 303);
			return;
		}
		if (route.role == Role::Aide) {
			std::string why;
			if (!AdminReachable(ctx, why)) {
				Forbidden(ctx, why);
				return;
			}
			if (!ctx.IsAide()) {
				// 403 and not 404: the route names are in the README anyway, and
				// 403 is the better signal to an operator reading a log.
				Forbidden(ctx, "This page is for system administrators.");
				return;
			}
		}
		if (req.method == "POST" && route.role != Role::Dav) {
			if (!OriginAcceptable(ctx)) {
				Forbidden(ctx, "This form was submitted from another site.");
				return;
			}
			// The synchronizer token. Checked here rather than in each handler,
			// so a new POST route cannot be added without it. A visitor with no
			// session (the login form) gets the address-and-time-bound variant.
			std::string submitted = SubmittedCsrf(req);
			bool csrf_ok = ctx.Authed()
			                   ? quackmail::web::CheckCsrf(con, ctx.session_hash, submitted)
			                   : quackmail::web::CheckAnonCsrf(con, req.peer_ip, submitted);
			if (!csrf_ok) {
				Forbidden(ctx, "This form has expired. Go back, reload the page and try again.");
				return;
			}
		}
		route.fn(ctx);
		return;
	}

	if (path_matched) {
		SecurityHeaders(ctx);
		ctx.resp.SetHeader("Allow", "GET, POST");
		ErrorPage(ctx, 405, "Method not allowed", "That address does not accept this kind of request.");
		return;
	}
	NotFound(ctx);
}

} // namespace qmweb
} // namespace duckdb
