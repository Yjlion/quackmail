#include "quackmail/websession.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/util.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <ctime>

namespace quackmail {
namespace web {

using duckdb::Connection;
using duckdb::idx_t;
using duckdb::MaterializedQueryResult;
using duckdb::QueryResult;
using duckdb::Value;

namespace {

// How long a session survives without use, and how long it may live at all.
constexpr int kIdleSeconds = 12 * 3600;
constexpr int kMaxLifetimeSeconds = 7 * 24 * 3600;
// The idle window is only rewritten when it would move by more than this, so a
// busy session does not cost a write per request.
constexpr int kRefreshSlackSeconds = 60;

// Login throttling: this many failures from one address within the window
// locks it out for the rest of that window.
constexpr int kMaxLoginFailures = 10;
constexpr int kLoginWindowSeconds = 300;

duckdb::unique_ptr<QueryResult> ExecP(Connection &con, const std::string &sql, duckdb::vector<Value> params) {
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

int64_t AsBigint(const Value &v, int64_t dflt = 0) {
	return v.IsNull() ? dflt : v.GetValue<int64_t>();
}

std::string AsString(const Value &v) {
	return v.IsNull() ? std::string() : v.ToString();
}

bool AsBool(const Value &v) {
	return !v.IsNull() && v.GetValue<bool>();
}

int64_t NowEpoch() {
	return (int64_t)std::time(nullptr);
}

} // namespace

void EnsureSchema(Connection &con) {
	// Timestamps are stored as unix seconds rather than TIMESTAMP so that expiry
	// arithmetic is plain integer comparison, with no dependence on the session's
	// time zone setting.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_web_sessions (
			token_hash VARCHAR PRIMARY KEY,
			username   VARCHAR NOT NULL,
			created_at BIGINT DEFAULT 0,
			last_seen  BIGINT DEFAULT 0,
			expires_at BIGINT DEFAULT 0,
			peer_ip    VARCHAR DEFAULT '',
			user_agent VARCHAR DEFAULT '',
			tls        BOOLEAN DEFAULT false,
			revoked    BOOLEAN DEFAULT false
		)
	)");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_web_login_fails (
			peer_ip  VARCHAR,
			username VARCHAR,
			at       BIGINT DEFAULT 0
		)
	)");
}

namespace {

// The server-side secret the CSRF tokens are derived from. Generated on first
// use and kept in citadel_config, so it survives restarts (a restart that
// invalidated every open form would be its own small denial of service).
std::string WebSecret(Connection &con) {
	std::string secret = citadel::GetConfig(con, "qm_web_secret", "");
	if (!secret.empty()) {
		return secret;
	}
	secret = util::RandomHex(32);
	if (secret.empty()) {
		return std::string(); // RNG failure; callers fail closed
	}
	ExecP(con,
	      "INSERT INTO citadel_config (name, value) VALUES ('qm_web_secret', $1) "
	      "ON CONFLICT (name) DO NOTHING",
	      {Value(secret)});
	// Re-read: another session may have won the race and stored a different one.
	return citadel::GetConfig(con, "qm_web_secret", secret);
}

} // namespace

bool CreateSession(Connection &con, const std::string &username, bool tls, const std::string &peer_ip,
                   const std::string &user_agent, std::string &out_token, std::string &err) {
	// 32 bytes. RandomBase64Url returns "" when OpenSSL's RNG failed, and a
	// predictable session token is worse than no login at all.
	out_token = util::RandomBase64Url(32);
	if (out_token.empty()) {
		err = "the random number generator is unavailable";
		return false;
	}
	int64_t now = NowEpoch();
	// The user agent is echoed nowhere, but it is long enough to be worth
	// bounding before it goes in a row.
	std::string ua = user_agent.size() > 255 ? user_agent.substr(0, 255) : user_agent;

	auto r = ExecP(con,
	               "INSERT INTO quackmail_web_sessions "
	               "(token_hash, username, created_at, last_seen, expires_at, peer_ip, "
	               " user_agent, tls, revoked) "
	               "VALUES ($1, $2, $3, $3, $4, $5, $6, $7, false)",
	               {Value(util::Sha256Hex(out_token)), Value(username), Value::BIGINT(now),
	                Value::BIGINT(now + kIdleSeconds), Value(peer_ip), Value(ua), Value::BOOLEAN(tls)});
	if (!r) {
		err = "could not store the session";
		out_token.clear();
		return false;
	}
	return true;
}

