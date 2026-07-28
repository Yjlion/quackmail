#include "quackmail/mailpolicy.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/dmarc.hpp"
#include "quackmail/util.hpp"
#include "quackmail/wildmat.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace quackmail {
namespace policy {

using duckdb::Connection;
using duckdb::MaterializedQueryResult;
using duckdb::QueryResult;
using duckdb::Value;

namespace {

duckdb::unique_ptr<QueryResult> ExecP(Connection &con, const std::string &sql,
                                      duckdb::vector<Value> params) {
	auto stmt = con.Prepare(sql);
	if (stmt->HasError()) {
		return nullptr;
	}
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		return nullptr;
	}
	return r;
}

Value ScalarP(Connection &con, const std::string &sql, duckdb::vector<Value> params) {
	auto r = ExecP(con, sql, std::move(params));
	if (!r) {
		return Value();
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return Value();
	}
	return mat.GetValue(0, 0);
}

std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string Str(const Value &v) {
	return v.IsNull() ? std::string() : v.ToString();
}

int64_t Int(const Value &v, int64_t dflt = 0) {
	return v.IsNull() ? dflt : v.GetValue<int64_t>();
}

std::string DomainOf(const std::string &addr) {
	auto at = addr.rfind('@');
	return at == std::string::npos ? "" : Lower(addr.substr(at + 1));
}

// Does `ip` fall inside the CIDR block `cidr` ("192.0.2.0/24", "2001:db8::/32")?
// A bare address with no prefix is treated as a /32 or /128.
bool CidrMatch(const std::string &ip, const std::string &cidr) {
	std::string net = cidr;
	int bits = -1;
	auto slash = net.find('/');
	if (slash != std::string::npos) {
		bits = std::atoi(net.c_str() + slash + 1);
		net = net.substr(0, slash);
	}

	unsigned char a[16] = {0}, b[16] = {0};
	int size = 0;
	struct in_addr v4a, v4b;
	struct in6_addr v6a, v6b;
	if (inet_pton(AF_INET, ip.c_str(), &v4a) == 1 && inet_pton(AF_INET, net.c_str(), &v4b) == 1) {
		std::memcpy(a, &v4a.s_addr, 4);
		std::memcpy(b, &v4b.s_addr, 4);
		size = 4;
	} else if (inet_pton(AF_INET6, ip.c_str(), &v6a) == 1 &&
	           inet_pton(AF_INET6, net.c_str(), &v6b) == 1) {
		std::memcpy(a, v6a.s6_addr, 16);
		std::memcpy(b, v6b.s6_addr, 16);
		size = 16;
	} else {
		return false; // mismatched families, or not addresses at all
	}

	int total = size * 8;
	if (bits < 0 || bits > total) {
		bits = total;
	}
	int whole = bits / 8, rest = bits % 8;
	if (whole > 0 && std::memcmp(a, b, (size_t)whole) != 0) {
		return false;
	}
	if (rest == 0) {
		return true;
	}
	unsigned char mask = (unsigned char)(0xff << (8 - rest));
	return (a[whole] & mask) == (b[whole] & mask);
}

constexpr int kMaxAliasDepth = 10;

} // namespace

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

