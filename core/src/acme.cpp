// The ACME state machine: configuration, storage, and one pass of the order
// flow. The crypto it rests on is in acme_crypto.cpp.
#include "quackmail/acme.hpp"

#include "quackmail/http_client.hpp"
#include "quackmail/json.hpp"
#include "quackmail/util.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace quackmail {
namespace acme {

using duckdb::Connection;
using duckdb::MaterializedQueryResult;
using duckdb::QueryResult;
using duckdb::Value;

namespace {

// Let's Encrypt **staging**. Not production: the first run of a fresh install is
// the one most likely to have DNS or a firewall wrong, and a failed validation
// counts against a per-account-per-hostname-per-hour limit that is small.
const char *kStagingDirectory = "https://acme-staging-v02.api.letsencrypt.org/directory";

const char *kUserAgent = "QuackCit ACME";

int64_t Now() {
	return (int64_t)std::time(nullptr);
}

duckdb::unique_ptr<QueryResult> ExecP(Connection &con, const std::string &sql,
                                      duckdb::vector<Value> params) {
	auto stmt = con.Prepare(sql);
	if (!stmt || stmt->HasError()) {
		return nullptr;
	}
	auto r = stmt->Execute(params, false);
	if (!r || r->HasError()) {
		return nullptr;
	}
	return r;
}

std::string ConfigStr(Connection &con, const char *name, const std::string &dflt) {
	auto r = ExecP(con, "SELECT value FROM citadel_config WHERE name = $1", {Value(name)});
	if (!r) {
		return dflt;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() == 0 || mat.GetValue(0, 0).IsNull()) {
		return dflt;
	}
	std::string v = mat.GetValue(0, 0).ToString();
	return v.empty() ? dflt : v;
}

bool ConfigBool(Connection &con, const char *name, bool dflt) {
	std::string v = util::Lower(ConfigStr(con, name, dflt ? "1" : "0"));
	return v == "1" || v == "true" || v == "yes" || v == "on";
}

int ConfigInt(Connection &con, const char *name, int dflt) {
	std::string v = ConfigStr(con, name, "");
	if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos) {
		return dflt;
	}
	return (int)std::strtol(v.c_str(), nullptr, 10);
}

// A problem document (RFC 7807, as RFC 8555 §6.7 uses it), rendered short
// enough to store in a status column and read in a page.
std::string Problem(const std::string &body) {
	json::Value v;
	if (!json::Parse(body, v) || v.type != json::Value::Object) {
		return body.size() > 200 ? body.substr(0, 200) : body;
	}
	std::string type = v["type"].AsString();
	std::string detail = v["detail"].AsString();
	if (detail.empty()) {
		detail = type;
	}
	if (!type.empty() && type != detail) {
		return detail + " (" + type + ")";
	}
	return detail;
}

bool IsRateLimited(const std::string &body) {
	return body.find("urn:ietf:params:acme:error:rateLimited") != std::string::npos;
}

bool IsBadNonce(const std::string &body) {
	return body.find("urn:ietf:params:acme:error:badNonce") != std::string::npos;
}

// mkdir -p for the certificate directory. 0700: the key lives here.
bool EnsureDir(const std::string &path, std::string &err) {
	if (path.empty()) {
		err = "no certificate directory is configured";
		return false;
	}
	std::string built;
	size_t at = 0;
	while (at <= path.size()) {
		size_t slash = path.find('/', at);
		std::string part = path.substr(0, slash == std::string::npos ? path.size() : slash);
		if (!part.empty() && mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) {
			err = "cannot create " + part + ": " + std::string(strerror(errno));
			return false;
		}
		if (slash == std::string::npos) {
			break;
		}
		at = slash + 1;
	}
	(void)built;
	return true;
}

// Write via a temporary file and rename, so a reader never sees a half-written
// certificate — the same discipline deploy/quackcit.sh tls_generate() follows.
bool WriteFile(const std::string &path, const std::string &data, mode_t mode, std::string &err) {
	const std::string tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f) {
		err = "cannot write " + tmp + ": " + std::string(strerror(errno));
		return false;
	}
	bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	if (fclose(f) != 0) {
		ok = false;
	}
	if (ok) {
		ok = chmod(tmp.c_str(), mode) == 0;
	}
	if (!ok) {
		remove(tmp.c_str());
		err = "cannot write " + tmp + ": " + std::string(strerror(errno));
		return false;
	}
	if (rename(tmp.c_str(), path.c_str()) != 0) {
		remove(tmp.c_str());
		err = "cannot rename onto " + path + ": " + std::string(strerror(errno));
		return false;
	}
	return true;
}