bool LookupSession(Connection &con, const std::string &token, bool tls, Session &out) {
	if (token.empty()) {
		return false;
	}
	// The lookup is a full-hash equality probe on the primary key, so unlike a
	// stored-plaintext prefix scan there is no timing channel to walk.
	std::string hash = util::Sha256Hex(token);
	auto r = ExecP(con,
	               "SELECT username, created_at, expires_at, tls, revoked FROM quackmail_web_sessions "
	               "WHERE token_hash = $1",
	               {Value(hash)});
	if (!r) {
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return false;
	}
	std::string username = AsString(mat.GetValue(0, 0));
	int64_t created_at = AsBigint(mat.GetValue(1, 0));
	int64_t expires_at = AsBigint(mat.GetValue(2, 0));
	bool row_tls = AsBool(mat.GetValue(3, 0));
	bool revoked = AsBool(mat.GetValue(4, 0));

	int64_t now = NowEpoch();
	if (revoked || expires_at <= now || created_at + kMaxLifetimeSeconds <= now) {
		return false;
	}

	// Transport pinning, both directions. A session minted over TLS arriving in
	// the clear should have been impossible (the cookie is Secure), so treat it
	// as compromise and kill the row. A session minted in the clear presented
	// over TLS is refused so that a network attacker on the plaintext port
	// cannot launder a stolen cookie into the secure area.
	if (row_tls != tls) {
		if (row_tls) {
			ExecP(con, "UPDATE quackmail_web_sessions SET revoked = true WHERE token_hash = $1",
			      {Value(hash)});
		}
		return false;
	}

	int64_t new_expiry = now + kIdleSeconds;
	int64_t cap = created_at + kMaxLifetimeSeconds;
	if (new_expiry > cap) {
		new_expiry = cap;
	}
	if (new_expiry > expires_at + kRefreshSlackSeconds) {
		ExecP(con, "UPDATE quackmail_web_sessions SET last_seen = $2, expires_at = $3 WHERE token_hash = $1",
		      {Value(hash), Value::BIGINT(now), Value::BIGINT(new_expiry)});
	} else {
		ExecP(con, "UPDATE quackmail_web_sessions SET last_seen = $2 WHERE token_hash = $1",
		      {Value(hash), Value::BIGINT(now)});
	}

	out.token_hash = hash;
	out.username = username;
	out.csrf = CsrfToken(con, hash);
	out.tls = row_tls;
	out.axlevel = citadel::GetAxLevel(con, username);
	return true;
}

std::string CsrfToken(Connection &con, const std::string &token_hash) {
	if (token_hash.empty()) {
		return std::string();
	}
	std::string secret = WebSecret(con);
	if (secret.empty()) {
		return std::string(); // no secret, no token: CheckCsrf then fails closed
	}
	return util::Sha256Hex(secret + ":csrf:" + token_hash);
}

bool CheckCsrf(Connection &con, const std::string &token_hash, const std::string &submitted) {
	std::string expected = CsrfToken(con, token_hash);
	if (expected.empty() || submitted.empty()) {
		return false;
	}
	return util::SecureEquals(expected, submitted);
}

namespace {

constexpr int64_t kAnonCsrfBucketSeconds = 3600;

std::string AnonCsrfForBucket(Connection &con, const std::string &peer_ip, int64_t bucket) {
	std::string secret = WebSecret(con);
	if (secret.empty()) {
		return std::string();
	}
	return util::Sha256Hex(secret + ":login:" + peer_ip + ":" + std::to_string(bucket));
}

} // namespace

std::string AnonCsrfToken(Connection &con, const std::string &peer_ip) {
	return AnonCsrfForBucket(con, peer_ip, NowEpoch() / kAnonCsrfBucketSeconds);
}

bool CheckAnonCsrf(Connection &con, const std::string &peer_ip, const std::string &submitted) {
	if (submitted.empty()) {
		return false;
	}
	int64_t bucket = NowEpoch() / kAnonCsrfBucketSeconds;
	// Accept the previous bucket too, so a login form that was rendered just
	// before a bucket boundary still submits.
	for (int64_t b = bucket; b >= bucket - 1; b--) {
		std::string expected = AnonCsrfForBucket(con, peer_ip, b);
		if (!expected.empty() && util::SecureEquals(expected, submitted)) {
			return true;
		}
	}
	return false;
}