void EnsureSchema(Connection &con) {
	con.Query("CREATE SEQUENCE IF NOT EXISTS quackmail_acl_seq START 1");

	// Domains we accept mail for. c_fqdn is implicitly local, so a
	// single-domain install can leave this table empty.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_domains (
			domain        VARCHAR PRIMARY KEY,
			kind          VARCHAR DEFAULT 'local',
			enabled       BOOLEAN DEFAULT true,
			dkim_selector VARCHAR DEFAULT '',
			note          VARCHAR DEFAULT '',
			created_at    TIMESTAMP DEFAULT now()
		)
	)");

	// Address rewriting. One alias may have several destinations (a list), and
	// "@domain" is that domain's catch-all.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_aliases (
			alias       VARCHAR,
			destination VARCHAR,
			enabled     BOOLEAN DEFAULT true,
			created_at  TIMESTAMP DEFAULT now()
		)
	)");

	// Allow/block rules. Allow wins, so a narrow allow can carve an exception
	// out of a broad block.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_acl (
			id         BIGINT DEFAULT nextval('quackmail_acl_seq'),
			scope      VARCHAR,
			pattern    VARCHAR,
			action     VARCHAR,
			enabled    BOOLEAN DEFAULT true,
			note       VARCHAR DEFAULT '',
			created_at TIMESTAMP DEFAULT now()
		)
	)");

	// DNSBL zones. Deliberately seeded empty — a fresh install must not start
	// sending queries to third-party blocklists on its own.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_rbl_zones (
			zone       VARCHAR PRIMARY KEY,
			enabled    BOOLEAN DEFAULT true,
			note       VARCHAR DEFAULT '',
			created_at TIMESTAMP DEFAULT now()
		)
	)");

	// Outbound signing keys. The private half lives here, so the database file's
	// permissions are the security boundary for it.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_dkim_keys (
			domain      VARCHAR,
			selector    VARCHAR,
			private_key VARCHAR,
			public_key  VARCHAR,
			algo        VARCHAR DEFAULT 'rsa-sha256',
			headers     VARCHAR DEFAULT 'from:to:cc:subject:date:message-id:mime-version:content-type:content-transfer-encoding',
			enabled     BOOLEAN DEFAULT true,
			created_at  TIMESTAMP DEFAULT now(),
			PRIMARY KEY (domain, selector)
		)
	)");

	// Per-user send quotas. The row with username '' is the default applied to
	// everyone without their own row.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_rate_limits (
			username   VARCHAR PRIMARY KEY,
			burst_max  BIGINT DEFAULT 100,
			burst_secs BIGINT DEFAULT 300,
			daily_max  BIGINT DEFAULT 500,
			enabled    BOOLEAN DEFAULT true
		)
	)");
	con.Query("INSERT OR IGNORE INTO quackmail_rate_limits "
	          "(username, burst_max, burst_secs, daily_max) VALUES ('', 100, 300, 500)");

	// The sliding window the limiter counts over.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_send_log (
			username VARCHAR,
			rcpt     VARCHAR,
			sent_at  TIMESTAMP DEFAULT now()
		)
	)");

	// What the inbound checks decided, for after-the-fact diagnosis.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_inbound_log (
			client_ip   VARCHAR,
			helo        VARCHAR,
			mail_from   VARCHAR,
			rcpt        VARCHAR,
			spf         VARCHAR,
			dkim        VARCHAR,
			dmarc       VARCHAR,
			rbl         VARCHAR,
			disposition VARCHAR,
			detail      VARCHAR,
			-- not `at`: that is a SQL keyword (AT TIME ZONE) and the
			-- CREATE fails outright.
			logged_at   TIMESTAMP DEFAULT now()
		)
	)");

	// Enforcement defaults. INSERT OR IGNORE so an admin's changes survive.
	con.Query("INSERT OR IGNORE INTO citadel_config (name, value) VALUES "
	          "('qm_spf_reject', '0'), "
	          "('qm_dkim_reject', '0'), "
	          "('qm_dmarc_enforce', '1'), "
	          "('qm_rbl_reject', '1'), "
	          "('qm_quarantine_room', 'Junk')");
}

// ---------------------------------------------------------------------------
// Domains
// ---------------------------------------------------------------------------

bool IsLocalDomain(Connection &con, const std::string &domain) {
	if (domain.empty()) {
		return true; // a bare local-part is addressed to this host by definition
	}
	std::string want = Lower(domain);
	std::string fqdn = Lower(citadel::GetConfig(con, "c_fqdn", ""));
	if (!fqdn.empty() && want == fqdn) {
		return true;
	}
	auto v = ScalarP(con,
	                 "SELECT 1 FROM quackmail_domains "
	                 "WHERE lower(domain) = $1 AND enabled AND kind = 'local'",
	                 {Value(want)});
	return !v.IsNull();
}

