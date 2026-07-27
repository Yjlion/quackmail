#include "web.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/sieve.hpp"

#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Registration;
using quackmail::citadel::UserInfo;

namespace {

// The US_* bits a user may set for themselves. Citadel calls this US_USER_SET;
// the BBS shell's <.E>nter <C>onfiguration edits exactly the same list, so a
// preference set here shows up over telnet and vice versa.
struct FlagOption {
	const char *field;
	int64_t bit;
	const char *label;
};

const FlagOption kFlagOptions[] = {
    {"expert", quackmail::citadel::US_EXPERT, "Expert mode (hide the menu in the BBS shell)"},
    {"paginator", quackmail::citadel::US_PAGINATOR, "Pause after each screenful"},
    {"lastold", quackmail::citadel::US_LASTOLD, "Show the last already-read message with new ones"},
    {"noprompt", quackmail::citadel::US_NOPROMPT, "Do not prompt after each message"},
    {"promptctl", quackmail::citadel::US_PROMPTCTL, "<N>ext and <S>top work at the message prompt"},
    {"disappear", quackmail::citadel::US_DISAPPEAR, "Disappearing message prompts"},
    {"unlisted", quackmail::citadel::US_UNLISTED, "Hide me from the user listing"},
    {"color", quackmail::citadel::US_COLOR, "ANSI colour"},
};

void GetPrefs(Ctx &ctx) {
	UserInfo user;
	quackmail::citadel::GetUser(ctx.con, ctx.username, user);
	Registration reg;
	quackmail::citadel::GetRegistration(ctx.con, ctx.username, reg);

	std::string body = "<div class=\"msghead\"><dl>";
	body += "<dt>User number</dt><dd>" + T(std::to_string(user.usernum)) + "</dd>";
	body += "<dt>Access level</dt><dd>" + T(std::to_string(user.axlevel)) +
	        (user.axlevel >= 6 ? " (aide)" : "") + "</dd>";
	body += "<dt>Calls</dt><dd>" + T(std::to_string(user.times_called)) + "</dd>";
	body += "<dt>Posts</dt><dd>" + T(std::to_string(user.num_posts)) + "</dd>";
	body += "<dt>Last call</dt><dd>" + T(FormatTime(user.last_call)) + "</dd>";
	body += "</dl></div>";

	body += "<h2>Password</h2>";
	body += FormStart(ctx, "/prefs/password");
	body += "<label class=\"field\"><span>Current password</span>" + TextInput("current", "", "password") +
	        "</label>";
	body += "<label class=\"field\"><span>New password</span>" + TextInput("password", "", "password") +
	        "</label>";
	body += "<label class=\"field\"><span>Repeat new password</span>" +
	        TextInput("password2", "", "password") + "</label>";
	body += "<p>" + Button("Change password") + "</p>";
	body += "<p class=\"muted\">Changing your password signs out every other session for this "
	        "account.</p>";
	body += FormEnd();

	body += "<h2>Settings</h2>";
	body += FormStart(ctx, "/prefs/settings");
	for (auto &opt : kFlagOptions) {
		body += Checkbox(opt.field, (user.flags & opt.bit) != 0, opt.label) + "<br>";
	}
	body += "<label class=\"field\"><span>Screen width</span>" +
	        TextInput("width", std::to_string(user.screenwidth), "number") + "</label>";
	body += "<label class=\"field\"><span>Screen height</span>" +
	        TextInput("height", std::to_string(user.screenheight), "number") + "</label>";
	body += "<p>" + Button("Save settings") + "</p>";
	body += "<p class=\"muted\">These are the same preferences the BBS shell's "
	        "<code>.Enter Configuration</code> edits.</p>";
	body += FormEnd();

	body += "<h2>Registration</h2>";
	body += FormStart(ctx, "/prefs/profile");
	body += "<label class=\"field\"><span>Real name</span>" + TextInput("real_name", reg.real_name) +
	        "</label>";
	body += "<label class=\"field\"><span>Street</span>" + TextInput("street", reg.street) + "</label>";
	body += "<label class=\"field\"><span>City</span>" + TextInput("city", reg.city) + "</label>";
	body += "<label class=\"field\"><span>State</span>" + TextInput("state", reg.state) + "</label>";
	body += "<label class=\"field\"><span>Postal code</span>" + TextInput("zipcode", reg.zipcode) +
	        "</label>";
	body += "<label class=\"field\"><span>Telephone</span>" + TextInput("phone", reg.phone) + "</label>";
	body += "<label class=\"field\"><span>E-mail</span>" + TextInput("email", reg.email) + "</label>";
	body += "<label class=\"field\"><span>Country</span>" + TextInput("country", reg.country) + "</label>";
	body += "<label class=\"field\"><span>Biography</span>" + TextArea("bio", reg.bio, 8) + "</label>";
	body += "<p>" + Button("Save registration") + "</p>";
	body += FormEnd();

	body += "<h2>Elsewhere</h2><ul>";
	body += "<li>" + Link("/prefs/sieve", "Mail filters (Sieve)") + "</li>";
	body += "<li>" + Link("/prefs/sessions", "Signed-in browsers") + "</li>";
	body += "</ul>";

	Render(ctx, "Preferences", body);
}

void PostPassword(Ctx &ctx) {
	std::string current = ctx.req.Form("current");
	std::string next = ctx.req.Form("password");
	std::string repeat = ctx.req.Form("password2");

	// Re-authenticate: a stolen session must not be enough to take the account.
	if (!quackmail::auth::Verify(ctx.con, ctx.username, current)) {
		Forbidden(ctx, "That is not your current password.");
		return;
	}
	if (next.size() < 6) {
		BadRequest(ctx, "Choose a password of at least six characters.");
		return;
	}
	if (next != repeat) {
		BadRequest(ctx, "The two new passwords do not match.");
		return;
	}
	std::string err;
	// auth::AddUser is an INSERT OR REPLACE, so it doubles as "set password".
	if (!quackmail::auth::AddUser(ctx.con, ctx.username, next, err)) {
		ErrorPage(ctx, 500, "Could not change the password", err);
		return;
	}
	// Every session predates the new password, including this one — otherwise
	// a password change would not actually evict anyone.
	quackmail::web::RevokeAllForUser(ctx.con, ctx.username);

	SecurityHeaders(ctx);
	http::Cookie dead;
	dead.name = kSessionCookie;
	dead.value = "";
	dead.max_age = 0;
	dead.secure = ctx.tls;
	ctx.resp.AddHeader("Set-Cookie", http::SerializeCookie(dead));
	ctx.resp.Redirect("/login?ok=password", 303);
}

void PostSettings(Ctx &ctx) {
	UserInfo user;
	if (!quackmail::citadel::GetUser(ctx.con, ctx.username, user)) {
		NotFound(ctx);
		return;
	}
	// Keep every bit outside US_USER_SET exactly as it was: US_NEEDVALID and
	// US_PERM are an aide's to set, not the user's.
	int64_t flags = user.flags & ~quackmail::citadel::kUserSettableFlags;
	for (auto &opt : kFlagOptions) {
		if (!ctx.req.Form(opt.field).empty()) {
			flags |= opt.bit;
		}
	}
	quackmail::citadel::SetUserFlags(ctx.con, ctx.username, flags);
	quackmail::citadel::SetScreenSize(ctx.con, ctx.username, ctx.FormInt("width", 80),
	                                  ctx.FormInt("height", 24));
	RedirectTo(ctx, "/prefs", "saved");
}

void PostProfile(Ctx &ctx) {
	Registration reg;
	reg.real_name = ctx.req.Form("real_name");
	reg.street = ctx.req.Form("street");
	reg.city = ctx.req.Form("city");
	reg.state = ctx.req.Form("state");
	reg.zipcode = ctx.req.Form("zipcode");
	reg.phone = ctx.req.Form("phone");
	reg.email = ctx.req.Form("email");
	reg.country = ctx.req.Form("country");
	reg.bio = ctx.req.Form("bio");
	if (!quackmail::citadel::SetRegistration(ctx.con, ctx.username, reg)) {
		ErrorPage(ctx, 500, "Could not save", "The registration could not be stored.");
		return;
	}
	RedirectTo(ctx, "/prefs", "saved");
}

// ---- Sieve ---------------------------------------------------------------

struct Script {
	std::string name;
	bool active = false;
	std::string body;
};

std::vector<Script> LoadScripts(Ctx &ctx, const std::string &user) {
	std::vector<Script> out;
	auto r = Exec(ctx.con,
	              "SELECT name, active, script FROM quackmail_sieve_scripts WHERE username = $1 "
	              "ORDER BY name",
	              {Value(user)});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		Script s;
		s.name = mat.GetValue(0, i).IsNull() ? "" : mat.GetValue(0, i).ToString();
		s.active = !mat.GetValue(1, i).IsNull() && mat.GetValue(1, i).GetValue<bool>();
		s.body = mat.GetValue(2, i).IsNull() ? "" : mat.GetValue(2, i).ToString();
		out.push_back(std::move(s));
	}
	return out;
}

