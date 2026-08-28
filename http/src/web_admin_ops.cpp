#include "web.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>

namespace duckdb {
namespace qmweb {

namespace {

// ---- inbound audit log ---------------------------------------------------

void GetInbound(Ctx &ctx) {
	// mailpolicy.hpp has LogInbound but no read API, so this is raw SQL — the
	// same thing quackcitadm.sh does for the tables without list functions.
	std::string rcpt = ctx.req.Param("rcpt");
	std::string disposition = ctx.req.Param("disposition");
	int64_t limit = std::min<int64_t>(std::max<int64_t>(ctx.ParamInt("n", 200), 10), 2000);

	std::string sql = "SELECT logged_at, client_ip, helo, mail_from, rcpt, spf, dkim, dmarc, rbl, "
	                  "disposition, detail FROM quackmail_inbound_log WHERE 1 = 1";
	duckdb::vector<Value> params;
	if (!rcpt.empty()) {
		sql += " AND rcpt = $" + std::to_string(params.size() + 1);
		params.push_back(Value(rcpt));
	}
	if (!disposition.empty()) {
		sql += " AND disposition = $" + std::to_string(params.size() + 1);
		params.push_back(Value(disposition));
	}
	sql += " ORDER BY logged_at DESC LIMIT " + std::to_string(limit);

	std::string body = "<form method=\"get\" action=\"/admin/inbound\">";
	body += "<label class=\"field\"><span>Recipient</span>" + TextInput("rcpt", rcpt) + "</label>";
	body += "<label class=\"field\"><span>Disposition</span>" + TextInput("disposition", disposition) +
	        "</label>";
	body += "<p>" + Button("Filter") + "</p></form>";

	body += "<div class=\"wrap\"><table><tr>" + Head("When") + Head("Client") + Head("HELO") +
	        Head("From") + Head("To") + Head("SPF") + Head("DKIM") + Head("DMARC") + Head("DNSBL") +
	        Head("Disposition") + Head("Detail") + "</tr>";
	auto stmt = ctx.con.Prepare(sql);
	if (!stmt->HasError()) {
		auto r = stmt->Execute(params, false);
		if (!r->HasError()) {
			auto &mat = r->Cast<MaterializedQueryResult>();
			for (idx_t i = 0; i < mat.RowCount(); i++) {
				body += "<tr>";
				for (idx_t c = 0; c < mat.ColumnCount(); c++) {
					auto v = mat.GetValue(c, i);
					body += Cell(v.IsNull() ? "" : v.ToString());
				}
				body += "</tr>";
			}
		}
	}
	body += "</table></div>";
	body += "<p class=\"muted\">Every accepted and rejected recipient is logged here with the verdict "
	        "each check reached. Most failures are reported rather than rejected — see the enforcement "
	        "toggles under Config.</p>";

	AdminPage(ctx, "Inbound audit log", body);
}

// ---- outbound queue ------------------------------------------------------

void GetQueue(Ctx &ctx) {
	std::string body;
	Table table(ctx, "admin-queue",
	            {Column::Num("id", "ID"), Column("from", "From"), Column("to", "To"),
	             Column("status", "Status"), Column::Num("attempts", "Attempts"),
	             Column("next", "Next attempt"), Column("error", "Last error"), Column("", "")});
	auto r = ctx.con.Query("SELECT id, from_addr, rcpt, status, attempts, next_attempt_at, last_error "
	                       "FROM quackmail_outbound ORDER BY id DESC LIMIT 500");
	if (!r->HasError()) {
		auto &mat = r->Cast<MaterializedQueryResult>();
		for (idx_t i = 0; i < mat.RowCount(); i++) {
			std::string id = mat.GetValue(0, i).ToString();
			auto text = [&](idx_t col) {
				return mat.GetValue(col, i).IsNull() ? std::string() : mat.GetValue(col, i).ToString();
			};
			table.Add()
			    .Text(id)
			    .Text(text(1))
			    .Text(text(2))
			    .Text(text(3))
			    .Number((int64_t)std::strtoll(text(4).c_str(), nullptr, 10))
			    .Text(text(5))
			    .Text(text(6))
			    .Html(FormStart(ctx, "/admin/queue/retry", "inline") + Hidden("id", id) +
			          Button("Retry now", "sec") + FormEnd());
		}
	}
	body += table.Render();

	body += FormStart(ctx, "/admin/queue/flush");
	body += "<p>" + Button("Retry everything now", "sec") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">The relay drainer picks up anything whose next attempt is due.</p>";

	AdminPage(ctx, "Outbound queue", body);
}

void PostQueueRetry(Ctx &ctx) {
	Exec(ctx.con,
	     "UPDATE quackmail_outbound SET status = 'queued', next_attempt_at = now() WHERE id = $1",
	     {Value::BIGINT(ctx.FormInt("id", -1))});
	RedirectTo(ctx, "/admin/queue", "queued");
}

void PostQueueFlush(Ctx &ctx) {
	ctx.con.Query("UPDATE quackmail_outbound SET status = 'queued', next_attempt_at = now() "
	              "WHERE status <> 'sent'");
	RedirectTo(ctx, "/admin/queue", "queued");
}

// ---- Sieve for any user --------------------------------------------------

std::string TargetUser(Ctx &ctx, bool from_form) {
	std::string user = from_form ? ctx.req.Form("user") : ctx.req.Param("user");
	return user.empty() ? ctx.username : user;
}

void GetAdminSieve(Ctx &ctx) {
	std::string user = TargetUser(ctx, false);
	std::string body = "<form method=\"get\" action=\"/admin/sieve\">";
	body += "<label class=\"field\"><span>User</span>" + TextInput("user", user) + "</label>";
	body += "<p>" + Button("Show") + "</p></form>";
	if (quackmail::citadel::GetOrAssignUserNum(ctx.con, user) <= 0) {
		body += "<p class=\"muted\">No such user.</p>";
		AdminPage(ctx, "Mail filters", body);
		return;
	}
	body += SieveSection(ctx, user, "/admin/sieve");
	AdminPage(ctx, "Mail filters for " + user, body);
}

void PostAdminSieveSave(Ctx &ctx) {
	std::string user = TargetUser(ctx, true);
	std::string err;
	if (!SieveSave(ctx, user, ctx.req.Form("name"), ctx.req.Form("script"), err)) {
		ErrorPage(ctx, 400, "The script was not saved", err);
		return;
	}
	RedirectTo(ctx, "/admin/sieve?user=" + http::PercentEncode(user), "saved");
}

void PostAdminSieveActivate(Ctx &ctx) {
	std::string user = TargetUser(ctx, true);
	SieveActivate(ctx, user, ctx.req.Form("name"));
	RedirectTo(ctx, "/admin/sieve?user=" + http::PercentEncode(user), "activated");
}

void PostAdminSieveDelete(Ctx &ctx) {
	std::string user = TargetUser(ctx, true);
	Exec(ctx.con, "DELETE FROM quackmail_sieve_scripts WHERE username = $1 AND name = $2",
	     {Value(user), Value(ctx.req.Form("name"))});
	RedirectTo(ctx, "/admin/sieve?user=" + http::PercentEncode(user), "deleted");
}

// The rule builder's own controls. SieveSection has always rendered them here
// too, and until these existed every one of them posted to a route that was
// never registered — so the whole builder was decorative on this page while
// looking identical to the one on /prefs/sieve that works.
void PostAdminRuleAdd(Ctx &ctx) {
	SieveRuleAdd(ctx, "/admin/sieve");
}
void PostAdminRuleDelete(Ctx &ctx) {
	SieveRuleDelete(ctx, "/admin/sieve");
}
void PostAdminRuleMove(Ctx &ctx) {
	SieveRuleMove(ctx, "/admin/sieve");
}
void PostAdminRuleTestAdd(Ctx &ctx) {
	SieveRuleTestAdd(ctx, "/admin/sieve");
}
void PostAdminRuleTestDelete(Ctx &ctx) {
	SieveRuleTestDelete(ctx, "/admin/sieve");
}
void PostAdminRuleActionAdd(Ctx &ctx) {
	SieveRuleActionAdd(ctx, "/admin/sieve");
}
void PostAdminRuleActionDelete(Ctx &ctx) {
	SieveRuleActionDelete(ctx, "/admin/sieve");
}
void PostAdminRuleMatch(Ctx &ctx) {
	SieveRuleMatch(ctx, "/admin/sieve");
}
void PostAdminRuleStop(Ctx &ctx) {
	SieveRuleStop(ctx, "/admin/sieve");
}

// ---- presence and browser sessions ---------------------------------------

void GetAdminWho(Ctx &ctx) {
	std::string body;
	Table table(ctx, "admin-sessions",
	            {Column::Num("session", "Session"), Column("user", "User"), Column("room", "Room"),
	             Column("from", "From"), Column("client", "Client"), Column("doing", "Doing"),
	             Column::Num("access", "Access"), Column("since", "Since", "", true)});
	for (auto &s : quackmail::citadel::ListSessions(ctx.con)) {
		table.Add()
		    .Number(s.session_id)
		    .Text(s.username.empty() ? "(signing in)" : s.username)
		    .Text(s.room)
		    .Text(s.host)
		    .Text(s.client)
		    .Text(s.last_cmd)
		    .Number(s.axlevel)
		    .Html(T(FormatTime(ctx, s.since)), std::to_string(s.since));
	}
	body += table.Render();
	body += "<p class=\"muted\">These are live front-end sessions, browsers included. A front-end that "
	        "declares a heartbeat — a browser, an XMPP client — is held to it and its row is swept "
	        "within a few minutes of going quiet. One that cannot (a Citadel or telnet session sits "
	        "blocked in a read, with no timer to hang a heartbeat off) unregisters itself on a clean "
	        "disconnect, and is otherwise swept after <code>qm_session_stale_secs</code>.</p>";
	AdminPage(ctx, "Sessions online", body);
}

void GetWebSessions(Ctx &ctx) {
	auto rows = quackmail::web::ListSessions(ctx.con, "");
	std::string body = WebSessionTable(ctx, rows, "/admin/websessions/revoke", true);
	body += "<p class=\"muted\">Signing a browser out here is immediate: the row is the session.</p>";
	AdminPage(ctx, "Signed-in browsers", body);
}

void PostRevokeWebSession(Ctx &ctx) {
	// An aide may revoke anyone's, so no username qualifier.
	quackmail::web::RevokeByHash(ctx.con, ctx.req.Form("token_hash"), "");
	RedirectTo(ctx, "/admin/websessions", "revoked");
}

} // namespace

void RegisterAdminOpsRoutes(std::vector<Route> &out) {
	out.push_back({"GET", "/admin/inbound", Role::Aide, GetInbound});
	out.push_back({"GET", "/admin/queue", Role::Aide, GetQueue});
	out.push_back({"POST", "/admin/queue/retry", Role::Aide, PostQueueRetry});
	out.push_back({"POST", "/admin/queue/flush", Role::Aide, PostQueueFlush});
	out.push_back({"GET", "/admin/sieve", Role::Aide, GetAdminSieve});
	out.push_back({"POST", "/admin/sieve/save", Role::Aide, PostAdminSieveSave});
	out.push_back({"POST", "/admin/sieve/activate", Role::Aide, PostAdminSieveActivate});
	out.push_back({"POST", "/admin/sieve/delete", Role::Aide, PostAdminSieveDelete});
	out.push_back({"POST", "/admin/sieve/rule/add", Role::Aide, PostAdminRuleAdd});
	out.push_back({"POST", "/admin/sieve/rule/delete", Role::Aide, PostAdminRuleDelete});
	out.push_back({"POST", "/admin/sieve/rule/move", Role::Aide, PostAdminRuleMove});
	out.push_back({"POST", "/admin/sieve/rule/test/add", Role::Aide, PostAdminRuleTestAdd});
	out.push_back({"POST", "/admin/sieve/rule/test/delete", Role::Aide, PostAdminRuleTestDelete});
	out.push_back({"POST", "/admin/sieve/rule/action/add", Role::Aide, PostAdminRuleActionAdd});
	out.push_back({"POST", "/admin/sieve/rule/action/delete", Role::Aide, PostAdminRuleActionDelete});
	out.push_back({"POST", "/admin/sieve/rule/match", Role::Aide, PostAdminRuleMatch});
	out.push_back({"POST", "/admin/sieve/rule/stop", Role::Aide, PostAdminRuleStop});
	out.push_back({"GET", "/admin/who", Role::Aide, GetAdminWho});
	out.push_back({"GET", "/admin/websessions", Role::Aide, GetWebSessions});
	out.push_back({"POST", "/admin/websessions/revoke", Role::Aide, PostRevokeWebSession});
}

} // namespace qmweb
} // namespace duckdb