std::vector<std::string> SplitDomains(const std::string &csv) {
	std::vector<std::string> out;
	std::string cur;
	for (char c : csv) {
		if (c == ',' || c == ' ' || c == '\t') {
			if (!cur.empty()) {
				out.push_back(cur);
				cur.clear();
			}
			continue;
		}
		cur += (char)tolower((unsigned char)c);
	}
	if (!cur.empty()) {
		out.push_back(cur);
	}
	return out;
}

// ---- the transport -------------------------------------------------------

// One ACME conversation. Holds the directory, the current nonce and the account
// binding, so the order flow below reads as the RFC does.
class Session {
public:
	Session(Connection &con, const Config &config) : con_(con), config_(config) {
	}

	const std::string &error() const {
		return error_;
	}

	bool LoadDirectory() {
		httpc::Options opts = BaseOptions();
		httpc::Response res = httpc::Request(config_.directory_url, opts);
		if (!Ok(res, "fetching the ACME directory")) {
			return false;
		}
		json::Value dir;
		if (!json::Parse(res.body, dir)) {
			error_ = "the ACME directory is not JSON";
			return false;
		}
		new_nonce_ = dir["newNonce"].AsString();
		new_account_ = dir["newAccount"].AsString();
		new_order_ = dir["newOrder"].AsString();
		revoke_ = dir["revokeCert"].AsString();
		tos_ = dir["meta"]["termsOfService"].AsString();
		if (new_nonce_.empty() || new_account_.empty() || new_order_.empty()) {
			error_ = "the ACME directory is missing newNonce, newAccount or newOrder";
			return false;
		}
		return true;
	}

	const std::string &terms_of_service() const {
		return tos_;
	}
	const std::string &account_url() const {
		return account_url_;
	}

	// Sign in, creating the account if this directory has not seen the key.
	bool EnsureAccount() {
		auto r = ExecP(con_,
		               "SELECT key_pem, account_url FROM quackmail_acme_accounts "
		               " WHERE directory_url = $1",
		               {Value(config_.directory_url)});
		if (r) {
			auto &mat = r->Cast<MaterializedQueryResult>();
			if (mat.RowCount() > 0) {
				key_pem_ = mat.GetValue(0, 0).IsNull() ? "" : mat.GetValue(0, 0).ToString();
				account_url_ = mat.GetValue(1, 0).IsNull() ? "" : mat.GetValue(1, 0).ToString();
			}
		}
		if (key_pem_.empty()) {
			std::string err;
			if (!GenerateAccountKey(config_.key_bits, key_pem_, err)) {
				error_ = err;
				return false;
			}
			account_url_.clear();
		}
		std::string err;
		if (!JwkThumbprint(key_pem_, thumbprint_, err)) {
			error_ = err;
			return false;
		}

		json::Value payload = json::Value::MakeObject();
		payload.Set("termsOfServiceAgreed", config_.tos_agreed);
		if (!config_.contact.empty()) {
			json::Value contacts = json::Value::MakeArray();
			contacts.Push(json::Value::MakeString("mailto:" + config_.contact));
			payload.Set("contact", contacts);
		}
		// newAccount is the one request signed with the JWK rather than the
		// account URL, because the account URL is what it returns.
		httpc::Response res;
		std::string body;
		if (!Post(new_account_, json::Serialize(payload), true, body, res)) {
			return false;
		}
		if (res.status != 200 && res.status != 201) {
			error_ = "the ACME server refused the account: " + Problem(body);
			return false;
		}
		std::string location = res.Header("location");
		if (!location.empty()) {
			account_url_ = location;
		}
		if (account_url_.empty()) {
			error_ = "the ACME server returned no account URL";
			return false;
		}
		ExecP(con_,
		      "INSERT OR REPLACE INTO quackmail_acme_accounts "
		      "  (directory_url, account_url, key_pem, contact, tos_agreed, created) "
		      "VALUES ($1, $2, $3, $4, $5, $6)",
		      {Value(config_.directory_url), Value(account_url_), Value(key_pem_),
		       Value(config_.contact), Value::BOOLEAN(config_.tos_agreed), Value::BIGINT(Now())});
		return true;
	}