// Shared by /prefs/sieve and the admin console's per-user view.
std::string SieveBody(Ctx &ctx, const std::string &user, const std::string &action_prefix) {
	auto scripts = LoadScripts(ctx, user);
	std::string editing = ctx.req.Param("name");
	std::string current;
	for (auto &s : scripts) {
		if (s.name == editing) {
			current = s.body;
		}
	}

	std::string body = "<div class=\"wrap\"><table><tr>" + Head("Script") + Head("State") + Head("") +
	                   "</tr>";
	for (auto &s : scripts) {
		body += "<tr>";
		body += "<td>" + Link(action_prefix + "?name=" + http::PercentEncode(s.name) +
		                          (user.empty() ? "" : "&user=" + http::PercentEncode(user)),
		                      s.name) +
		        "</td>";
		body += Cell(s.active ? "active" : "");
		body += "<td>";
		if (!s.active) {
			body += FormStart(ctx, action_prefix + "/activate", "inline") + Hidden("name", s.name) +
			        Hidden("user", user) + Button("Activate", "sec") + FormEnd();
		}
		body += FormStart(ctx, action_prefix + "/delete", "inline") + Hidden("name", s.name) +
		        Hidden("user", user) + Button("Delete", "danger") + FormEnd();
		body += "</td></tr>";
	}
	body += "</table></div>";

	body += "<h2>" + T(editing.empty() ? "New script" : "Edit " + editing) + "</h2>";
	body += FormStart(ctx, action_prefix + "/save");
	body += Hidden("user", user);
	body += "<label class=\"field\"><span>Name</span>" + TextInput("name", editing) + "</label>";
	body += "<label class=\"field\"><span>Script</span>" + TextArea("script", current, 18) + "</label>";
	body += "<p>" + Button("Save") + "</p>";
	body += "<p class=\"muted\">Scripts are validated with the same parser the delivery path uses, so a "
	        "script that saves is a script that runs.</p>";
	body += FormEnd();
	return body;
}