std::vector<Domain> ListDomains(Connection &con) {
	std::vector<Domain> out;
	auto r = ExecP(con,
	               "SELECT domain, kind, enabled, dkim_selector, note FROM quackmail_domains "
	               "ORDER BY domain",
	               {});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
		Domain d;
		d.domain = Str(mat.GetValue(0, i));
		d.kind = Str(mat.GetValue(1, i));
		Value e = mat.GetValue(2, i);
		d.enabled = e.IsNull() ? true : e.GetValue<bool>();
		d.dkim_selector = Str(mat.GetValue(3, i));
		d.note = Str(mat.GetValue(4, i));
		out.push_back(std::move(d));
	}
	return out;
}

bool AddDomain(Connection &con, const std::string &domain, const std::string &kind, std::string &err) {
	if (domain.empty()) {
		err = "domain is required";
		return false;
	}
	std::string k = Lower(kind.empty() ? "local" : kind);
	if (k != "local" && k != "relay") {
		err = "kind must be 'local' or 'relay'";
		return false;
	}
	auto r = ExecP(con,
	               "INSERT INTO quackmail_domains (domain, kind) VALUES ($1, $2) "
	               "ON CONFLICT (domain) DO UPDATE SET kind = excluded.kind, enabled = true",
	               {Value(Lower(domain)), Value(k)});
	if (!r) {
		err = "could not add the domain";
		return false;
	}
	return true;
}

bool RemoveDomain(Connection &con, const std::string &domain, std::string &err) {
	auto r = ExecP(con, "DELETE FROM quackmail_domains WHERE lower(domain) = $1",
	               {Value(Lower(domain))});
	if (!r) {
		err = "could not remove the domain";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Aliases
// ---------------------------------------------------------------------------

namespace {

// One expansion step: exact alias match first, then the domain catch-all.
std::vector<std::string> ExpandOnce(Connection &con, const std::string &addr) {
	std::vector<std::string> out;
	auto collect = [&](const std::string &key) {
		auto r = ExecP(con,
		               "SELECT destination FROM quackmail_aliases "
		               "WHERE lower(alias) = $1 AND enabled",
		               {Value(Lower(key))});
		if (!r) {
			return;
		}
		auto &mat = r->Cast<MaterializedQueryResult>();
		for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
			std::string dest = Str(mat.GetValue(0, i));
			if (!dest.empty()) {
				out.push_back(dest);
			}
		}
	};

	collect(addr);
	if (!out.empty()) {
		return out;
	}
	// A catch-all only applies when nothing more specific matched.
	std::string domain = DomainOf(addr);
	if (!domain.empty()) {
		collect("@" + domain);
	}
	return out;
}

} // namespace

std::vector<std::string> ExpandAlias(Connection &con, const std::string &addr) {
	std::vector<std::string> frontier = ExpandOnce(con, addr);
	if (frontier.empty()) {
		return frontier;
	}

	std::vector<std::string> seen {Lower(addr)};
	std::vector<std::string> result;
	for (int depth = 0; depth < kMaxAliasDepth && !frontier.empty(); depth++) {
		std::vector<std::string> next;
		for (auto &candidate : frontier) {
			std::string key = Lower(candidate);
			// A cycle (a -> b -> a) would otherwise spin here forever.
			if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
				continue;
			}
			seen.push_back(key);

			auto further = ExpandOnce(con, candidate);
			if (further.empty()) {
				if (std::find(result.begin(), result.end(), candidate) == result.end()) {
					result.push_back(candidate);
				}
			} else {
				next.insert(next.end(), further.begin(), further.end());
			}
		}
		frontier = std::move(next);
	}
	return result;
}

bool AddAlias(Connection &con, const std::string &alias, const std::string &destination,
              std::string &err) {
	if (alias.empty() || destination.empty()) {
		err = "alias and destination are both required";
		return false;
	}
	auto r = ExecP(con, "INSERT INTO quackmail_aliases (alias, destination) VALUES ($1, $2)",
	               {Value(Lower(alias)), Value(destination)});
	if (!r) {
		err = "could not add the alias";
		return false;
	}
	return true;
}

