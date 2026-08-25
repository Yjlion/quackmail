#include "web.hpp"

#include "quackmail/acme.hpp"
#include "quackmail/util.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <ctime>

namespace duckdb {
namespace qmweb {

namespace {

// Neither the account key nor a certificate key is read on this page, let alone
// rendered. The account's *thumbprint* is shown instead: it is public by
// construction, it is what a challenge has to serve, and it is the value worth
// comparing against what a CA reports. test/integration/test_http.py asserts
// that no private key reaches any page; this page is inside that assertion.
std::string Thumbprint(Ctx &ctx, const std::string &key_pem) {
	std::string thumb;
	std::string err;
	quackmail::acme::JwkThumbprint(key_pem, thumb, err);
	(void)ctx;
	return thumb;
}

std::string Ago(Ctx &ctx, int64_t when) {
	return when > 0 ? FormatTime(ctx, when) : std::string("—");
}

void GetAcme(Ctx &ctx) {
	quackmail::acme::EnsureSchema(ctx.con);
	const quackmail::acme::Config cfg = quackmail::acme::LoadConfig(ctx.con);
	const int64_t now = (int64_t)std::time(nullptr);

	std::string body;

	if (!cfg.enabled) {
		body += "<div class=\"warnbar\">Automatic certificates are switched off. "
		        "Set <code>qm_acme_enabled</code> below to turn them on.</div>";
	}
	if (cfg.directory_url.find("staging") != std::string::npos) {
		body += "<div class=\"flash\">This is the <strong>staging</strong> directory. "
		        "Certificates it issues are not trusted by anything — which is what you want "
		        "until the challenge works, because a failed order counts against a rate limit "
		        "on the production one.</div>";
	}

	// ---- account ---------------------------------------------------------
	body += "<h2>Account</h2>";
	auto acct = Exec(ctx.con,
	                  "SELECT account_url, contact, key_pem, tos_agreed, created "
	                  "  FROM quackmail_acme_accounts WHERE directory_url = $1",
	                  {Value(cfg.directory_url)});
	body += "<table class=\"list\"><tbody>";
	body += "<tr><th>Directory</th><td><code>" + T(cfg.directory_url) + "</code></td></tr>";
	if (acct) {
		auto &m = acct->Cast<MaterializedQueryResult>();
		if (m.RowCount() > 0) {
			body += "<tr><th>Account</th><td><code>" +
			        T(m.GetValue(0, 0).IsNull() ? "" : m.GetValue(0, 0).ToString()) +
			        "</code></td></tr>";
			body += "<tr><th>Contact</th><td>" +
			        T(m.GetValue(1, 0).IsNull() ? "" : m.GetValue(1, 0).ToString()) + "</td></tr>";
			body += "<tr><th>Key thumbprint</th><td><code>" +
			        T(Thumbprint(ctx, m.GetValue(2, 0).IsNull() ? "" : m.GetValue(2, 0).ToString())) +
			        "</code></td></tr>";
			body += "<tr><th>Registered</th><td>" +
			        T(Ago(ctx, m.GetValue(4, 0).IsNull() ? 0 : m.GetValue(4, 0).GetValue<int64_t>())) +
			        "</td></tr>";
		} else {
			body += "<tr><th>Account</th><td class=\"muted\">Not registered yet — the first order "
			        "creates one.</td></tr>";
		}
	}
	body += "</tbody></table>";

	// ---- certificates ----------------------------------------------------
	body += "<h2>Certificates</h2>";
	auto rows = Exec(ctx.con,
	                  "SELECT o.name, o.domains, o.status, o.error, o.next_attempt, o.attempts, "
	                  "       c.not_after, c.cert_path "
	                  "  FROM quackmail_acme_orders o "
	                  "  LEFT JOIN quackmail_acme_certs c ON c.name = o.name ORDER BY o.name",
	                  {});
	bool any = false;
	if (rows) {
		auto &m = rows->Cast<MaterializedQueryResult>();
		any = m.RowCount() > 0;
		if (any) {
			body += "<div class=\"wrap\"><table><tr>" + Head("Name") + Head("Domains") +
			        Head("State") + "<th class=\"num\">Days left</th>" + Head("File") + Head("") +
			        "</tr>";
			for (idx_t i = 0; i < m.RowCount(); i++) {
				const std::string name = m.GetValue(0, i).ToString();
				const bool have = !m.GetValue(6, i).IsNull();
				const int64_t not_after = have ? m.GetValue(6, i).GetValue<int64_t>() : 0;
				const std::string err = m.GetValue(3, i).IsNull() ? "" : m.GetValue(3, i).ToString();
				const int64_t next = m.GetValue(4, i).IsNull() ? 0 : m.GetValue(4, i).GetValue<int64_t>();

				std::string state = m.GetValue(2, i).IsNull() ? "" : m.GetValue(2, i).ToString();
				if (!err.empty()) {
					state += "<div class=\"muted\">" + T(err);
					if (next > now) {
						state += "<br>Next attempt " + T(FormatTime(ctx, next)) + " (attempt " +
						         T(m.GetValue(5, i).ToString()) + ")";
					}
					state += "</div>";
				}

				body += "<tr>" + Cell(name) + Cell(m.GetValue(1, i).ToString()) +
				        "<td>" + RawHtml(state) + "</td>";
				body += "<td class=\"num\">" +
				        (have ? T(std::to_string((not_after - now) / 86400)) : std::string("—")) +
				        "</td>";
				body += "<td><code>" +
				        T(m.GetValue(7, i).IsNull() ? "" : m.GetValue(7, i).ToString()) +
				        "</code></td><td class=\"actions\">";
				body += FormStart(ctx, "/admin/acme/order", "inline") + Hidden("name", name) +
				        Hidden("domains", m.GetValue(1, i).ToString()) +
				        "<button class=\"btn sec\">Renew now</button>" + FormEnd();
				body += FormStart(ctx, "/admin/acme/revoke", "inline") + Hidden("name", name) +
				        RawHtml(ReAuthField()) +
				        "<button class=\"btn danger\" data-confirm=\"Revoke this certificate?\">"
				        "Revoke</button>" +
				        FormEnd();
				body += FormStart(ctx, "/admin/acme/forget", "inline") + Hidden("name", name) +
				        "<button class=\"btn sec\" data-confirm=\"Forget this certificate?\">"
				        "Forget</button>" +
				        FormEnd();
				body += "</td></tr>";
			}
			body += "</table></div>";
		}
	}
	if (!any) {
		body += "<p class=\"muted\">No certificates are being managed yet.</p>";
	}

	body += "<h3>Order a certificate</h3>";
	body += FormStart(ctx, "/admin/acme/order");
	body += "<label class=\"field\"><span>Name</span>" + TextInput("name", "web") + "</label>";
	body += "<p class=\"muted\">A short label. It is also the file name under the certificate "
	        "directory, so keep it to letters and digits.</p>";
	body += "<label class=\"field\"><span>Domains</span>" + TextInput("domains", "") + "</label>";
	body += "<p class=\"muted\">Comma or space separated. Every one of them must resolve to this "
	        "server and reach it on port 80: the http-01 challenge is fetched over plain HTTP, "
	        "and that is the whole of what the CA checks.</p>";
	body += "<p>" + Button("Queue the order") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">Ordering queues the work; the <code>qm_acme</code> worker performs "
	        "it, because an order takes tens of seconds against a real CA. Renewal happens "
	        "automatically once a certificate is within " +
	        T(std::to_string(cfg.renew_days)) + " days of expiry.</p>";

	// ---- reload ----------------------------------------------------------
	body += "<h3>Listeners</h3>";
	body += "<p class=\"muted\">A renewed certificate is put into service without dropping a "
	        "connection: each listener re-reads its files and the next connection gets the new "
	        "certificate. This runs automatically after every successful renewal.</p>";
	body += FormStart(ctx, "/admin/acme/reload");
	body += "<p>" + Button("Reload certificates now", "sec") + "</p>";
	body += FormEnd();

	AdminPage(ctx, "Certificates (ACME)", body);
}

void PostAcmeOrder(Ctx &ctx) {
	std::string err;
	const std::string name = ctx.req.Form("name");
	const std::string domains = ctx.req.Form("domains");
	// Queued, not run inline: an order against a real CA takes tens of seconds
	// of polling, which is not a web request's to hold. The tables are the bus
	// and the worker is the consumer.
	if (!quackmail::acme::Order(ctx.con, name, domains, err)) {
		BadRequest(ctx, err);
		return;
	}
	AideLog(ctx, "Certificate ordered",
	        "An ACME order was queued.\n\nName: " + name + "\nDomains: " + domains +
	            "\n\nThe qm_acme worker will perform it. Every name must resolve to this server "
	            "and reach it on port 80.");
	RedirectTo(ctx, "/admin/acme", "queued");
}

void PostAcmeRevoke(Ctx &ctx) {
	if (!ReAuth(ctx)) {
		ReAuthFailed(ctx);
		return;
	}
	std::string err;
	const std::string name = ctx.req.Form("name");
	if (!quackmail::acme::Revoke(ctx.con, name, 0, err)) {
		BadRequest(ctx, err);
		return;
	}
	AideLog(ctx, "Certificate revoked", "The certificate '" + name + "' was revoked.");
	RedirectTo(ctx, "/admin/acme", "revoked");
}

void PostAcmeForget(Ctx &ctx) {
	std::string err;
	const std::string name = ctx.req.Form("name");
	quackmail::acme::Forget(ctx.con, name, err);
	AideLog(ctx, "Certificate forgotten",
	        "'" + name + "' is no longer managed. The certificate itself was not revoked and the "
	        "files on disk were left alone.");
	RedirectTo(ctx, "/admin/acme", "deleted");
}

void PostAcmeReload(Ctx &ctx) {
	int ok = 0;
	int total = 0;
	for (const auto &r : quackmail::acme::ReloadListeners(ctx.con)) {
		total++;
		if (r.second == "reloaded") {
			ok++;
		}
	}
	AideLog(ctx, "Certificates reloaded",
	        std::to_string(ok) + " of " + std::to_string(total) +
	            " listeners re-read their certificate.");
	RedirectTo(ctx, "/admin/acme", ok > 0 ? "saved" : "unchanged");
}

} // namespace

void RegisterAcmeAdminRoutes(std::vector<Route> &out) {
	out.push_back({"GET", "/admin/acme", Role::Aide, GetAcme});
	out.push_back({"POST", "/admin/acme/order", Role::Aide, PostAcmeOrder});
	out.push_back({"POST", "/admin/acme/revoke", Role::Aide, PostAcmeRevoke});
	out.push_back({"POST", "/admin/acme/forget", Role::Aide, PostAcmeForget});
	out.push_back({"POST", "/admin/acme/reload", Role::Aide, PostAcmeReload});
}

} // namespace qmweb
} // namespace duckdb