bool SaveScript(Ctx &ctx, const std::string &user, const std::string &name, const std::string &script,
                std::string &err) {
	if (name.empty()) {
		err = "A script needs a name.";
		return false;
	}
	// Validate before storing — exactly what `quackcitadm.sh sieve set` does.
	if (!quackmail::sieve::Check(script, err)) {
		return false;
	}
	Exec(ctx.con, "DELETE FROM quackmail_sieve_scripts WHERE username = $1 AND name = $2",
	     {Value(user), Value(name)});
	if (!Exec(ctx.con,
	          "INSERT INTO quackmail_sieve_scripts (username, name, active, script) "
	          "VALUES ($1, $2, false, $3)",
	          {Value(user), Value(name), Value(script)})) {
		err = "Could not store the script.";
		return false;
	}
	return true;
}

void ActivateScript(Ctx &ctx, const std::string &user, const std::string &name) {
	// Exactly one script may be active, so clear the rest first — the same
	// order `quackcitadm.sh sieve activate` uses.
	Exec(ctx.con, "UPDATE quackmail_sieve_scripts SET active = false WHERE username = $1",
	     {Value(user)});
	Exec(ctx.con, "UPDATE quackmail_sieve_scripts SET active = true WHERE username = $1 AND name = $2",
	     {Value(user), Value(name)});
}

void GetSieve(Ctx &ctx) {
	Render(ctx, "Mail filters", SieveBody(ctx, ctx.username, "/prefs/sieve"));
}

