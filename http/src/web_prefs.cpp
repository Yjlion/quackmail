#include "web.hpp"
#include "quackmail/tz.hpp"

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
	body += "<dt>Last call</dt><dd>" + T(FormatTime(ctx, user.last_call)) + "</dd>";
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
	body += "<label class=\"field\"><span>Colour theme</span>" +
	        Select("theme", ThemeOptions(),
	               quackmail::citadel::GetUserPref(ctx.con, ctx.username, "web_theme", "auto")) +
	        "</label>";

	// Every date on every page renders in this zone, and the calendar is
	// unusable without it. The empty option means "follow the site default",
	// which is stored as a cleared row rather than today's value of it.
	std::vector<std::pair<std::string, std::string>> zones;
	zones.push_back({"", "Follow the server (" + ConfigStr(ctx.con, "qm_default_tz", "UTC") + ")"});
	for (auto &z : quackmail::tz::List()) {
		zones.push_back({z, z});
	}
	body += "<label class=\"field\"><span>Time zone</span>" +
	        Select("tz", zones, quackmail::citadel::GetUserPref(ctx.con, ctx.username, "web_tz")) +
	        "</label>";

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

	PageOpts opts;
	opts.active = "prefs";
	Render(ctx, "Preferences", body, opts);
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
	// Only a theme we actually ship is stored; "auto" clears the row so the user
	// follows the site default rather than being pinned to today's value of it.
	std::string theme = ctx.req.Form("theme");
	for (auto &t : ThemeOptions()) {
		if (t.first == theme) {
			quackmail::citadel::SetUserPref(ctx.con, ctx.username, "web_theme",
			                                theme == "auto" ? "" : theme);
			break;
		}
	}
	// Likewise only a zone the bundled database knows; anything else clears the
	// row rather than storing a name that would silently fall back on every page.
	std::string zone = ctx.req.Form("tz");
	quackmail::citadel::SetUserPref(ctx.con, ctx.username, "web_tz",
	                                quackmail::tz::IsKnown(zone) ? zone : "");

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
bool SaveScript(Ctx &ctx, const std::string &user, const std::string &name,
                const std::string &script, std::string &err);

// ---- the rule builder ----------------------------------------------------
//
// Every control here is a plain form that posts. There is no JavaScript: adding
// a rule, deleting one and reordering are each a submit, which means the builder
// works in a text browser and cannot get into a state the server disagrees with.
//
// The round trip is always text -> rules -> edit -> text. Nothing structured is
// stored, so a script rewritten over ManageSieve simply shows up as different
// rules on the next page load rather than being clobbered.

const std::pair<const char *, const char *> kRuleFields[] = {
    {"from", "From"},
    {"to", "To"},
    {"cc", "Cc"},
    {"subject", "Subject"},
    {"body", "Message body"},
    {"size", "Size in bytes"},
};

const std::pair<const char *, const char *> kTextOps[] = {
    {"contains", "contains"},
    {"is", "is exactly"},
    {"matches", "matches (with * and ?)"},
};

const std::pair<const char *, const char *> kSizeOps[] = {
    {"over", "is over"},
    {"under", "is under"},
};

// Only the actions the evaluator implements. `Capabilities()` is
// "fileinto reject envelope body copy subaddress", so there is no vacation here
// — offering one would be a lie about what the server does with the message.
const std::pair<const char *, const char *> kRuleActions[] = {
    {"fileinto", "File into a folder"},
    {"keep", "Keep in the inbox"},
    {"redirect", "Forward to an address"},
    {"reject", "Refuse with a reason"},
    {"discard", "Discard silently"},
};