	const std::string &thumbprint() const {
		return thumbprint_;
	}
	const std::string &new_order_url() const {
		return new_order_;
	}
	const std::string &revoke_url() const {
		return revoke_;
	}
	const std::string &key_pem() const {
		return key_pem_;
	}

	// A signed POST. `use_jwk` embeds the key itself rather than the account
	// URL, which only newAccount and a revoke-by-key do.
	bool Post(const std::string &url, const std::string &payload, bool use_jwk, std::string &body,
	          httpc::Response &res) {
		for (int attempt = 0; attempt < 2; attempt++) {
			if (nonce_.empty() && !FetchNonce()) {
				return false;
			}
			json::Value prot = json::Value::MakeObject();
			prot.Set("alg", "RS256");
			prot.Set("nonce", nonce_);
			prot.Set("url", url);
			if (use_jwk) {
				std::string jwk;
				std::string err;
				if (!JwkPublic(key_pem_, jwk, err)) {
					error_ = err;
					return false;
				}
				json::Value parsed;
				json::Parse(jwk, parsed);
				prot.Set("jwk", parsed);
			} else {
				prot.Set("kid", account_url_);
			}
			std::string jws;
			std::string err;
			if (!JwsSign(key_pem_, json::Serialize(prot), payload, jws, err)) {
				error_ = err;
				return false;
			}
			nonce_.clear(); // a nonce is single-use whatever happens next

			httpc::Options opts = BaseOptions();
			opts.method = "POST";
			opts.body = jws;
			opts.content_type = "application/jose+json";
			res = httpc::Request(url, opts);
			if (!res.error.empty()) {
				error_ = res.error;
				return false;
			}
			body = res.body;
			std::string fresh = res.Header("replay-nonce");
			if (!fresh.empty()) {
				nonce_ = fresh;
			}
			// RFC 8555 §6.5: a badNonce is retried **once**, with the nonce the
			// error itself carried. More than once is a loop against a server
			// that is telling us something else is wrong.
			if (res.status >= 400 && IsBadNonce(body) && attempt == 0) {
				continue;
			}
			return true;
		}
		return true;
	}

private:
	httpc::Options BaseOptions() const {
		httpc::Options opts;
		opts.timeout_ms = config_.timeout_ms;
		opts.user_agent = kUserAgent;
		opts.accept = "application/json, */*";
		// **The whole point of the verifying client context.** An ACME client
		// that does not authenticate the CA is not an ACME client: everything
		// else it does rests on having talked to the right server.
		opts.verify_peer = true;
		opts.ca_bundle = config_.ca_bundle;
		return opts;
	}

	bool Ok(const httpc::Response &res, const char *what) {
		if (!res.error.empty()) {
			error_ = std::string(what) + " failed: " + res.error;
			return false;
		}
		if (res.status < 200 || res.status >= 300) {
			error_ = std::string(what) + " returned " + std::to_string(res.status) + ": " +
			         Problem(res.body);
			return false;
		}
		return true;
	}

	bool FetchNonce() {
		httpc::Options opts = BaseOptions();
		opts.method = "HEAD";
		httpc::Response res = httpc::Request(new_nonce_, opts);
		if (!res.error.empty()) {
			error_ = "fetching a nonce failed: " + res.error;
			return false;
		}
		nonce_ = res.Header("replay-nonce");
		if (nonce_.empty()) {
			error_ = "the ACME server returned no Replay-Nonce";
			return false;
		}
		return true;
	}

