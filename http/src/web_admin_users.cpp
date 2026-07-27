#include "web.hpp"

#include "quackmail/auth.hpp"

#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Room;

// ---- shared admin helpers -------------------------------------------------

bool ReAuth(Ctx &ctx) {
	// The sharpest actions ask for the operator's own password again, so that a
	// stolen session alone is not enough to mint credentials or rewrite config.
	return quackmail::auth::Verify(ctx.con, ctx.username, ctx.req.Form("admin_password"));
}

std::string ReAuthField() {
	return "<label class=\"field\"><span>Confirm with your own password</span>"
	       "<input type=\"password\" name=\"admin_password\"></label>";
}

void ReAuthFailed(Ctx &ctx) {
	Forbidden(ctx, "That is not your password. This action asks for it because it changes who can reach "
	               "this server.");
}

std::string AdminNav() {
	std::string out = "<div class=\"actions\">";
	out += Link("/admin/", "Overview", "btn sec");
	out += Link("/admin/users", "Users", "btn sec");
	out += Link("/admin/rooms", "Rooms", "btn sec");
	out += Link("/admin/floors", "Floors", "btn sec");
	out += Link("/admin/config", "Config", "btn sec");
	out += Link("/admin/domains", "Domains", "btn sec");
	out += Link("/admin/aliases", "Aliases", "btn sec");
	out += Link("/admin/acl", "Access rules", "btn sec");
	out += Link("/admin/rbl", "Blocklists", "btn sec");
	out += Link("/admin/dkim", "DKIM", "btn sec");
	out += Link("/admin/ratelimits", "Quotas", "btn sec");
	out += Link("/admin/inbound", "Audit log", "btn sec");
	out += Link("/admin/queue", "Mail queue", "btn sec");
	out += Link("/admin/sieve", "Filters", "btn sec");
	out += Link("/admin/websessions", "Web sessions", "btn sec");
	return out + "</div>";
}

void AdminPage(Ctx &ctx, const std::string &title, const std::string &body) {
	Render(ctx, title, AdminNav() + body);
}