std::string DescribeTest(const quackmail::sieve::RuleTest &t) {
	std::string field = t.field;
	for (auto &f : kRuleFields) {
		if (field == f.first) {
			field = f.second;
			break;
		}
	}
	if (field.rfind("header:", 0) == 0) {
		field = "header " + field.substr(7);
	}
	std::string op = t.op;
	for (auto &o : kTextOps) {
		if (op == o.first) {
			op = o.second;
			break;
		}
	}
	for (auto &o : kSizeOps) {
		if (op == o.first) {
			op = o.second;
			break;
		}
	}
	return field + (t.negate ? " does not " : " ") + op + " " + t.value;
}

std::string DescribeAction(const quackmail::sieve::Action &a) {
	switch (a.type) {
	case quackmail::sieve::Action::FILEINTO:
		return "file into " + a.folder;
	case quackmail::sieve::Action::REDIRECT:
		return "forward to " + a.address;
	case quackmail::sieve::Action::REJECT:
		return "refuse: " + a.reason;
	case quackmail::sieve::Action::DISCARD:
		return "discard";
	default:
		return "keep";
	}
}

// One card per rule, each with its own small forms. A rule is identified by its
// index in the decomposed list — which is stable for the text as it stands, and
// re-derived on every load, so a concurrent change over ManageSieve cannot make
// a button act on the wrong rule silently: the indices simply describe whatever
// the script says now.
std::string RuleCards(Ctx &ctx, const std::string &action_prefix, const std::string &user,
                      const std::string &script_name,
                      const std::vector<quackmail::sieve::Rule> &rules) {
	std::string out = "<div class=\"rules\">";
	for (size_t i = 0; i < rules.size(); i++) {
		const auto &r = rules[i];
		out += "<div class=\"card rule\">";
		out += "<h3>" + T(r.name.empty() ? "Rule " + std::to_string(i + 1) : r.name) + "</h3>";

		out += "<p class=\"muted\">If " + T(r.all ? "all" : "any") + " of:</p><ul>";
		for (auto &t : r.tests) {
			out += "<li>" + T(DescribeTest(t)) + "</li>";
		}
		out += "</ul><p class=\"muted\">then:</p><ul>";
		for (auto &a : r.actions) {
			out += "<li>" + T(DescribeAction(a)) + "</li>";
		}
		if (r.stop) {
			out += "<li>" + T("stop — later rules do not run") + "</li>";
		}
		out += "</ul>";

		out += "<div class=\"actions\">";
		auto hidden = [&]() {
			return Hidden("user", user) + Hidden("name", script_name) +
			       Hidden("rule", std::to_string(i));
		};
		if (i > 0) {
			out += FormStart(ctx, action_prefix + "/rule/move", "inline") + hidden() +
			       Hidden("dir", "up") + Button("Move up", "sec") + FormEnd();
		}
		if (i + 1 < rules.size()) {
			out += FormStart(ctx, action_prefix + "/rule/move", "inline") + hidden() +
			       Hidden("dir", "down") + Button("Move down", "sec") + FormEnd();
		}
		out += FormStart(ctx, action_prefix + "/rule/delete", "inline") + hidden() +
		       "<button class=\"btn danger\" type=\"submit\" data-confirm=\"Delete this rule?\">"
		       "Delete rule</button>" +
		       FormEnd();
		out += "</div></div>";
	}
	return out + "</div>";
}