	Connection &con_;
	const Config &config_;
	std::string new_nonce_;
	std::string new_account_;
	std::string new_order_;
	std::string revoke_;
	std::string tos_;
	std::string nonce_;
	std::string key_pem_;
	std::string account_url_;
	std::string thumbprint_;
	std::string error_;
};

// ---- one certificate's order ---------------------------------------------

struct OrderRow {
	std::string name;
	std::string domains;
	int64_t attempts = 0;
};

void Defer(Connection &con, const std::string &name, int64_t attempts, const std::string &why,
           bool rate_limited) {
	// Exponential backoff to a one-day cap. Let's Encrypt counts failed
	// validations per account per hostname per hour, so a worker that retried a
	// broken order every tick would get the whole account throttled — including
	// for the names that were fine.
	int64_t delay = 900; // 15 minutes
	for (int64_t i = 0; i < attempts && delay < 86400; i++) {
		delay *= 2;
	}
	if (delay > 86400) {
		delay = 86400;
	}
	if (rate_limited) {
		delay = 86400;
	}
	ExecP(con,
	      "UPDATE quackmail_acme_orders SET attempts = attempts + 1, next_attempt = $2, "
	      "       status = 'deferred', error = $3, updated = $4 WHERE name = $1",
	      {Value(name), Value::BIGINT(Now() + delay), Value(why), Value::BIGINT(Now())});
}