bool RemoveAlias(Connection &con, const std::string &alias, const std::string &destination,
                 std::string &err) {
	// An empty destination removes every mapping for the alias.
	auto r = destination.empty()
	             ? ExecP(con, "DELETE FROM quackmail_aliases WHERE lower(alias) = $1",
	                     {Value(Lower(alias))})
	             : ExecP(con,
	                     "DELETE FROM quackmail_aliases WHERE lower(alias) = $1 AND destination = $2",
	                     {Value(Lower(alias)), Value(destination)});
	if (!r) {
		err = "could not remove the alias";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Access control
// ---------------------------------------------------------------------------

bool IpMatches(const std::string &ip, const std::string &pattern) {
	if (ip.empty() || pattern.empty()) {
		return false;
	}
	if (pattern.find('/') != std::string::npos) {
		return CidrMatch(ip, pattern);
	}
	return WildmatMatch(Lower(ip), Lower(pattern));
}

bool IpMatchesAny(const std::string &ip, const std::string &patterns) {
	size_t pos = 0;
	while (pos <= patterns.size()) {
		size_t comma = patterns.find(',', pos);
		std::string entry =
		    patterns.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
		while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) {
			entry.erase(0, 1);
		}
		while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\t')) {
			entry.pop_back();
		}
		if (IpMatches(ip, entry)) {
			return true;
		}
		if (comma == std::string::npos) {
			break;
		}
		pos = comma + 1;
	}
	return false;
}

AclVerdict CheckAcl(Connection &con, const std::string &scope, const std::string &value,
                    std::string &note) {
	note.clear();
	if (value.empty()) {
		return AclVerdict::None;
	}
	auto r = ExecP(con,
	               "SELECT pattern, action, note FROM quackmail_acl "
	               "WHERE lower(scope) = $1 AND enabled ORDER BY id",
	               {Value(Lower(scope))});
	if (!r) {
		return AclVerdict::None;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();

	bool blocked = false;
	std::string block_note;
	std::string lowered = Lower(value);
	// Scopes whose values are client addresses, so a pattern may be a CIDR
	// block rather than a glob. "webadmin" restricts which networks may reach
	// the web admin console; it is separate from "ip" so that blocking a
	// spam source does not also lock an operator out of the console.
	std::string lscope = Lower(scope);
	bool is_ip = lscope == "ip" || lscope == "webadmin";

	for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
		std::string pattern = Str(mat.GetValue(0, i));
		std::string action = Lower(Str(mat.GetValue(1, i)));
		std::string rule_note = Str(mat.GetValue(2, i));
		if (pattern.empty()) {
			continue;
		}

		bool hit = false;
		if (is_ip && pattern.find('/') != std::string::npos) {
			hit = CidrMatch(value, pattern);
		} else {
			hit = WildmatMatch(lowered, Lower(pattern));
		}
		if (!hit) {
			continue;
		}

		// Allow short-circuits: it is the exception mechanism, so it must beat
		// any block rule regardless of ordering.
		if (action == "allow") {
			note = rule_note.empty() ? ("allowed by rule '" + pattern + "'") : rule_note;
			return AclVerdict::Allow;
		}
		if (action == "block" && !blocked) {
			blocked = true;
			block_note = rule_note.empty() ? ("blocked by rule '" + pattern + "'") : rule_note;
		}
	}

	if (blocked) {
		note = block_note;
		return AclVerdict::Block;
	}
	return AclVerdict::None;
}