// The add form. One condition at a time: a multi-condition rule is built by
// adding the rule and then adding conditions to it, which keeps every step a
// single POST with no client-side state to lose.
std::string AddRuleForm(Ctx &ctx, const std::string &action_prefix, const std::string &user,
                        const std::string &script_name) {
	std::vector<std::pair<std::string, std::string>> fields;
	for (auto &f : kRuleFields) {
		fields.push_back({f.first, f.second});
	}
	std::vector<std::pair<std::string, std::string>> ops;
	for (auto &o : kTextOps) {
		ops.push_back({o.first, o.second});
	}
	for (auto &o : kSizeOps) {
		ops.push_back({o.first, o.second});
	}
	std::vector<std::pair<std::string, std::string>> actions;
	for (auto &a : kRuleActions) {
		actions.push_back({a.first, a.second});
	}

	std::string out = "<h3>Add a rule</h3>";
	out += FormStart(ctx, action_prefix + "/rule/add");
	out += Hidden("user", user);
	out += Hidden("name", script_name);
	out += "<label class=\"field\"><span>Name (optional)</span>" + TextInput("rule_name", "") +
	       "</label>";
	out += "<label class=\"field\"><span>When</span>" + Select("field", fields, "from") + "</label>";
	out += "<label class=\"field\"><span>Test</span>" + Select("op", ops, "contains") + "</label>";
	out += "<label class=\"field\"><span>Value</span>" + TextInput("value", "") + "</label>";
	out += Checkbox("negate", false, "Invert this condition") + "<br>";
	out += "<label class=\"field\"><span>Then</span>" + Select("action", actions, "fileinto") +
	       "</label>";
	out += "<label class=\"field\"><span>Folder or address</span>" + TextInput("argument", "") +
	       "</label>";
	out += Checkbox("stop", false, "Stop: do not run later rules") + "<br>";
	out += "<p>" + Button("Add rule") + "</p>";
	out += "<p class=\"muted\">Adding a rule rewrites the script text below. A rule with more than one "
	       "condition is built by adding it and then adding a condition to it.</p>";
	out += FormEnd();
	return out;
}

// ---- rule POST handlers --------------------------------------------------
//
// Each one decomposes the current text, changes one thing, and composes it back.
// A script the rule view cannot represent is refused rather than rewritten:
// otherwise a stray click would silently replace filtering the user wrote by
// hand with an approximation of it.

// The named script's text, and the decomposed rules, or false having rendered
// the reason.
bool LoadRules(Ctx &ctx, const std::string &user, const std::string &name, std::string &text,
               std::vector<quackmail::sieve::Rule> &rules) {
	auto r = Exec(ctx.con,
	              "SELECT script FROM quackmail_sieve_scripts WHERE username = $1 AND name = $2 LIMIT 1",
	              {Value(user), Value(name)});
	if (!r) {
		NotFound(ctx);
		return false;
	}
	auto &mat = r->Cast<duckdb::MaterializedQueryResult>();
	text = mat.RowCount() ? mat.GetValue(0, 0).ToString() : std::string();

	std::string why;
	if (!text.empty() && !quackmail::sieve::Decompose(text, rules, why)) {
		ErrorPage(ctx, 400, "This script cannot be edited as rules",
		          why + " Edit it as text instead — the rule editor reads it back from the text, so "
		                "nothing is lost by doing so.");
		return false;
	}
	return true;
}

bool StoreRules(Ctx &ctx, const std::string &user, const std::string &name,
                const std::vector<quackmail::sieve::Rule> &rules) {
	std::string composed = quackmail::sieve::Compose(rules);
	std::string err;
	// Compose is the only writer, so this should never fail — but it goes through
	// the same validation as a hand-typed script rather than trusting that.
	if (!SaveScript(ctx, user, name, composed, err)) {
		ErrorPage(ctx, 500, "The rules could not be saved", err);
		return false;
	}
	return true;
}