bool RunOrder(Connection &con, Session &s, const Config &config, const OrderRow &row, Result &out) {
	out.name = row.name;
	const std::vector<std::string> domains = SplitDomains(row.domains);
	if (domains.empty()) {
		out.status = "error";
		out.note = "no domains configured";
		return false;
	}

	// newOrder
	json::Value payload = json::Value::MakeObject();
	json::Value ids = json::Value::MakeArray();
	for (const std::string &d : domains) {
		json::Value id = json::Value::MakeObject();
		id.Set("type", "dns");
		id.Set("value", d);
		ids.Push(id);
	}
	payload.Set("identifiers", ids);

	std::string body;
	httpc::Response res;
	if (!s.Post(s.new_order_url(), json::Serialize(payload), false, body, res)) {
		out.status = "error";
		out.note = s.error();
		Defer(con, row.name, row.attempts, out.note, false);
		return false;
	}
	if (res.status != 201 && res.status != 200) {
		out.status = "error";
		out.note = "newOrder returned " + std::to_string(res.status) + ": " + Problem(body);
		Defer(con, row.name, row.attempts, out.note, IsRateLimited(body));
		return false;
	}
	const std::string order_url = res.Header("location");
	json::Value order;
	json::Parse(body, order);
	const std::string finalize_url = order["finalize"].AsString();
	if (finalize_url.empty()) {
		out.status = "error";
		out.note = "the order has no finalize URL";
		Defer(con, row.name, row.attempts, out.note, false);
		return false;
	}
	ExecP(con,
	      "UPDATE quackmail_acme_orders SET order_url = $2, finalize_url = $3, "
	      "       status = 'pending', updated = $4 WHERE name = $1",
	      {Value(row.name), Value(order_url), Value(finalize_url), Value::BIGINT(Now())});

	// Each authorization: publish the http-01 token, then tell the CA to look.
	const json::Value &authzs = order["authorizations"];
	for (size_t i = 0; i < authzs.Size(); i++) {
		const std::string authz_url = authzs.At(i).AsString();
		if (!s.Post(authz_url, "", false, body, res) || res.status >= 300) {
			out.status = "error";
			out.note = "reading an authorization failed: " +
			           (s.error().empty() ? Problem(body) : s.error());
			Defer(con, row.name, row.attempts, out.note, IsRateLimited(body));
			return false;
		}
		json::Value authz;
		json::Parse(body, authz);
		if (authz["status"].AsString() == "valid") {
			continue; // already answered, and still good
		}
		std::string chal_url;
		std::string token;
		const json::Value &challenges = authz["challenges"];
		for (size_t c = 0; c < challenges.Size(); c++) {
			if (challenges.At(c)["type"].AsString() == "http-01") {
				chal_url = challenges.At(c)["url"].AsString();
				token = challenges.At(c)["token"].AsString();
				break;
			}
		}
		if (chal_url.empty() || token.empty()) {
			out.status = "error";
			out.note = "the ACME server offered no http-01 challenge for " +
			           authz["identifier"]["value"].AsString();
			Defer(con, row.name, row.attempts, out.note, false);
			return false;
		}

		// Publish it where the HTTP listener will find it. The row is the whole
		// interface between this worker and that listener.
		ExecP(con,
		      "INSERT OR REPLACE INTO quackmail_acme_challenges (token, key_auth, name, expires) "
		      "VALUES ($1, $2, $3, $4)",
		      {Value(token), Value(KeyAuthorization(token, s.thumbprint())), Value(row.name),
		       Value::BIGINT(Now() + 3600)});

		if (!s.Post(chal_url, "{}", false, body, res) || res.status >= 300) {
			out.status = "error";
			out.note = "answering the challenge failed: " +
			           (s.error().empty() ? Problem(body) : s.error());
			Defer(con, row.name, row.attempts, out.note, IsRateLimited(body));
			return false;
		}

		// Poll. Bounded: a CA that has not made up its mind in a minute is a CA
		// to come back to on the next tick, not one to hold a worker thread for.
		std::string state = "pending";
		for (int poll = 0; poll < 20; poll++) {
			struct timespec ts { 0, 300 * 1000 * 1000 };
			nanosleep(&ts, nullptr);
			if (!s.Post(authz_url, "", false, body, res)) {
				break;
			}
			json::Value again;
			json::Parse(body, again);
			state = again["status"].AsString();
			if (state != "pending" && state != "processing") {
				break;
			}
		}
		ExecP(con, "DELETE FROM quackmail_acme_challenges WHERE token = $1", {Value(token)});
		if (state != "valid") {
			out.status = "error";
			out.note = "the challenge for " + authz["identifier"]["value"].AsString() +
			           " ended as '" + state + "'";
			Defer(con, row.name, row.attempts, out.note, IsRateLimited(body));
			return false;
		}
	}

	// finalize, with a **fresh key** for this certificate.
	std::string cert_key;
	std::string csr;
	std::string err;
	if (!MakeCsr(domains, cert_key, csr, err)) {
		out.status = "error";
		out.note = err;
		Defer(con, row.name, row.attempts, err, false);
		return false;
	}
	json::Value fin = json::Value::MakeObject();
	fin.Set("csr", csr);
	if (!s.Post(finalize_url, json::Serialize(fin), false, body, res) || res.status >= 300) {
		out.status = "error";
		out.note = "finalize failed: " + (s.error().empty() ? Problem(body) : s.error());
		Defer(con, row.name, row.attempts, out.note, IsRateLimited(body));
		return false;
	}

	std::string cert_url;
	for (int poll = 0; poll < 20; poll++) {
		json::Value o;
		json::Parse(body, o);
		const std::string state = o["status"].AsString();
		cert_url = o["certificate"].AsString();
		if (state == "valid" && !cert_url.empty()) {
			break;
		}
		if (state == "invalid") {
			out.status = "error";
			out.note = "the order was rejected: " + Problem(body);
			Defer(con, row.name, row.attempts, out.note, false);
			return false;
		}
		struct timespec ts { 0, 300 * 1000 * 1000 };
		nanosleep(&ts, nullptr);
		if (order_url.empty() || !s.Post(order_url, "", false, body, res)) {
			break;
		}
	}
	if (cert_url.empty()) {
		out.status = "error";
		out.note = "the order never produced a certificate";
		Defer(con, row.name, row.attempts, out.note, false);
		return false;
	}

	if (!s.Post(cert_url, "", false, body, res) || res.status >= 300) {
		out.status = "error";
		out.note = "downloading the certificate failed: " +
		           (s.error().empty() ? Problem(body) : s.error());
		Defer(con, row.name, row.attempts, out.note, false);
		return false;
	}
	const std::string chain = body;
	int64_t not_after = 0;
	if (!CertNotAfter(chain, not_after, err)) {
		out.status = "error";
		out.note = "the issued certificate is unreadable: " + err;
		Defer(con, row.name, row.attempts, out.note, false);
		return false;
	}

	const std::string cert_path = config.cert_dir + "/" + row.name + ".pem";
	const std::string key_path = config.cert_dir + "/" + row.name + ".key";
	if (!EnsureDir(config.cert_dir, err) || !WriteFile(cert_path, chain, 0644, err) ||
	    !WriteFile(key_path, cert_key, 0600, err)) {
		out.status = "error";
		out.note = err;
		Defer(con, row.name, row.attempts, err, false);
		return false;
	}

	ExecP(con,
	      "INSERT OR REPLACE INTO quackmail_acme_certs "
	      "  (name, domains, cert_pem, key_pem, not_after, issued, cert_path, key_path) "
	      "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)",
	      {Value(row.name), Value(row.domains), Value(chain), Value(cert_key),
	       Value::BIGINT(not_after), Value::BIGINT(Now()), Value(cert_path), Value(key_path)});
	ExecP(con,
	      "UPDATE quackmail_acme_orders SET status = 'issued', attempts = 0, next_attempt = 0, "
	      "       error = '', updated = $2 WHERE name = $1",
	      {Value(row.name), Value::BIGINT(Now())});

	out.status = "issued";
	out.not_after = not_after;
	int reloaded = 0;
	for (const auto &r : ReloadListeners(con)) {
		if (r.second == "reloaded") {
			reloaded++;
		}
	}
	out.note = "issued until " + std::to_string(not_after) + "; " + std::to_string(reloaded) +
	           " listener(s) reloaded";
	return true;
}

} // namespace

