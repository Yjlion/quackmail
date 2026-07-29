#include "web.hpp"

#include "quackmail/dkim.hpp"
#include "quackmail/mailpolicy.hpp"

#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {
namespace qmweb {

namespace policy = quackmail::policy;

namespace {

// ---- hosted domains ------------------------------------------------------

void GetDomains(Ctx &ctx) {
	std::string body = "<div class=\"wrap\"><table><tr>" + Head("Domain") + Head("Kind") + Head("Enabled") +
	                   Head("DKIM selector") + Head("Note") + Head("") + "</tr>";
	for (auto &d : policy::ListDomains(ctx.con)) {
		body += "<tr>";
		body += Cell(d.domain);
		body += Cell(d.kind);
		body += Cell(d.enabled ? "yes" : "no");
		body += Cell(d.dkim_selector);
		body += Cell(d.note);
		body += "<td>" + FormStart(ctx, "/admin/domains/remove", "inline") + Hidden("domain", d.domain) +
		        Button("Remove", "danger") + FormEnd() + "</td>";
		body += "</tr>";
	}
	body += "</table></div>";

	body += "<h2>Add a domain</h2>";
	body += FormStart(ctx, "/admin/domains/add");
	body += "<label class=\"field\"><span>Domain</span>" + TextInput("domain", "") + "</label>";
	body += "<label class=\"field\"><span>Kind</span>" +
	        Select("kind", {{"local", "local — deliver here"}, {"relay", "relay — forward onward"}},
	               "local") +
	        "</label>";
	body += "<p>" + Button("Add") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">Mail is always accepted for <code>c_fqdn</code>; these are the extra "
	        "domains this server answers for.</p>";

	AdminPage(ctx, "Hosted domains", body);
}

void PostDomainAdd(Ctx &ctx) {
	std::string err;
	if (!policy::AddDomain(ctx.con, ctx.req.Form("domain"), ctx.req.Form("kind"), err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/domains", "created");
}

void PostDomainRemove(Ctx &ctx) {
	std::string err;
	if (!policy::RemoveDomain(ctx.con, ctx.req.Form("domain"), err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/domains", "deleted");
}

// ---- aliases -------------------------------------------------------------

void GetAliases(Ctx &ctx) {
	std::string body = "<div class=\"wrap\"><table><tr>" + Head("Alias") + Head("Destination") +
	                   Head("Enabled") + Head("") + "</tr>";
	auto r = ctx.con.Query("SELECT alias, destination, enabled FROM quackmail_aliases "
	                       "ORDER BY alias, destination");
	if (!r->HasError()) {
		auto &mat = r->Cast<MaterializedQueryResult>();
		for (idx_t i = 0; i < mat.RowCount(); i++) {
			std::string alias = mat.GetValue(0, i).ToString();
			std::string dest = mat.GetValue(1, i).ToString();
			body += "<tr>";
			body += Cell(alias);
			body += Cell(dest);
			body += Cell(mat.GetValue(2, i).ToString());
			body += "<td>" + FormStart(ctx, "/admin/aliases/remove", "inline") + Hidden("alias", alias) +
			        Hidden("destination", dest) + Button("Remove", "danger") + FormEnd() + "</td>";
			body += "</tr>";
		}
	}
	body += "</table></div>";

	body += "<h2>Add an alias</h2>";
	body += FormStart(ctx, "/admin/aliases/add");
	body += "<label class=\"field\"><span>Alias</span>" +
	        TextInput("alias", "", "text", "info@example.com or @example.com") + "</label>";
	body += "<label class=\"field\"><span>Destination</span>" + TextInput("destination", "") + "</label>";
	body += "<p>" + Button("Add") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">Several rows for one alias fan the mail out to several people. "
	        "<code>@example.com</code> is that domain's catch-all. A destination that is not a local user "
	        "is forwarded onto the outbound queue.</p>";

	AdminPage(ctx, "Aliases", body);
}

void PostAliasAdd(Ctx &ctx) {
	std::string err;
	if (!policy::AddAlias(ctx.con, ctx.req.Form("alias"), ctx.req.Form("destination"), err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/aliases", "created");
}

void PostAliasRemove(Ctx &ctx) {
	std::string err;
	if (!policy::RemoveAlias(ctx.con, ctx.req.Form("alias"), ctx.req.Form("destination"), err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/aliases", "deleted");
}

// ---- access rules --------------------------------------------------------

void GetAcl(Ctx &ctx) {
	std::string body = "<div class=\"wrap\"><table><tr>" + Head("ID") + Head("Scope") + Head("Pattern") +
	                   Head("Action") + Head("Enabled") + Head("Note") + Head("") + "</tr>";
	auto r = ctx.con.Query("SELECT id, scope, pattern, action, enabled, note FROM quackmail_acl ORDER BY id");
	if (!r->HasError()) {
		auto &mat = r->Cast<MaterializedQueryResult>();
		for (idx_t i = 0; i < mat.RowCount(); i++) {
			std::string id = mat.GetValue(0, i).ToString();
			body += "<tr>";
			for (idx_t c = 0; c < 6; c++) {
				auto v = mat.GetValue(c, i);
				body += Cell(v.IsNull() ? "" : v.ToString());
			}
			body += "<td>" + FormStart(ctx, "/admin/acl/remove", "inline") + Hidden("id", id) +
			        Button("Remove", "danger") + FormEnd() + "</td>";
			body += "</tr>";
		}
	}
	body += "</table></div>";

	body += "<h2>Add a rule</h2>";
	body += FormStart(ctx, "/admin/acl/add");
	body += "<label class=\"field\"><span>Scope</span>" +
	        Select("scope",
	               {{"ip", "ip — connecting client"},
	                {"sender", "sender — MAIL FROM"},
	                {"domain", "domain — sender's domain"},
	                {"rcpt", "rcpt — recipient"},
	                {"helo", "helo — announced name"},
	                {"webadmin", "webadmin — who may reach this console"}},
	               "ip") +
	        "</label>";
	body += "<label class=\"field\"><span>Pattern</span>" +
	        TextInput("pattern", "", "text", "glob, or CIDR for ip/webadmin") + "</label>";
	body += "<label class=\"field\"><span>Action</span>" +
	        Select("action", {{"block", "block"}, {"allow", "allow"}}, "block") + "</label>";
	body += "<label class=\"field\"><span>Note</span>" + TextInput("note", "") + "</label>";
	body += "<p>" + Button("Add") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">Allow always beats block, so a narrow allow carves an exception out of a "
	        "broad block. To restrict this console to one network: block <code>webadmin *</code>, then "
	        "allow <code>webadmin 10.0.0.0/8</code>.</p>";

	AdminPage(ctx, "Access rules", body);
}

void PostAclAdd(Ctx &ctx) {
	std::string err;
	if (!policy::AddAcl(ctx.con, ctx.req.Form("scope"), ctx.req.Form("pattern"), ctx.req.Form("action"),
	                    ctx.req.Form("note"), err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/acl", "created");
}

void PostAclRemove(Ctx &ctx) {
	std::string err;
	if (!policy::RemoveAcl(ctx.con, ctx.FormInt("id", -1), err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/acl", "deleted");
}

// ---- DNSBL ---------------------------------------------------------------

void GetRbl(Ctx &ctx) {
	std::string body = "<div class=\"wrap\"><table><tr>" + Head("Zone") + Head("") + "</tr>";
	for (auto &zone : policy::RblZones(ctx.con)) {
		body += "<tr>" + Cell(zone) + "<td>" + FormStart(ctx, "/admin/rbl/remove", "inline") +
		        Hidden("zone", zone) + Button("Remove", "danger") + FormEnd() + "</td></tr>";
	}
	body += "</table></div>";

	body += "<h2>Add a zone</h2>";
	body += FormStart(ctx, "/admin/rbl/add");
	body += "<label class=\"field\"><span>Zone</span>" +
	        TextInput("zone", "", "text", "zen.spamhaus.org") + "</label>";
	body += "<p>" + Button("Add") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">Empty by default — blocklist checking is opt-in. Zones are queried in "
	        "order at connection time.</p>";

	AdminPage(ctx, "Blocklists", body);
}

void PostRblAdd(Ctx &ctx) {
	std::string err;
	if (!policy::AddRblZone(ctx.con, ctx.req.Form("zone"), err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/rbl", "created");
}

void PostRblRemove(Ctx &ctx) {
	std::string err;
	if (!policy::RemoveRblZone(ctx.con, ctx.req.Form("zone"), err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/rbl", "deleted");
}

// ---- DKIM ----------------------------------------------------------------

void GetDkim(Ctx &ctx) {
	std::string body = "<div class=\"wrap\"><table><tr>" + Head("Domain") + Head("Selector") +
	                   Head("Algorithm") + Head("Enabled") + Head("DNS record") + Head("") + "</tr>";
	// WARNING: policy::ListDkimKeys returns the private half of every key. Only
	// these five fields may ever reach a page — the same projection the SQL
	// surface makes (see qm_dkim_keys in the umbrella extension). Do not add
	// `k.private_key` to anything below, including a title= or an error string.
	for (auto &k : policy::ListDkimKeys(ctx.con)) {
		std::string record = quackmail::dkim::DnsRecord(k.public_key);
		body += "<tr>";
		body += Cell(k.domain);
		body += Cell(k.selector);
		body += Cell(k.algo);
		body += Cell(k.enabled ? "yes" : "no");
		body += "<td><code>" + T(k.selector + "._domainkey." + k.domain) + "</code><br>" + Cell(record) +
		        "</td>";
		body += "<td>" + FormStart(ctx, "/admin/dkim/remove", "inline") + Hidden("domain", k.domain) +
		        Hidden("selector", k.selector) + Button("Remove", "danger") + FormEnd() + "</td>";
		body += "</tr>";
	}
	body += "</table></div>";

	body += "<h2>Generate a signing key</h2>";
	body += FormStart(ctx, "/admin/dkim/keygen");
	body += "<label class=\"field\"><span>Domain</span>" + TextInput("domain", "") + "</label>";
	body += "<label class=\"field\"><span>Selector</span>" + TextInput("selector", "default") + "</label>";
	body += "<label class=\"field\"><span>Key size</span>" +
	        Select("bits", {{"2048", "2048 bits"}, {"1024", "1024 bits"}}, "2048") + "</label>";
	body += RawHtml(ReAuthField());
	body += "<p>" + Button("Generate") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">The private half is stored in the database and is never shown here or "
	        "returned by any SQL function — the database file's permissions are its security "
	        "boundary. Publish the DNS record above before mail signed with the key goes out.</p>";

	AdminPage(ctx, "DKIM keys", body);
}

void PostDkimKeygen(Ctx &ctx) {
	if (!ReAuth(ctx)) {
		ReAuthFailed(ctx);
		return;
	}
	std::string dns_record, err;
	int bits = (int)ctx.FormInt("bits", 2048);
	if (!policy::GenerateDkimKey(ctx.con, ctx.req.Form("domain"), ctx.req.Form("selector"), bits,
	                             dns_record, err)) {
		BadRequest(ctx, err);
		return;
	}
	AideLog(ctx, "DKIM key generated",
	        "A new signing key was generated. Publish the DNS record before relying on it.\n\nDomain: " +
	            ctx.req.Form("domain") + "\nSelector: " + ctx.req.Form("selector") +
	            "\nKey size: " + std::to_string(bits) + " bits");
	RedirectTo(ctx, "/admin/dkim", "keygen");
}

void PostDkimRemove(Ctx &ctx) {
	std::string err;
	if (!policy::RemoveDkimKey(ctx.con, ctx.req.Form("domain"), ctx.req.Form("selector"), err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/dkim", "deleted");
}

// ---- send quotas ---------------------------------------------------------

void GetRateLimits(Ctx &ctx) {
	std::string body = "<div class=\"wrap\"><table><tr>" + Head("User") + "<th class=\"num\">Burst</th>" +
	                   "<th class=\"num\">Window (s)</th>" + "<th class=\"num\">Per day</th>" +
	                   Head("Enabled") + "</tr>";
	for (auto &l : policy::ListRateLimits(ctx.con)) {
		body += "<tr>";
		body += Cell(l.username.empty() ? "(default)" : l.username);
		body += "<td class=\"num\">" + T(std::to_string(l.burst_max)) + "</td>";
		body += "<td class=\"num\">" + T(std::to_string(l.burst_secs)) + "</td>";
		body += "<td class=\"num\">" + T(std::to_string(l.daily_max)) + "</td>";
		body += Cell(l.enabled ? "yes" : "no");
		body += "</tr>";
	}
	body += "</table></div>";

	body += "<h2>Set a quota</h2>";
	body += FormStart(ctx, "/admin/ratelimits/set");
	body += "<label class=\"field\"><span>User (blank for the default)</span>" +
	        TextInput("username", "") + "</label>";
	body += "<label class=\"field\"><span>Messages per burst window</span>" +
	        TextInput("burst_max", "100", "number") + "</label>";
	body += "<label class=\"field\"><span>Burst window, seconds</span>" +
	        TextInput("burst_secs", "300", "number") + "</label>";
	body += "<label class=\"field\"><span>Messages per day</span>" +
	        TextInput("daily_max", "500", "number") + "</label>";
	body += "<p>" + Button("Save") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">One unit is charged per envelope recipient, on the submission port and "
	        "in webmail alike.</p>";

	AdminPage(ctx, "Send quotas", body);
}

void PostRateLimitSet(Ctx &ctx) {
	std::string err;
	if (!policy::SetRateLimit(ctx.con, ctx.req.Form("username"), ctx.FormInt("burst_max", 100),
	                          ctx.FormInt("burst_secs", 300), ctx.FormInt("daily_max", 500), err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, "/admin/ratelimits", "saved");
}

} // namespace

void RegisterAdminPolicyRoutes(std::vector<Route> &out) {
	out.push_back({"GET", "/admin/domains", Role::Aide, GetDomains});
	out.push_back({"POST", "/admin/domains/add", Role::Aide, PostDomainAdd});
	out.push_back({"POST", "/admin/domains/remove", Role::Aide, PostDomainRemove});
	out.push_back({"GET", "/admin/aliases", Role::Aide, GetAliases});
	out.push_back({"POST", "/admin/aliases/add", Role::Aide, PostAliasAdd});
	out.push_back({"POST", "/admin/aliases/remove", Role::Aide, PostAliasRemove});
	out.push_back({"GET", "/admin/acl", Role::Aide, GetAcl});
	out.push_back({"POST", "/admin/acl/add", Role::Aide, PostAclAdd});
	out.push_back({"POST", "/admin/acl/remove", Role::Aide, PostAclRemove});
	out.push_back({"GET", "/admin/rbl", Role::Aide, GetRbl});
	out.push_back({"POST", "/admin/rbl/add", Role::Aide, PostRblAdd});
	out.push_back({"POST", "/admin/rbl/remove", Role::Aide, PostRblRemove});
	out.push_back({"GET", "/admin/dkim", Role::Aide, GetDkim});
	out.push_back({"POST", "/admin/dkim/keygen", Role::Aide, PostDkimKeygen});
	out.push_back({"POST", "/admin/dkim/remove", Role::Aide, PostDkimRemove});
	out.push_back({"GET", "/admin/ratelimits", Role::Aide, GetRateLimits});
	out.push_back({"POST", "/admin/ratelimits/set", Role::Aide, PostRateLimitSet});
}

} // namespace qmweb
} // namespace duckdb