std::string SieveHref(const std::string &action_prefix, const std::string &user,
                      const std::string &name) {
	std::string out = action_prefix + "?name=" + http::PercentEncode(name);
	if (!user.empty()) {
		out += "&user=" + http::PercentEncode(user);
	}
	return out;
}

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

	// ---- the rule view ---------------------------------------------------
	// Rules are derived from the script text every time, never stored beside it:
	// ManageSieve and the admin console write the same row, so a table of rules
	// would either be overwritten silently or start describing filtering that is
	// not what runs. See the note in core/include/quackmail/sieve.hpp.
	if (!editing.empty() || !current.empty()) {
		body += "<h2>Rules</h2>";
		std::vector<quackmail::sieve::Rule> rules;
		std::string why;
		if (!quackmail::sieve::Decompose(current, rules, why)) {
			// Refuse to show a partial decomposition: describing filtering that
			// is not what happens is worse than admitting the view cannot show
			// it. Only the text editor below is offered.
			body += "<div class=\"warnbar\">" + T(why) +
			        " Edit it as text below — the rules here are read back from that text, so nothing "
			        "is lost by doing so.</div>";
		} else if (rules.empty()) {
			body += "<p class=\"muted\">This script has no rules yet.</p>";
		} else {
			body += RawHtml(RuleCards(ctx, action_prefix, user, editing, rules));
		}
		body += RawHtml(AddRuleForm(ctx, action_prefix, user, editing));
	}

	body += "<h2>" + T(editing.empty() ? "New script" : "Source of " + editing) + "</h2>";
	body += FormStart(ctx, action_prefix + "/save");
	body += Hidden("user", user);
	body += "<label class=\"field\"><span>Name</span>" + TextInput("name", editing) + "</label>";
	body += "<label class=\"field\"><span>Script</span>" + TextArea("script", current, 18) + "</label>";
	body += "<p>" + Button("Save") + "</p>";
	body += "<p class=\"muted\">This text is what runs, and what the rules above are read from. Scripts "
	        "are validated with the same parser the delivery path uses, so a script that saves is a "
	        "script that runs.</p>";
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

// The rule handlers. `user` is only honoured for an aide editing someone else's
// script through /admin/sieve; for /prefs/sieve it is the signed-in user, and
// the router's Role gate is what keeps the two apart.
std::string RuleUser(Ctx &ctx) {
	std::string requested = ctx.req.Form("user");
	if (requested.empty() || !ctx.IsAide()) {
		return ctx.username;
	}
	return requested;
}

void PostRuleAdd(Ctx &ctx, const std::string &action_prefix) {
	std::string user = RuleUser(ctx);
	std::string name = ctx.req.Form("name");
	if (name.empty()) {
		BadRequest(ctx, "Choose or name a script first.");
		return;
	}
	std::string text;
	std::vector<quackmail::sieve::Rule> rules;
	if (!LoadRules(ctx, user, name, text, rules)) {
		return;
	}

	quackmail::sieve::Rule rule;
	rule.name = ctx.req.Form("rule_name");
	rule.all = true;
	rule.stop = !ctx.req.Form("stop").empty();

	quackmail::sieve::RuleTest test;
	test.field = ctx.req.Form("field");
	test.op = ctx.req.Form("op");
	test.value = ctx.req.Form("value");
	test.negate = !ctx.req.Form("negate").empty();
	if (test.field.empty() || test.value.empty()) {
		BadRequest(ctx, "A rule needs something to test and a value to test it against.");
		return;
	}
	// A size test only means anything against a number, and the two operator sets
	// are not interchangeable.
	if (test.field == "size") {
		if (test.op != "over" && test.op != "under") {
			test.op = "over";
		}
		for (char c : test.value) {
			if (c < '0' || c > '9') {
				BadRequest(ctx, "A size is a number of bytes.");
				return;
			}
		}
	} else if (test.op != "is" && test.op != "contains" && test.op != "matches") {
		test.op = "contains";
	}
	rule.tests.push_back(test);

	std::string action = ctx.req.Form("action");
	std::string argument = ctx.req.Form("argument");
	quackmail::sieve::Action a;
	if (action == "fileinto") {
		if (argument.empty()) {
			BadRequest(ctx, "Filing a message needs a folder to file it into.");
			return;
		}
		a.type = quackmail::sieve::Action::FILEINTO;
		a.folder = argument;
	} else if (action == "redirect") {
		if (argument.find('@') == std::string::npos) {
			BadRequest(ctx, "Forwarding needs an e-mail address.");
			return;
		}
		a.type = quackmail::sieve::Action::REDIRECT;
		a.address = argument;
	} else if (action == "reject") {
		a.type = quackmail::sieve::Action::REJECT;
		a.reason = argument.empty() ? "message refused by the recipient's filter" : argument;
	} else if (action == "discard") {
		a.type = quackmail::sieve::Action::DISCARD;
	} else {
		a.type = quackmail::sieve::Action::KEEP;
	}
	rule.actions.push_back(a);

	rules.push_back(rule);
	if (!StoreRules(ctx, user, name, rules)) {
		return;
	}
	RedirectTo(ctx, SieveHref(action_prefix, ctx.req.Form("user"), name), "saved");
}