Config LoadConfig(Connection &con) {
	Config c;
	c.enabled = ConfigBool(con, "qm_acme_enabled", false);
	c.directory_url = ConfigStr(con, "qm_acme_directory", kStagingDirectory);
	c.contact = ConfigStr(con, "qm_acme_contact", "");
	c.ca_bundle = ConfigStr(con, "qm_acme_ca_bundle", "");
	c.cert_dir = ConfigStr(con, "qm_acme_cert_dir", "");
	c.renew_days = ConfigInt(con, "qm_acme_renew_days", 30);
	c.tos_agreed = ConfigBool(con, "qm_acme_tos_agreed", false);
	c.key_bits = ConfigInt(con, "qm_acme_key_bits", 2048);
	c.timeout_ms = ConfigInt(con, "qm_acme_timeout_ms", 30000);
	if (c.renew_days < 1 || c.renew_days > 89) {
		c.renew_days = 30;
	}
	return c;
}

void EnsureSchema(Connection &con) {
	con.Query("CREATE TABLE IF NOT EXISTS quackmail_acme_accounts ("
	          "  directory_url VARCHAR PRIMARY KEY, account_url VARCHAR, key_pem VARCHAR,"
	          "  contact VARCHAR, tos_agreed BOOLEAN, created BIGINT)");
	con.Query("CREATE TABLE IF NOT EXISTS quackmail_acme_certs ("
	          "  name VARCHAR PRIMARY KEY, domains VARCHAR, cert_pem VARCHAR, key_pem VARCHAR,"
	          "  not_after BIGINT, issued BIGINT, cert_path VARCHAR, key_path VARCHAR)");
	con.Query("CREATE TABLE IF NOT EXISTS quackmail_acme_orders ("
	          "  name VARCHAR PRIMARY KEY, domains VARCHAR, order_url VARCHAR,"
	          "  finalize_url VARCHAR, status VARCHAR, attempts BIGINT DEFAULT 0,"
	          "  next_attempt BIGINT DEFAULT 0, error VARCHAR, created BIGINT, updated BIGINT)");
	// The one table the HTTP listener reads. It lives here rather than in the
	// spool extension because the responder has to work whether or not the
	// worker's extension was ever loaded.
	con.Query("CREATE TABLE IF NOT EXISTS quackmail_acme_challenges ("
	          "  token VARCHAR PRIMARY KEY, key_auth VARCHAR, name VARCHAR, expires BIGINT)");
}