namespace {

int64_t ScalarCount(Ctx &ctx, const std::string &sql) {
	auto r = ctx.con.Query(sql);
	if (r->HasError() || r->RowCount() < 1) {
		return 0;
	}
	auto v = r->GetValue(0, 0);
	return v.IsNull() ? 0 : v.GetValue<int64_t>();
}

void GetAdminIndex(Ctx &ctx) {
	std::string body =
	    "<div class=\"wrap\"><table><tr>" + Head("Metric") + "<th class=\"num\">Value</th></tr>";
	auto row = [&](const char *label, int64_t value) {
		body += "<tr>" + Cell(label) + "<td class=\"num\">" + T(std::to_string(value)) + "</td></tr>";
	};
	row("Users", ScalarCount(ctx, "SELECT count(*) FROM quackmail_users"));
	row("Floors", ScalarCount(ctx, "SELECT count(*) FROM citadel_floors"));
	row("Rooms", ScalarCount(ctx, "SELECT count(*) FROM citadel_rooms"));
	row("Messages", ScalarCount(ctx, "SELECT count(*) FROM citadel_messages"));
	row("Queued for delivery",
	    ScalarCount(ctx, "SELECT count(*) FROM quackmail_outbound WHERE status = 'queued'"));
	row("Protocol sessions online", ScalarCount(ctx, "SELECT count(*) FROM citadel_sessions"));
	row("Signed-in browsers",
	    ScalarCount(ctx, "SELECT count(*) FROM quackmail_web_sessions WHERE NOT revoked"));
	body += "</table></div>";

	body += "<p class=\"muted\">This console can create accounts, rewrite server configuration and "
	        "generate signing keys — reaching it is equivalent to root on this host. It is gated by "
	        "<code>qm_web_admin_enabled</code>, <code>qm_web_admin_require_tls</code> and the "
	        "<code>webadmin</code> ACL scope.</p>";
	AdminPage(ctx, "Administration", body);
}

// ---- users ---------------------------------------------------------------

void GetUsers(Ctx &ctx) {
	std::string body = "<div class=\"wrap\"><table><tr>" + Head("User") + "<th class=\"num\">Number</th>" +
	                   Head("Access") + Head("Enabled") + "<th class=\"num\">Calls</th>" +
	                   Head("Last call") + Head("") + "</tr>";
	for (auto &u : quackmail::citadel::ListUsers(ctx.con)) {
		body += "<tr>";
		body += Cell(u.username);
		body += "<td class=\"num\">" + T(std::to_string(u.usernum)) + "</td>";
		body += "<td>" + FormStart(ctx, "/admin/users/axlevel", "inline") + Hidden("username", u.username) +
		        TextInput("axlevel", std::to_string(u.axlevel), "number") + Button("Set", "sec") +
		        FormEnd() + "</td>";
		body += "<td>" + FormStart(ctx, "/admin/users/enable", "inline") + Hidden("username", u.username) +
		        Hidden("enabled", u.enabled ? "0" : "1") +
		        Button(u.enabled ? "Yes — disable" : "No — enable", "sec") + FormEnd() + "</td>";
		body += "<td class=\"num\">" + T(std::to_string(u.times_called)) + "</td>";
		body += Cell(FormatTime(u.last_call));
		body += "<td>" + Link("/admin/sieve?user=" + http::PercentEncode(u.username), "Filters") + "</td>";
		body += "</tr>";
	}
	body += "</table></div>";

	body += "<h2>Add a user</h2>";
	body += FormStart(ctx, "/admin/users/add");
	body += "<label class=\"field\"><span>User name</span>" + TextInput("username", "") + "</label>";
	body += "<label class=\"field\"><span>Password</span>" + TextInput("password", "", "password") +
	        "</label>";
	body += RawHtml(ReAuthField());
	body += "<p>" + Button("Create") + "</p>";
	body += FormEnd();

	body += "<h2>Reset a password</h2>";
	body += FormStart(ctx, "/admin/users/passwd");
	body += "<label class=\"field\"><span>User name</span>" + TextInput("username", "") + "</label>";
	body += "<label class=\"field\"><span>New password</span>" + TextInput("password", "", "password") +
	        "</label>";
	body += RawHtml(ReAuthField());
	body += "<p>" + Button("Set password") + "</p>";
	body += "<p class=\"muted\">This also signs that user out of every browser.</p>";
	body += FormEnd();

	body += "<h2>Remove a user</h2>";
	body += FormStart(ctx, "/admin/users/remove");
	body += "<label class=\"field\"><span>User name</span>" + TextInput("username", "") + "</label>";
	body += RawHtml(ReAuthField());
	body += "<p>" + Button("Remove", "danger") + "</p>";
	body += "<p class=\"muted\">Removes the credential only; the user's rooms and messages stay.</p>";
	body += FormEnd();

	AdminPage(ctx, "Users", body);
}

void PostUserAdd(Ctx &ctx) {
	if (!ReAuth(ctx)) {
		ReAuthFailed(ctx);
		return;
	}
	std::string name = ctx.req.Form("username");
	std::string pw = ctx.req.Form("password");
	if (name.empty() || pw.empty()) {
		BadRequest(ctx, "A user needs both a name and a password.");
		return;
	}
	if (quackmail::citadel::GetOrAssignUserNum(ctx.con, name) > 0) {
		BadRequest(ctx, "That user already exists — use the password reset instead.");
		return;
	}
	std::string err;
	if (!quackmail::auth::AddUser(ctx.con, name, pw, err)) {
		ErrorPage(ctx, 500, "Could not create the user", err);
		return;
	}
	quackmail::citadel::EnsureUserRooms(ctx.con, name);
	RedirectTo(ctx, "/admin/users", "created");
}

void PostUserPasswd(Ctx &ctx) {
	if (!ReAuth(ctx)) {
		ReAuthFailed(ctx);
		return;
	}
	std::string name = ctx.req.Form("username");
	std::string pw = ctx.req.Form("password");
	if (name.empty() || pw.empty()) {
		BadRequest(ctx, "A password reset needs both a user name and a password.");
		return;
	}
	if (quackmail::citadel::GetOrAssignUserNum(ctx.con, name) <= 0) {
		NotFound(ctx);
		return;
	}
	std::string err;
	// auth::AddUser is an INSERT OR REPLACE, so it doubles as "set password".
	if (!quackmail::auth::AddUser(ctx.con, name, pw, err)) {
		ErrorPage(ctx, 500, "Could not set the password", err);
		return;
	}
	// Sessions minted under the old password must not outlive it.
	quackmail::web::RevokeAllForUser(ctx.con, name);
	RedirectTo(ctx, "/admin/users", "saved");
}

void PostUserRemove(Ctx &ctx) {
	if (!ReAuth(ctx)) {
		ReAuthFailed(ctx);
		return;
	}
	std::string name = ctx.req.Form("username");
	if (name == ctx.username) {
		BadRequest(ctx, "You cannot remove the account you are signed in with.");
		return;
	}
	std::string err;
	if (!quackmail::auth::RemoveUser(ctx.con, name, err)) {
		ErrorPage(ctx, 500, "Could not remove the user", err);
		return;
	}
	quackmail::web::RevokeAllForUser(ctx.con, name);
	RedirectTo(ctx, "/admin/users", "deleted");
}

void PostUserAxlevel(Ctx &ctx) {
	std::string name = ctx.req.Form("username");
	int64_t level = ctx.FormInt("axlevel", -1);
	if (name == ctx.username && level < 6) {
		BadRequest(ctx, "You cannot drop your own aide access — another aide has to do it.");
		return;
	}
	std::string err;
	if (!quackmail::citadel::SetAxLevel(ctx.con, name, level, err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/users", "saved");
}

void PostUserEnable(Ctx &ctx) {
	std::string name = ctx.req.Form("username");
	bool enabled = ctx.req.Form("enabled") == "1";
	if (name == ctx.username && !enabled) {
		BadRequest(ctx, "You cannot disable the account you are signed in with.");
		return;
	}
	Exec(ctx.con, "UPDATE quackmail_users SET enabled = $2 WHERE username = $1",
	     {Value(name), Value::BOOLEAN(enabled)});
	if (!enabled) {
		quackmail::web::RevokeAllForUser(ctx.con, name);
	}
	RedirectTo(ctx, "/admin/users", "saved");
}

// ---- rooms ---------------------------------------------------------------

struct FlagBit {
	const char *field;
	int64_t bit;
	const char *label;
};

const FlagBit kRoomFlags[] = {
    {"private", quackmail::citadel::QR_PRIVATE, "Private"},
    {"passworded", quackmail::citadel::QR_PASSWORDED, "Password protected"},
    {"guessname", quackmail::citadel::QR_GUESSNAME, "Guess-name"},
    {"readonly", quackmail::citadel::QR_READONLY, "Read only"},
    {"directory", quackmail::citadel::QR_DIRECTORY, "Directory"},
    {"permanent", quackmail::citadel::QR_PERMANENT, "Never auto-purge"},
    {"prefonly", quackmail::citadel::QR_PREFONLY, "Preferred users only"},
    {"network", quackmail::citadel::QR_NETWORK, "Network shared"},
};

std::vector<std::pair<std::string, std::string>> ViewOptions() {
	return {{"0", "Message board"}, {"1", "Mailbox"}, {"2", "Address book"},
	        {"3", "Calendar"},      {"4", "Tasks"},   {"5", "Notes"}};
}

std::vector<std::pair<std::string, std::string>> FloorOptions(Ctx &ctx) {
	std::vector<std::pair<std::string, std::string>> out;
	for (auto &f : quackmail::citadel::ListFloors(ctx.con)) {
		out.emplace_back(std::to_string(f.floor_num), f.name);
	}
	return out;
}

int64_t FlagsFromForm(Ctx &ctx) {
	int64_t flags = 0;
	for (auto &f : kRoomFlags) {
		if (!ctx.req.Form(f.field).empty()) {
			flags |= f.bit;
		}
	}
	return flags;
}

void GetRooms(Ctx &ctx) {
	int64_t editing = ctx.ParamInt("edit", -1);

	std::string body = "<div class=\"wrap\"><table><tr>" + Head("Room") + "<th class=\"num\">Number</th>" +
	                   "<th class=\"num\">Floor</th>" + "<th class=\"num\">Flags</th>" +
	                   "<th class=\"num\">Owner</th>" + Head("") + "</tr>";
	auto r = ctx.con.Query("SELECT room_num, display_name, floor_num, qr_flags, mailbox_owner "
	                       "FROM citadel_rooms ORDER BY floor_num, listorder, display_name");
	if (!r->HasError()) {
		auto &mat = r->Cast<MaterializedQueryResult>();
		for (idx_t i = 0; i < mat.RowCount(); i++) {
			std::string num = mat.GetValue(0, i).ToString();
			body += "<tr>";
			body += Cell(mat.GetValue(1, i).ToString());
			body += "<td class=\"num\">" + T(num) + "</td>";
			body += "<td class=\"num\">" + T(mat.GetValue(2, i).ToString()) + "</td>";
			body += "<td class=\"num\">" + T(mat.GetValue(3, i).ToString()) + "</td>";
			body += "<td class=\"num\">" + T(mat.GetValue(4, i).ToString()) + "</td>";
			body += "<td>" + Link("/admin/rooms?edit=" + num, "Edit") + "</td>";
			body += "</tr>";
		}
	}
	body += "</table></div>";

	Room room;
	if (editing >= 0 && quackmail::citadel::GetRoomByNum(ctx.con, editing, room)) {
		body += "<h2>" + T("Edit " + room.display_name) + "</h2>";
		body += FormStart(ctx, "/admin/rooms/edit");
		body += Hidden("room_num", std::to_string(room.room_num));
		body += "<label class=\"field\"><span>Name</span>" + TextInput("display_name", room.display_name) +
		        "</label>";
		body += "<label class=\"field\"><span>Floor</span>" +
		        Select("floor", FloorOptions(ctx), std::to_string(room.floor_num)) + "</label>";
		body += "<label class=\"field\"><span>Default view</span>" +
		        Select("view", ViewOptions(), std::to_string(room.default_view)) + "</label>";
		body += "<label class=\"field\"><span>List order</span>" +
		        TextInput("listorder", std::to_string(room.listorder), "number") + "</label>";
		body += "<label class=\"field\"><span>Room password</span>" + TextInput("password", room.password) +
		        "</label>";
		body += "<label class=\"field\"><span>Info</span>" + TextArea("info", room.info, 4) + "</label>";
		for (auto &f : kRoomFlags) {
			body += Checkbox(f.field, (room.qr_flags & f.bit) != 0, f.label) + " ";
		}
		body += "<p>" + Button("Save") + "</p>";
		body += FormEnd();

		if (room.room_num != quackmail::citadel::kLobbyRoom) {
			body += FormStart(ctx, "/admin/rooms/kill");
			body += Hidden("room_num", std::to_string(room.room_num));
			body += "<p>" + Button("Delete this room and its message pointers", "danger") + "</p>";
			body += FormEnd();
		}
	}

	body += "<h2>Create a room</h2>";
	body += FormStart(ctx, "/admin/rooms/add");
	body += "<label class=\"field\"><span>Name</span>" + TextInput("display_name", "") + "</label>";
	body += "<label class=\"field\"><span>Floor</span>" + Select("floor", FloorOptions(ctx), "0") +
	        "</label>";
	body += "<label class=\"field\"><span>Password (optional)</span>" + TextInput("password", "") +
	        "</label>";
	for (auto &f : kRoomFlags) {
		body += Checkbox(f.field, false, f.label) + " ";
	}
	body += "<p>" + Button("Create") + "</p>";
	body += FormEnd();

	AdminPage(ctx, "Rooms", body);
}

void PostRoomAdd(Ctx &ctx) {
	std::string name = ctx.req.Form("display_name");
	if (name.empty()) {
		BadRequest(ctx, "A room needs a name.");
		return;
	}
	std::string err;
	if (quackmail::citadel::CreateRoom(ctx.con, name, ctx.FormInt("floor", 0), FlagsFromForm(ctx),
	                                   ctx.req.Form("password"), 0, err) < 0) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/rooms", "created");
}

void PostRoomEdit(Ctx &ctx) {
	Room room;
	if (!quackmail::citadel::GetRoomByNum(ctx.con, ctx.FormInt("room_num", -1), room)) {
		NotFound(ctx);
		return;
	}
	room.display_name = ctx.req.Form("display_name");
	room.floor_num = ctx.FormInt("floor", room.floor_num);
	room.default_view = ctx.FormInt("view", room.default_view);
	room.listorder = ctx.FormInt("listorder", room.listorder);
	room.password = ctx.req.Form("password");
	room.info = ctx.req.Form("info");
	// A personal room keeps QR_MAILBOX whatever the form says, or it would stop
	// being a mailbox the moment an aide saved this page.
	room.qr_flags = FlagsFromForm(ctx) | (room.qr_flags & quackmail::citadel::QR_MAILBOX);
	std::string err;
	if (!quackmail::citadel::UpdateRoom(ctx.con, room, err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/rooms", "saved");
}

void PostRoomKill(Ctx &ctx) {
	std::string err;
	if (!quackmail::citadel::KillRoom(ctx.con, ctx.FormInt("room_num", -1), err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/rooms", "deleted");
}

// ---- floors --------------------------------------------------------------

void GetFloors(Ctx &ctx) {
	std::string body = "<div class=\"wrap\"><table><tr>" + Head("Floor") + "<th class=\"num\">Number</th>" +
	                   "<th class=\"num\">Rooms</th>" + Head("") + "</tr>";
	for (auto &f : quackmail::citadel::ListFloors(ctx.con)) {
		body += "<tr>";
		body += "<td>" + FormStart(ctx, "/admin/floors/rename", "inline") +
		        Hidden("floor_num", std::to_string(f.floor_num)) + TextInput("name", f.name) +
		        Button("Rename", "sec") + FormEnd() + "</td>";
		body += "<td class=\"num\">" + T(std::to_string(f.floor_num)) + "</td>";
		body += "<td class=\"num\">" + T(std::to_string(f.room_count)) + "</td>";
		body += "<td>";
		if (f.floor_num != 0 && f.room_count == 0) {
			body += FormStart(ctx, "/admin/floors/kill", "inline") +
			        Hidden("floor_num", std::to_string(f.floor_num)) + Button("Delete", "danger") +
			        FormEnd();
		}
		body += "</td></tr>";
	}
	body += "</table></div>";

	body += "<h2>Create a floor</h2>";
	body += FormStart(ctx, "/admin/floors/add");
	body += "<label class=\"field\"><span>Name</span>" + TextInput("name", "") + "</label>";
	body += "<p>" + Button("Create") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">A floor can only be deleted once it holds no rooms.</p>";

	AdminPage(ctx, "Floors", body);
}

void PostFloorAdd(Ctx &ctx) {
	std::string err;
	if (quackmail::citadel::CreateFloor(ctx.con, ctx.req.Form("name"), err) < 0) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/floors", "created");
}

void PostFloorRename(Ctx &ctx) {
	std::string err;
	if (!quackmail::citadel::RenameFloor(ctx.con, ctx.FormInt("floor_num", -1), ctx.req.Form("name"),
	                                     err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/floors", "saved");
}

void PostFloorKill(Ctx &ctx) {
	std::string err;
	if (!quackmail::citadel::KillFloor(ctx.con, ctx.FormInt("floor_num", -1), err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/floors", "deleted");
}

// ---- config --------------------------------------------------------------

// The keys the console offers by name, with what each does. Anything else in
// citadel_config is still shown below, read-only — a free-form "set any key"
// box would be one typo away from breaking the server with no warning.
struct ConfigKey {
	const char *name;
	const char *label;
};

const ConfigKey kConfigKeys[] = {
    {"c_nodename", "Node name"},
    {"c_humannode", "Human-readable name"},
    {"c_fqdn", "Fully-qualified host name"},
    {"c_bbs_city", "Location"},
    {"c_sysadm", "System administrator"},
    {"qm_web_force_https", "Redirect HTTP to HTTPS (1/0)"},
    {"qm_web_trusted_proxies", "Trusted reverse proxies (CIDR list)"},
    {"qm_web_origins", "Allowed form origins"},
    {"qm_web_hsts", "Send HSTS over TLS (1/0)"},
    {"qm_web_admin_enabled", "Web admin console enabled (1/0)"},
    {"qm_web_admin_require_tls", "Web admin console requires TLS (1/0)"},
    {"qm_spf_reject", "Reject on SPF failure (1/0)"},
    {"qm_dkim_reject", "Reject on DKIM failure (1/0)"},
    {"qm_dmarc_enforce", "Honour the sender's DMARC policy (1/0)"},
    {"qm_rbl_reject", "Reject listed clients (1/0)"},
    {"qm_quarantine_room", "Quarantine folder"},
};

void GetConfig(Ctx &ctx) {
	std::string body = FormStart(ctx, "/admin/config/set");
	body += "<div class=\"wrap\"><table><tr>" + Head("Setting") + Head("Key") + Head("Value") + "</tr>";
	for (auto &key : kConfigKeys) {
		body += "<tr>";
		body += Cell(key.label);
		body += "<td><code>" + T(key.name) + "</code></td>";
		body += "<td>" + TextInput(std::string("v_") + key.name, ConfigStr(ctx.con, key.name, "")) +
		        "</td>";
		body += "</tr>";
	}
	body += "</table></div>";
	body += RawHtml(ReAuthField());
	body += "<p>" + Button("Save configuration") + "</p>";
	body += FormEnd();

	body += "<h2>Everything in citadel_config</h2>";
	body += "<div class=\"wrap\"><table><tr>" + Head("Key") + Head("Value") + "</tr>";
	auto r = ctx.con.Query("SELECT name, value FROM citadel_config ORDER BY name");
	if (!r->HasError()) {
		auto &mat = r->Cast<MaterializedQueryResult>();
		for (idx_t i = 0; i < mat.RowCount(); i++) {
			std::string name = mat.GetValue(0, i).ToString();
			// qm_web_secret is the key CSRF tokens are derived from: a server
			// secret, not a setting, and it never goes on a page.
			std::string value = name == "qm_web_secret" ? "(hidden)" : mat.GetValue(1, i).ToString();
			body += "<tr>" + Cell(name) + Cell(value) + "</tr>";
		}
	}
	body += "</table></div>";

	AdminPage(ctx, "Configuration", body);
}

void PostConfigSet(Ctx &ctx) {
	if (!ReAuth(ctx)) {
		ReAuthFailed(ctx);
		return;
	}
	for (auto &key : kConfigKeys) {
		std::string field = std::string("v_") + key.name;
		if (!ctx.req.HasForm(field)) {
			continue;
		}
		Exec(ctx.con,
		     "INSERT INTO citadel_config (name, value) VALUES ($1, $2) "
		     "ON CONFLICT (name) DO UPDATE SET value = excluded.value",
		     {Value(key.name), Value(ctx.req.Form(field))});
	}
	RedirectTo(ctx, "/admin/config", "saved");
}

} // namespace

void RegisterAdminUserRoutes(std::vector<Route> &out) {
	out.push_back({"GET", "/admin/", Role::Aide, GetAdminIndex});
	out.push_back({"GET", "/admin/users", Role::Aide, GetUsers});
	out.push_back({"POST", "/admin/users/add", Role::Aide, PostUserAdd});
	out.push_back({"POST", "/admin/users/passwd", Role::Aide, PostUserPasswd});
	out.push_back({"POST", "/admin/users/remove", Role::Aide, PostUserRemove});
	out.push_back({"POST", "/admin/users/axlevel", Role::Aide, PostUserAxlevel});
	out.push_back({"POST", "/admin/users/enable", Role::Aide, PostUserEnable});
	out.push_back({"GET", "/admin/rooms", Role::Aide, GetRooms});
	out.push_back({"POST", "/admin/rooms/add", Role::Aide, PostRoomAdd});
	out.push_back({"POST", "/admin/rooms/edit", Role::Aide, PostRoomEdit});
	out.push_back({"POST", "/admin/rooms/kill", Role::Aide, PostRoomKill});
	out.push_back({"GET", "/admin/floors", Role::Aide, GetFloors});
	out.push_back({"POST", "/admin/floors/add", Role::Aide, PostFloorAdd});
	out.push_back({"POST", "/admin/floors/rename", Role::Aide, PostFloorRename});
	out.push_back({"POST", "/admin/floors/kill", Role::Aide, PostFloorKill});
	out.push_back({"GET", "/admin/config", Role::Aide, GetConfig});
	out.push_back({"POST", "/admin/config/set", Role::Aide, PostConfigSet});
}

} // namespace qmweb
} // namespace duckdb