void RevokeSession(Connection &con, const std::string &token) {
	if (token.empty()) {
		return;
	}
	ExecP(con, "DELETE FROM quackmail_web_sessions WHERE token_hash = $1",
	      {Value(util::Sha256Hex(token))});
}

void RevokeByHash(Connection &con, const std::string &token_hash, const std::string &username) {
	if (token_hash.empty()) {
		return;
	}
	// The username qualifier is what keeps one user from revoking another's
	// session by pasting in a hash; an admin passes "" to skip it.
	if (username.empty()) {
		ExecP(con, "DELETE FROM quackmail_web_sessions WHERE token_hash = $1", {Value(token_hash)});
	} else {
		ExecP(con, "DELETE FROM quackmail_web_sessions WHERE token_hash = $1 AND username = $2",
		      {Value(token_hash), Value(username)});
	}
}

void RevokeAllForUser(Connection &con, const std::string &username) {
	ExecP(con, "DELETE FROM quackmail_web_sessions WHERE username = $1", {Value(username)});
}

void PruneSessions(Connection &con) {
	ExecP(con, "DELETE FROM quackmail_web_sessions WHERE revoked OR expires_at <= $1",
	      {Value::BIGINT(NowEpoch())});
	ExecP(con, "DELETE FROM quackmail_web_login_fails WHERE at < $1",
	      {Value::BIGINT(NowEpoch() - kLoginWindowSeconds)});
}

std::vector<SessionRow> ListSessions(Connection &con, const std::string &username) {
	std::vector<SessionRow> out;
	std::string sql = "SELECT token_hash, username, peer_ip, user_agent, tls, created_at, last_seen, "
	                  "expires_at FROM quackmail_web_sessions WHERE NOT revoked AND expires_at > $1";
	duckdb::vector<Value> params = {Value::BIGINT(NowEpoch())};
	if (!username.empty()) {
		sql += " AND username = $2";
		params.push_back(Value(username));
	}
	sql += " ORDER BY last_seen DESC";
	auto r = ExecP(con, sql, params);
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		SessionRow s;
		s.token_hash = AsString(mat.GetValue(0, i));
		s.username = AsString(mat.GetValue(1, i));
		s.peer_ip = AsString(mat.GetValue(2, i));
		s.user_agent = AsString(mat.GetValue(3, i));
		s.tls = AsBool(mat.GetValue(4, i));
		s.created_at = AsBigint(mat.GetValue(5, i));
		s.last_seen = AsBigint(mat.GetValue(6, i));
		s.expires_at = AsBigint(mat.GetValue(7, i));
		out.push_back(std::move(s));
	}
	return out;
}

bool LoginAllowed(Connection &con, const std::string &peer_ip, int64_t &retry_after) {
	retry_after = 0;
	if (peer_ip.empty()) {
		return true;
	}
	int64_t cutoff = NowEpoch() - kLoginWindowSeconds;
	auto v = ScalarP(con, "SELECT count(*) FROM quackmail_web_login_fails WHERE peer_ip = $1 AND at >= $2",
	                 {Value(peer_ip), Value::BIGINT(cutoff)});
	if (AsBigint(v) < kMaxLoginFailures) {
		return true;
	}
	auto oldest = ScalarP(con,
	                      "SELECT min(at) FROM quackmail_web_login_fails WHERE peer_ip = $1 AND at >= $2",
	                      {Value(peer_ip), Value::BIGINT(cutoff)});
	retry_after = AsBigint(oldest) + kLoginWindowSeconds - NowEpoch();
	if (retry_after < 1) {
		retry_after = 1;
	}
	return false;
}

void RecordLoginFailure(Connection &con, const std::string &peer_ip, const std::string &username) {
	ExecP(con, "INSERT INTO quackmail_web_login_fails (peer_ip, username, at) VALUES ($1, $2, $3)",
	      {Value(peer_ip), Value(username), Value::BIGINT(NowEpoch())});
}

void ClearLoginFailures(Connection &con, const std::string &peer_ip) {
	ExecP(con, "DELETE FROM quackmail_web_login_fails WHERE peer_ip = $1", {Value(peer_ip)});
}

} // namespace web
} // namespace quackmail
