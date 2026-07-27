#include "web.hpp"

#include "quackmail/auth.hpp"

#include <chrono>
#include <thread>

namespace duckdb {
namespace qmweb {

namespace {

// Where to send a visitor after signing in. Only a same-site path is ever
// honoured, and the check runs *after* percent-decoding — checking the raw
// value would miss "%2f%2fevil.example".
std::string SafeNext(const http::Request &req) {
	std::string next = req.Param("next");
	if (next.empty()) {
		next = req.Form("next");
	}
	if (next.empty() || !http::IsSafeRedirectTarget(next)) {
		return "/mail/";
	}
	return next;
}

void SetSessionCookie(Ctx &ctx, const std::string &token) {
	http::Cookie c;
	c.name = kSessionCookie;
	c.value = token;
	c.http_only = true;
	c.same_site = "Lax"; // not Strict: a link into webmail should stay signed in
	// Secure only over TLS — a Secure cookie on a plaintext connection is
	// simply never sent back, which would look like a broken login. The
	// transport pinning in websession.cpp is what stops the two mixing.
	c.secure = ctx.tls;
	ctx.resp.AddHeader("Set-Cookie", http::SerializeCookie(c));
}

void LoginPage(Ctx &ctx, const std::string &problem, int status) {
	std::string node = ConfigStr(ctx.con, "c_humannode", "QuackCit");
	std::string body = "<div class=\"login\">";
	if (!problem.empty()) {
		body += "<div class=\"flash err\">" + T(problem) + "</div>";
	}
	body += FormStart(ctx, "/login");
	body += Hidden("next", SafeNext(ctx.req));
	body += "<label class=\"field\"><span>User name</span>" +
	        TextInput("username", ctx.req.Form("username")) + "</label>";
	body += "<label class=\"field\"><span>Password</span>" + TextInput("password", "", "password") +
	        "</label>";
	body += "<p>" + Button("Sign in") + "</p>";
	body += FormEnd();
	body += "</div>";
	Render(ctx, "Sign in to " + node, body, status);
}

void GetLogin(Ctx &ctx) {
	if (ctx.Authed()) {
		RedirectTo(ctx, SafeNext(ctx.req));
		return;
	}
	LoginPage(ctx, "", 200);
}

void PostLogin(Ctx &ctx) {
	std::string username = ctx.req.Form("username");
	std::string password = ctx.req.Form("password");

	int64_t retry_after = 0;
	if (!quackmail::web::LoginAllowed(ctx.con, ctx.req.peer_ip, retry_after)) {
		SecurityHeaders(ctx);
		ctx.resp.SetHeader("Retry-After", std::to_string(retry_after));
		ErrorPage(ctx, 429, "Too many attempts",
		          "Too many failed sign-ins from this address. Try again in " +
		              std::to_string(retry_after) + " seconds.");
		return;
	}

	bool ok = !username.empty() && !password.empty() && quackmail::auth::Verify(ctx.con, username, password);
	if (!ok) {
		quackmail::web::RecordLoginFailure(ctx.con, ctx.req.peer_ip, username);
		// A fixed delay on every failure, and one message for both "no such
		// user" and "wrong password". POP3 distinguishes them for Citadel
		// parity; a web login has no parity obligation and must not enumerate
		// accounts for anyone who can reach the form.
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		LoginPage(ctx, "That user name and password do not match.", 401);
		return;
	}

	std::string token, err;
	if (!quackmail::web::CreateSession(ctx.con, username, ctx.tls, ctx.req.peer_ip,
	                                   ctx.req.Header("User-Agent"), token, err)) {
		ErrorPage(ctx, 503, "Cannot sign in now", err);
		return;
	}
	quackmail::web::ClearLoginFailures(ctx.con, ctx.req.peer_ip);
	// First sign-in provisions the standard personal rooms, exactly as the POP3
	// and telnet logins do.
	quackmail::citadel::EnsureUserRooms(ctx.con, username);
	quackmail::citadel::RecordCall(ctx.con, username);

	SecurityHeaders(ctx);
	SetSessionCookie(ctx, token);
	ctx.resp.Redirect(SafeNext(ctx.req), 303);
}

void PostLogout(Ctx &ctx) {
	std::string cookie = http::CookieValue(ctx.req.Header("Cookie"), kSessionCookie);
	quackmail::web::RevokeSession(ctx.con, cookie);

	SecurityHeaders(ctx);
	http::Cookie dead;
	dead.name = kSessionCookie;
	dead.value = "";
	dead.max_age = 0;
	dead.secure = ctx.tls;
	ctx.resp.AddHeader("Set-Cookie", http::SerializeCookie(dead));
	ctx.resp.Redirect("/login", 303);
}

void GetRoot(Ctx &ctx) {
	RedirectTo(ctx, ctx.Authed() ? "/mail/" : "/login");
}

// A 204 rather than a 404, purely so every page load does not log a miss.
void GetFavicon(Ctx &ctx) {
	ctx.resp.status = 204;
	ctx.resp.SetHeader("Cache-Control", "public, max-age=86400");
}

// Nothing here is crawlable and none of it should be indexed.
void GetRobots(Ctx &ctx) {
	ctx.resp.Text("User-agent: *\nDisallow: /\n");
}

} // namespace

void RegisterAuthRoutes(std::vector<Route> &out) {
	out.push_back({"GET", "/", Role::Anon, GetRoot});
	out.push_back({"GET", "/login", Role::Anon, GetLogin});
	out.push_back({"POST", "/login", Role::Anon, PostLogin});
	out.push_back({"POST", "/logout", Role::User, PostLogout});
	out.push_back({"GET", "/favicon.ico", Role::Anon, GetFavicon});
	out.push_back({"GET", "/robots.txt", Role::Anon, GetRobots});
}

} // namespace qmweb
} // namespace duckdb