void PostRuleDelete(Ctx &ctx, const std::string &action_prefix) {
	std::string user = RuleUser(ctx);
	std::string name = ctx.req.Form("name");
	std::string text;
	std::vector<quackmail::sieve::Rule> rules;
	if (!LoadRules(ctx, user, name, text, rules)) {
		return;
	}
	int64_t which = ctx.FormInt("rule", -1);
	if (which < 0 || (size_t)which >= rules.size()) {
		NotFound(ctx);
		return;
	}
	rules.erase(rules.begin() + (size_t)which);
	if (!StoreRules(ctx, user, name, rules)) {
		return;
	}
	RedirectTo(ctx, SieveHref(action_prefix, ctx.req.Form("user"), name), "deleted");
}

void PostRuleMove(Ctx &ctx, const std::string &action_prefix) {
	std::string user = RuleUser(ctx);
	std::string name = ctx.req.Form("name");
	std::string text;
	std::vector<quackmail::sieve::Rule> rules;
	if (!LoadRules(ctx, user, name, text, rules)) {
		return;
	}
	int64_t which = ctx.FormInt("rule", -1);
	if (which < 0 || (size_t)which >= rules.size()) {
		NotFound(ctx);
		return;
	}
	// Order matters in Sieve: a rule with `stop` prevents everything after it, so
	// moving one is a real change rather than cosmetic.
	size_t i = (size_t)which;
	if (ctx.req.Form("dir") == "up" && i > 0) {
		std::swap(rules[i], rules[i - 1]);
	} else if (ctx.req.Form("dir") == "down" && i + 1 < rules.size()) {
		std::swap(rules[i], rules[i + 1]);
	}
	if (!StoreRules(ctx, user, name, rules)) {
		return;
	}
	RedirectTo(ctx, SieveHref(action_prefix, ctx.req.Form("user"), name), "saved");
}

void PostSieveRuleAdd(Ctx &ctx) {
	PostRuleAdd(ctx, "/prefs/sieve");
}
void PostSieveRuleDelete(Ctx &ctx) {
	PostRuleDelete(ctx, "/prefs/sieve");
}
void PostSieveRuleMove(Ctx &ctx) {
	PostRuleMove(ctx, "/prefs/sieve");
}

void GetSieve(Ctx &ctx) {
	PageOpts opts;
	opts.active = "sieve";
	Render(ctx, "Mail filters", SieveBody(ctx, ctx.username, "/prefs/sieve"), opts);
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
		body += Cell(FormatTime(ctx, s.created_at));
		body += Cell(FormatTime(ctx, s.last_seen));
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
	PageOpts opts;
	opts.active = "sessions";
	Render(ctx, "Signed-in browsers", SessionTable(ctx, rows, "/prefs/sessions/revoke", false), opts);
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
	out.push_back({"POST", "/prefs/sieve/rule/add", Role::User, PostSieveRuleAdd});
	out.push_back({"POST", "/prefs/sieve/rule/delete", Role::User, PostSieveRuleDelete});
	out.push_back({"POST", "/prefs/sieve/rule/move", Role::User, PostSieveRuleMove});
	out.push_back({"GET", "/prefs/sessions", Role::User, GetSessions});
	out.push_back({"POST", "/prefs/sessions/revoke", Role::User, PostRevokeSession});
}

} // namespace qmweb
} // namespace duckdb