bool AddAcl(Connection &con, const std::string &scope, const std::string &pattern,
            const std::string &action, const std::string &note, std::string &err) {
	std::string s = Lower(scope), a = Lower(action);
	if (s != "ip" && s != "sender" && s != "domain" && s != "rcpt" && s != "helo") {
		err = "scope must be one of: ip, sender, domain, rcpt, helo";
		return false;
	}
	if (a != "allow" && a != "block") {
		err = "action must be 'allow' or 'block'";
		return false;
	}
	if (pattern.empty()) {
		err = "pattern is required";
		return false;
	}
	auto r = ExecP(con, "INSERT INTO quackmail_acl (scope, pattern, action, note) VALUES ($1,$2,$3,$4)",
	               {Value(s), Value(pattern), Value(a), Value(note)});
	if (!r) {
		err = "could not add the rule";
		return false;
	}
	return true;
}

bool RemoveAcl(Connection &con, int64_t id, std::string &err) {
	auto r = ExecP(con, "DELETE FROM quackmail_acl WHERE id = $1", {Value::BIGINT(id)});
	if (!r) {
		err = "could not remove the rule";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// DNSBL
// ---------------------------------------------------------------------------

std::vector<std::string> RblZones(Connection &con) {
	std::vector<std::string> out;
	auto r = ExecP(con, "SELECT zone FROM quackmail_rbl_zones WHERE enabled ORDER BY zone", {});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
		std::string z = Str(mat.GetValue(0, i));
		if (!z.empty()) {
			out.push_back(z);
		}
	}
	return out;
}

bool AddRblZone(Connection &con, const std::string &zone, std::string &err) {
	if (zone.empty()) {
		err = "zone is required";
		return false;
	}
	auto r = ExecP(con,
	               "INSERT INTO quackmail_rbl_zones (zone) VALUES ($1) "
	               "ON CONFLICT (zone) DO UPDATE SET enabled = true",
	               {Value(Lower(zone))});
	if (!r) {
		err = "could not add the zone";
		return false;
	}
	return true;
}

bool RemoveRblZone(Connection &con, const std::string &zone, std::string &err) {
	auto r = ExecP(con, "DELETE FROM quackmail_rbl_zones WHERE lower(zone) = $1", {Value(Lower(zone))});
	if (!r) {
		err = "could not remove the zone";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// DKIM keys
// ---------------------------------------------------------------------------

namespace {

bool LoadKey(Connection &con, const std::string &domain, DkimKey &out) {
	auto r = ExecP(con,
	               "SELECT domain, selector, private_key, public_key, algo, headers "
	               "FROM quackmail_dkim_keys WHERE lower(domain) = $1 AND enabled "
	               "ORDER BY created_at DESC LIMIT 1",
	               {Value(Lower(domain))});
	if (!r) {
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return false;
	}
	out.domain = Str(mat.GetValue(0, 0));
	out.selector = Str(mat.GetValue(1, 0));
	out.private_key = Str(mat.GetValue(2, 0));
	out.public_key = Str(mat.GetValue(3, 0));
	out.algo = Str(mat.GetValue(4, 0));
	out.headers = Str(mat.GetValue(5, 0));
	out.enabled = true;
	return true;
}

} // namespace

bool DkimKeyFor(Connection &con, const std::string &domain, DkimKey &out) {
	if (domain.empty()) {
		return false;
	}
	if (LoadKey(con, domain, out)) {
		return true;
	}
	// Fall back to the organizational domain, so one key on example.com signs
	// mail from every subdomain that does not have its own.
	std::string org = dmarc::OrganizationalDomain(domain);
	if (org != Lower(domain) && !org.empty()) {
		return LoadKey(con, org, out);
	}
	return false;
}

std::vector<DkimKey> ListDkimKeys(Connection &con) {
	std::vector<DkimKey> out;
	auto r = ExecP(con,
	               "SELECT domain, selector, public_key, algo, headers, enabled "
	               "FROM quackmail_dkim_keys ORDER BY domain, selector",
	               {});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
		DkimKey k;
		k.domain = Str(mat.GetValue(0, i));
		k.selector = Str(mat.GetValue(1, i));
		// The private half is deliberately not returned by the listing.
		k.public_key = Str(mat.GetValue(2, i));
		k.algo = Str(mat.GetValue(3, i));
		k.headers = Str(mat.GetValue(4, i));
		Value e = mat.GetValue(5, i);
		k.enabled = e.IsNull() ? true : e.GetValue<bool>();
		out.push_back(std::move(k));
	}
	return out;
}

bool GenerateDkimKey(Connection &con, const std::string &domain, const std::string &selector,
                     int bits, std::string &dns_record, std::string &err) {
	if (domain.empty() || selector.empty()) {
		err = "domain and selector are both required";
		return false;
	}
	std::string priv, pub;
	if (!dkim::GenerateKey(bits, priv, pub, err)) {
		return false;
	}
	auto r = ExecP(con,
	               "INSERT INTO quackmail_dkim_keys (domain, selector, private_key, public_key) "
	               "VALUES ($1, $2, $3, $4) "
	               "ON CONFLICT (domain, selector) DO UPDATE SET "
	               "  private_key = excluded.private_key, public_key = excluded.public_key, "
	               "  enabled = true",
	               {Value(Lower(domain)), Value(selector), Value(priv), Value(pub)});
	if (!r) {
		err = "could not store the generated key";
		return false;
	}
	// Record the selector on the domain row so signing picks it up without a
	// second lookup.
	ExecP(con,
	      "INSERT INTO quackmail_domains (domain, dkim_selector) VALUES ($1, $2) "
	      "ON CONFLICT (domain) DO UPDATE SET dkim_selector = excluded.dkim_selector",
	      {Value(Lower(domain)), Value(selector)});

	dns_record = dkim::DnsRecord(pub);
	return true;
}

bool AddDkimKey(Connection &con, const std::string &domain, const std::string &selector,
                const std::string &private_key_pem, std::string &err) {
	if (domain.empty() || selector.empty() || private_key_pem.empty()) {
		err = "domain, selector and private key are all required";
		return false;
	}
	auto r = ExecP(con,
	               "INSERT INTO quackmail_dkim_keys (domain, selector, private_key, public_key) "
	               "VALUES ($1, $2, $3, '') "
	               "ON CONFLICT (domain, selector) DO UPDATE SET "
	               "  private_key = excluded.private_key, enabled = true",
	               {Value(Lower(domain)), Value(selector), Value(private_key_pem)});
	if (!r) {
		err = "could not store the key";
		return false;
	}
	return true;
}

bool RemoveDkimKey(Connection &con, const std::string &domain, const std::string &selector,
                   std::string &err) {
	auto r = ExecP(con,
	               "DELETE FROM quackmail_dkim_keys WHERE lower(domain) = $1 AND selector = $2",
	               {Value(Lower(domain)), Value(selector)});
	if (!r) {
		err = "could not remove the key";
		return false;
	}
	return true;
}

dkim::KeyLookup DkimKeyLookup(Connection &con) {
	// Captures the connection by reference: the returned callback is used only
	// within the caller's scope (one SMTP transaction), never stored.
	Connection *c = &con;
	return [c](const std::string &selector, const std::string &domain, std::string &txt) -> bool {
		auto v = ScalarP(*c,
		                 "SELECT public_key FROM quackmail_dkim_keys "
		                 "WHERE lower(domain) = $1 AND selector = $2 AND enabled",
		                 {Value(Lower(domain)), Value(selector)});
		std::string pub = Str(v);
		if (pub.empty()) {
			return false;
		}
		txt = dkim::DnsRecord(pub);
		return true;
	};
}

// ---------------------------------------------------------------------------
// Rate limiting
// ---------------------------------------------------------------------------

namespace {

RateLimit LoadLimit(Connection &con, const std::string &username) {
	RateLimit rl;
	rl.username = username;
	auto r = ExecP(con,
	               "SELECT username, burst_max, burst_secs, daily_max, enabled "
	               "FROM quackmail_rate_limits WHERE username IN ($1, '') "
	               // The user's own row (non-empty username) sorts first, so it
	               // wins over the default row when both exist.
	               "ORDER BY username DESC LIMIT 1",
	               {Value(username)});
	if (!r) {
		return rl;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return rl;
	}
	rl.username = Str(mat.GetValue(0, 0));
	rl.burst_max = Int(mat.GetValue(1, 0), 100);
	rl.burst_secs = Int(mat.GetValue(2, 0), 300);
	rl.daily_max = Int(mat.GetValue(3, 0), 500);
	Value e = mat.GetValue(4, 0);
	rl.enabled = e.IsNull() ? true : e.GetValue<bool>();
	return rl;
}

} // namespace

RateVerdict CheckRate(Connection &con, const std::string &username, int64_t count) {
	RateVerdict v;
	if (count < 1) {
		count = 1;
	}
	v.limit = LoadLimit(con, username);
	if (!v.limit.enabled) {
		return v; // limiting switched off for this user
	}

	int64_t burst_secs = v.limit.burst_secs > 0 ? v.limit.burst_secs : 300;
	v.burst_used = Int(ScalarP(con,
	                           "SELECT count(*) FROM quackmail_send_log "
	                           "WHERE username = $1 AND sent_at > now() - ($2 * INTERVAL 1 SECOND)",
	                           {Value(username), Value::BIGINT(burst_secs)}));
	v.daily_used = Int(ScalarP(con,
	                           "SELECT count(*) FROM quackmail_send_log "
	                           "WHERE username = $1 AND sent_at > now() - (24 * INTERVAL 1 HOUR)",
	                           {Value(username)}));

	if (v.limit.burst_max > 0 && v.burst_used + count > v.limit.burst_max) {
		v.allowed = false;
		// Retry once the oldest message in the window ages out.
		int64_t oldest = Int(ScalarP(con,
		                             "SELECT CAST(date_diff('second', min(sent_at), now()) AS BIGINT) "
		                             "FROM quackmail_send_log WHERE username = $1 "
		                             "AND sent_at > now() - ($2 * INTERVAL 1 SECOND)",
		                             {Value(username), Value::BIGINT(burst_secs)}));
		v.retry_after = burst_secs - oldest;
		if (v.retry_after < 1) {
			v.retry_after = 1;
		}
		v.reason = "rate limit exceeded (" + std::to_string(v.burst_used) + "/" +
		           std::to_string(v.limit.burst_max) + " in " + std::to_string(burst_secs) + "s)";
		return v;
	}

	if (v.limit.daily_max > 0 && v.daily_used + count > v.limit.daily_max) {
		v.allowed = false;
		int64_t oldest = Int(ScalarP(con,
		                             "SELECT CAST(date_diff('second', min(sent_at), now()) AS BIGINT) "
		                             "FROM quackmail_send_log WHERE username = $1 "
		                             "AND sent_at > now() - (24 * INTERVAL 1 HOUR)",
		                             {Value(username)}));
		v.retry_after = 86400 - oldest;
		if (v.retry_after < 1) {
			v.retry_after = 1;
		}
		v.reason = "daily limit exceeded (" + std::to_string(v.daily_used) + "/" +
		           std::to_string(v.limit.daily_max) + " in 24h)";
		return v;
	}

	return v;
}

void RecordSend(Connection &con, const std::string &username, const std::string &rcpt, int64_t count) {
	for (int64_t i = 0; i < count; i++) {
		ExecP(con, "INSERT INTO quackmail_send_log (username, rcpt) VALUES ($1, $2)",
		      {Value(username), Value(rcpt)});
	}
}

bool SetRateLimit(Connection &con, const std::string &username, int64_t burst_max, int64_t burst_secs,
                  int64_t daily_max, std::string &err) {
	if (burst_secs <= 0) {
		err = "burst_secs must be positive";
		return false;
	}
	auto r = ExecP(con,
	               "INSERT INTO quackmail_rate_limits (username, burst_max, burst_secs, daily_max) "
	               "VALUES ($1, $2, $3, $4) "
	               "ON CONFLICT (username) DO UPDATE SET "
	               "  burst_max = excluded.burst_max, burst_secs = excluded.burst_secs, "
	               "  daily_max = excluded.daily_max, enabled = true",
	               {Value(username), Value::BIGINT(burst_max), Value::BIGINT(burst_secs),
	                Value::BIGINT(daily_max)});
	if (!r) {
		err = "could not set the rate limit";
		return false;
	}
	return true;
}

std::vector<RateLimit> ListRateLimits(Connection &con) {
	std::vector<RateLimit> out;
	auto r = ExecP(con,
	               "SELECT username, burst_max, burst_secs, daily_max, enabled "
	               "FROM quackmail_rate_limits ORDER BY username",
	               {});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
		RateLimit rl;
		rl.username = Str(mat.GetValue(0, i));
		rl.burst_max = Int(mat.GetValue(1, i), 100);
		rl.burst_secs = Int(mat.GetValue(2, i), 300);
		rl.daily_max = Int(mat.GetValue(3, i), 500);
		Value e = mat.GetValue(4, i);
		rl.enabled = e.IsNull() ? true : e.GetValue<bool>();
		out.push_back(std::move(rl));
	}
	return out;
}

void PruneSendLog(Connection &con) {
	// Nothing older than the daily window can affect any quota decision.
	con.Query("DELETE FROM quackmail_send_log WHERE sent_at < now() - (25 * INTERVAL 1 HOUR)");
}

// ---------------------------------------------------------------------------
// Enforcement toggles
// ---------------------------------------------------------------------------

Enforcement GetEnforcement(Connection &con) {
	Enforcement e;
	auto flag = [&](const std::string &name, bool dflt) {
		std::string v = citadel::GetConfig(con, name, dflt ? "1" : "0");
		return v == "1" || Lower(v) == "true" || Lower(v) == "yes" || Lower(v) == "on";
	};
	e.spf_reject = flag("qm_spf_reject", false);
	e.dkim_reject = flag("qm_dkim_reject", false);
	e.dmarc_enforce = flag("qm_dmarc_enforce", true);
	e.rbl_reject = flag("qm_rbl_reject", true);
	e.quarantine_room = citadel::GetConfig(con, "qm_quarantine_room", "Junk");
	if (e.quarantine_room.empty()) {
		e.quarantine_room = "Junk";
	}
	return e;
}

// ---------------------------------------------------------------------------
// Audit
// ---------------------------------------------------------------------------

void LogInbound(Connection &con, const InboundVerdict &v) {
	ExecP(con,
	      "INSERT INTO quackmail_inbound_log "
	      "(client_ip, helo, mail_from, rcpt, spf, dkim, dmarc, rbl, disposition, detail) "
	      "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
	      {Value(v.client_ip), Value(v.helo), Value(v.mail_from), Value(v.rcpt), Value(v.spf),
	       Value(v.dkim), Value(v.dmarc), Value(v.rbl), Value(v.disposition), Value(v.detail)});

	// Optionally mirror refusals into the Aide room. Off by default: on a live
	// MX this is the single noisiest thing the server could post. The audit log
	// above is always written and is the complete record.
	if (v.disposition == "accept" ||
	    citadel::GetConfig(con, "qm_aide_log_rejects", "0") != "1") {
		return;
	}
	std::string text = "Disposition: " + v.disposition + "\n";
	auto add = [&](const char *label, const std::string &value) {
		if (!value.empty()) {
			text += std::string(label) + ": " + value + "\n";
		}
	};
	add("From", v.mail_from);
	add("To", v.rcpt);
	add("Client", v.client_ip);
	add("HELO", v.helo);
	add("SPF", v.spf);
	add("DKIM", v.dkim);
	add("DMARC", v.dmarc);
	add("Blocklist", v.rbl);
	add("Detail", v.detail);
	citadel::PostAideMessage(con, "Inbound mail " + v.disposition + ": " + v.mail_from, text);
}

} // namespace policy
} // namespace quackmail