std::vector<std::pair<std::string, std::string>> ReloadListeners(Connection &con) {
	std::vector<std::pair<std::string, std::string>> out;
	// The DuckDB catalog is already an authoritative register of what is loaded
	// here. A table of listeners maintained by hand would be a second one, and a
	// stale row in it would be a silent no-op.
	// NOT LIKE 'qm_acme%' matters: the spool extension exposes this sweep as a
	// table function of its own, and anything named `%_tls_reload` that is not a
	// listener would be called by the sweep — including a sweep, which recurses
	// until the stack runs out. The wrapper is named to avoid the pattern *and*
	// excluded here, because a future one would otherwise re-find the same trap.
	auto r = con.Query("SELECT DISTINCT function_name FROM duckdb_functions() "
	                   " WHERE function_name LIKE '%\\_tls\\_reload' ESCAPE '\\' "
	                   "   AND function_name NOT LIKE 'qm\\_acme\\_%' ESCAPE '\\' "
	                   " ORDER BY function_name");
	if (!r || r->HasError()) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
		const std::string fn = mat.GetValue(0, i).ToString();
		// Names come from the catalog, not from input, but the interpolation is
		// unavoidable here (a function name cannot be a bind parameter), so it
		// is checked rather than trusted.
		if (fn.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
			continue;
		}
		auto res = con.Query("SELECT note FROM " + fn + "()");
		if (!res || res->HasError()) {
			out.emplace_back(fn, "error");
			continue;
		}
		auto &m = res->Cast<MaterializedQueryResult>();
		out.emplace_back(fn, m.RowCount() ? m.GetValue(0, 0).ToString() : "no answer");
	}
	return out;
}

bool Order(Connection &con, const std::string &name, const std::string &domains,
           std::string &err) {
	EnsureSchema(con);
	if (name.empty() || name.find_first_of("/\\. \t\r\n") != std::string::npos) {
		// The name becomes a file name under the certificate directory.
		err = "'" + name + "' is not a usable certificate name";
		return false;
	}
	if (SplitDomains(domains).empty()) {
		err = "a certificate needs at least one domain";
		return false;
	}
	// Queued, not run: an order takes tens of seconds against a real CA, which
	// is not a web request's to wait for. The worker is the consumer.
	auto r = ExecP(con,
	               "INSERT OR REPLACE INTO quackmail_acme_orders "
	               "  (name, domains, status, attempts, next_attempt, error, created, updated) "
	               "VALUES ($1, $2, 'queued', 0, 0, '', $3, $3)",
	               {Value(name), Value(domains), Value::BIGINT(Now())});
	if (!r) {
		err = "the order could not be queued";
		return false;
	}
	return true;
}

bool Forget(Connection &con, const std::string &name, std::string &err) {
	EnsureSchema(con);
	ExecP(con, "DELETE FROM quackmail_acme_orders WHERE name = $1", {Value(name)});
	ExecP(con, "DELETE FROM quackmail_acme_certs WHERE name = $1", {Value(name)});
	(void)err;
	return true;
}