void PostSieveSave(Ctx &ctx) {
	std::string err;
	if (!SaveScript(ctx, ctx.username, ctx.req.Form("name"), ctx.req.Form("script"), err)) {
		ErrorPage(ctx, 400, "The script was not saved", err);
		return;
	}
	RedirectTo(ctx, "/prefs/sieve", "saved");
}

void PostSieveActivate(Ctx &ctx) {
	ActivateScript(ctx, ctx.username, ctx.req.Form("name"));
	RedirectTo(ctx, "/prefs/sieve", "activated");
}

void PostSieveDelete(Ctx &ctx) {
	Exec(ctx.con, "DELETE FROM quackmail_sieve_scripts WHERE username = $1 AND name = $2",
	     {Value(ctx.username), Value(ctx.req.Form("name"))});
	RedirectTo(ctx, "/prefs/sieve", "deleted");
}

// ---- browser sessions ----------------------------------------------------

std::string SessionTable(Ctx &ctx, const std::vector<quackmail::web::SessionRow> &rows,
                         const std::string &action, bool show_user) {
	std::string body = "<div class=\"wrap\"><table><tr>";
	if (show_user) {
		body += Head("User");
	}
	body += Head("From") + Head("Browser") + Head("Signed in") + Head("Last seen") + Head("") + "</tr>";
	for (auto &s : rows) {
		body += "<tr>";
		if (show_user) {
			body += Cell(s.username);
		}
		body += Cell(s.peer_ip + (s.tls ? " (HTTPS)" : " (HTTP)"));
		body += Cell(s.user_agent.size() > 60 ? s.user_agent.substr(0, 60) + "…" : s.user_agent);
		body += Cell(FormatTime(s.created_at));
		body += Cell(FormatTime(s.last_seen));
		body += "<td>";
		body += FormStart(ctx, action, "inline") + Hidden("token_hash", s.token_hash);
		bool self = s.token_hash == ctx.session_hash;
		body += Button(self ? "Sign out (this one)" : "Sign out", "danger") + FormEnd();
		body += "</td></tr>";
	}
	return body + "</table></div>";
}

void GetSessions(Ctx &ctx) {
	auto rows = quackmail::web::ListSessions(ctx.con, ctx.username);
	Render(ctx, "Signed-in browsers", SessionTable(ctx, rows, "/prefs/sessions/revoke", false));
}

void PostRevokeSession(Ctx &ctx) {
	// Qualified by username, so pasting someone else's hash does nothing.
	quackmail::web::RevokeByHash(ctx.con, ctx.req.Form("token_hash"), ctx.username);
	RedirectTo(ctx, "/prefs/sessions", "revoked");
}

} // namespace

// Reused by the admin console, which edits any user's filters.
std::string SieveSection(Ctx &ctx, const std::string &user, const std::string &action_prefix) {
	return SieveBody(ctx, user, action_prefix);
}

bool SieveSave(Ctx &ctx, const std::string &user, const std::string &name, const std::string &script,
               std::string &err) {
	return SaveScript(ctx, user, name, script, err);
}

void SieveActivate(Ctx &ctx, const std::string &user, const std::string &name) {
	ActivateScript(ctx, user, name);
}

std::string WebSessionTable(Ctx &ctx, const std::vector<quackmail::web::SessionRow> &rows,
                            const std::string &action, bool show_user) {
	return SessionTable(ctx, rows, action, show_user);
}

void RegisterPrefsRoutes(std::vector<Route> &out) {
	out.push_back({"GET", "/prefs", Role::User, GetPrefs});
	out.push_back({"POST", "/prefs/password", Role::User, PostPassword});
	out.push_back({"POST", "/prefs/settings", Role::User, PostSettings});
	out.push_back({"POST", "/prefs/profile", Role::User, PostProfile});
	out.push_back({"GET", "/prefs/sieve", Role::User, GetSieve});
	out.push_back({"POST", "/prefs/sieve/save", Role::User, PostSieveSave});
	out.push_back({"POST", "/prefs/sieve/activate", Role::User, PostSieveActivate});
	out.push_back({"POST", "/prefs/sieve/delete", Role::User, PostSieveDelete});
	out.push_back({"GET", "/prefs/sessions", Role::User, GetSessions});
	out.push_back({"POST", "/prefs/sessions/revoke", Role::User, PostRevokeSession});
}

} // namespace qmweb
} // namespace duckdb