bool Revoke(Connection &con, const std::string &name, int reason, std::string &err) {
	EnsureSchema(con);
	Config config = LoadConfig(con);
	auto r = ExecP(con, "SELECT cert_pem FROM quackmail_acme_certs WHERE name = $1", {Value(name)});
	if (!r) {
		err = "no certificate by that name";
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() == 0 || mat.GetValue(0, 0).IsNull()) {
		err = "no certificate by that name";
		return false;
	}
	const std::string pem = mat.GetValue(0, 0).ToString();

	// The DER of the leaf, base64url — RFC 8555 §7.6.
	const std::string begin = "-----BEGIN CERTIFICATE-----";
	const std::string end = "-----END CERTIFICATE-----";
	size_t b = pem.find(begin);
	size_t e = pem.find(end, b == std::string::npos ? 0 : b);
	if (b == std::string::npos || e == std::string::npos) {
		err = "the stored certificate is not PEM";
		return false;
	}
	std::string b64 = pem.substr(b + begin.size(), e - b - begin.size());
	std::string der;
	if (!util::Base64Decode(b64, der)) {
		err = "the stored certificate could not be decoded";
		return false;
	}

	Session s(con, config);
	if (!s.LoadDirectory() || !s.EnsureAccount()) {
		err = s.error();
		return false;
	}
	if (s.revoke_url().empty()) {
		err = "this ACME server does not advertise revokeCert";
		return false;
	}
	json::Value payload = json::Value::MakeObject();
	payload.Set("certificate", util::Base64UrlEncode(der));
	payload.Set("reason", (int64_t)reason);
	std::string body;
	httpc::Response res;
	if (!s.Post(s.revoke_url(), json::Serialize(payload), false, body, res)) {
		err = s.error();
		return false;
	}
	if (res.status >= 300) {
		err = "revocation returned " + std::to_string(res.status) + ": " + Problem(body);
		return false;
	}
	Forget(con, name, err);
	return true;
}

bool RunOnce(Connection &con, const std::string &only_name, bool force,
             std::vector<Result> &out) {
	EnsureSchema(con);
	// Expired challenges are pruned every pass: they are the one row an outside
	// party can ask about, and there is no reason for one to outlive its order.
	ExecP(con, "DELETE FROM quackmail_acme_challenges WHERE expires < $1", {Value::BIGINT(Now())});

	Config config = LoadConfig(con);
	if (!config.enabled && !force) {
		Result r;
		r.name = "";
		r.status = "disabled";
		r.note = "qm_acme_enabled is off";
		out.push_back(r);
		return true;
	}
	if (config.cert_dir.empty()) {
		Result r;
		r.status = "error";
		r.note = "qm_acme_cert_dir is not set";
		out.push_back(r);
		return false;
	}

	// What is due: a queued order, a deferred one whose time has come, or a
	// certificate inside its renewal window.
	std::string sql =
	    "SELECT o.name, o.domains, o.attempts FROM quackmail_acme_orders o "
	    "  LEFT JOIN quackmail_acme_certs c ON c.name = o.name "
	    " WHERE ($1 = '' OR o.name = $1) "
	    "   AND ($2 OR ("
	    "         o.next_attempt <= $3 AND ("
	    "           c.not_after IS NULL OR c.not_after - $3 < $4)))";
	auto r = ExecP(con, sql,
	               {Value(only_name), Value::BOOLEAN(force), Value::BIGINT(Now()),
	                Value::BIGINT((int64_t)config.renew_days * 86400)});
	if (!r) {
		Result res;
		res.status = "error";
		res.note = "could not read the order table";
		out.push_back(res);
		return false;
	}
	std::vector<OrderRow> due;
	{
		auto &mat = r->Cast<MaterializedQueryResult>();
		for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
			OrderRow row;
			row.name = mat.GetValue(0, i).ToString();
			row.domains = mat.GetValue(1, i).IsNull() ? "" : mat.GetValue(1, i).ToString();
			row.attempts = mat.GetValue(2, i).IsNull() ? 0 : mat.GetValue(2, i).GetValue<int64_t>();
			due.push_back(row);
		}
	}
	if (due.empty()) {
		Result res;
		res.status = "idle";
		res.note = "nothing is due";
		out.push_back(res);
		return true;
	}

	Session s(con, config);
	if (!s.LoadDirectory()) {
		Result res;
		res.status = "error";
		res.note = s.error();
		out.push_back(res);
		return false;
	}
	if (!s.EnsureAccount()) {
		Result res;
		res.status = "error";
		res.note = s.error();
		out.push_back(res);
		return false;
	}

	bool all_ok = true;
	for (const OrderRow &row : due) {
		Result res;
		if (!RunOrder(con, s, config, row, res)) {
			all_ok = false;
		}
		out.push_back(res);
	}
	return all_ok;
}

} // namespace acme
} // namespace quackmail
