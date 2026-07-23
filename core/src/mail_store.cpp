#include "quackmail/mail_store.hpp"

#include "quackmail/citadel_store.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <memory>

namespace quackmail {
namespace store {

using duckdb::Connection;
using duckdb::MaterializedQueryResult;
using duckdb::QueryResult;
using duckdb::Value;

namespace {

// Run a parameterized statement, returning the result (or nullptr on error).
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

Value RawBlob(const std::string &bytes) {
	return Value::BLOB(reinterpret_cast<const duckdb::data_t *>(bytes.data()), bytes.size());
}

} // namespace

void EnsureSchema(Connection &con) {
	// A sequence supplies ids for the outbound queue.
	con.Query("CREATE SEQUENCE IF NOT EXISTS quackmail_msg_seq START 1");

	// Outbound relay/submission queue (drained by smtp_out).
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_outbound (
			id              BIGINT DEFAULT nextval('quackmail_msg_seq'),
			from_addr       VARCHAR,
			rcpt            VARCHAR,
			raw             BLOB,
			status          VARCHAR DEFAULT 'queued',
			attempts        INTEGER DEFAULT 0,
			last_error      VARCHAR,
			next_attempt_at TIMESTAMP DEFAULT now(),
			created_at      TIMESTAMP DEFAULT now()
		)
	)");

	// Per-user Sieve scripts (consulted by smtp_in delivery).
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_sieve_scripts (
			username VARCHAR,
			name     VARCHAR,
			active   BOOLEAN DEFAULT false,
			script   VARCHAR
		)
	)");

	// Local user credentials (verified by SASL AUTH and the Citadel USER/PASS).
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_users (
			username      VARCHAR PRIMARY KEY,
			password_hash VARCHAR,
			salt          VARCHAR,
			algo          VARCHAR DEFAULT 'sha256',
			enabled       BOOLEAN DEFAULT true,
			created_at    TIMESTAMP DEFAULT now()
		)
	)");

	// The Citadel room/floor/message model is the message store. Create it here
	// so every extension gets the full schema on load, regardless of load order.
	citadel::EnsureCitadelSchema(con);
}

void EnqueueOutbound(Connection &con, const std::string &from_addr, const std::string &rcpt,
                     const std::string &raw) {
	ExecP(con,
	      "INSERT INTO quackmail_outbound (from_addr, rcpt, raw, status) VALUES ($1, $2, $3, 'queued')",
	      {Value(from_addr), Value(rcpt), RawBlob(raw)});
}

std::vector<OutboundItem> ClaimOutboundDue(Connection &con, int limit) {
	std::vector<OutboundItem> out;
	// Claim atomically: flip the due rows to 'sending' and return them.
	auto r = ExecP(con,
	               "UPDATE quackmail_outbound SET status = 'sending' "
	               "WHERE id IN (SELECT id FROM quackmail_outbound "
	               "             WHERE status = 'queued' AND next_attempt_at <= now() "
	               "             ORDER BY next_attempt_at LIMIT $1) "
	               "RETURNING id, from_addr, rcpt, raw, attempts",
	               {Value::INTEGER(limit)});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
		OutboundItem it;
		it.id = mat.GetValue(0, i).GetValue<int64_t>();
		Value fv = mat.GetValue(1, i);
		it.from_addr = fv.IsNull() ? std::string() : fv.ToString();
		Value rv = mat.GetValue(2, i);
		it.rcpt = rv.IsNull() ? std::string() : rv.ToString();
		Value raw_v = mat.GetValue(3, i);
		it.raw = raw_v.IsNull() ? std::string() : duckdb::StringValue::Get(raw_v);
		Value av = mat.GetValue(4, i);
		it.attempts = av.IsNull() ? 0 : (int)av.GetValue<int64_t>();
		out.push_back(std::move(it));
	}
	return out;
}

void MarkSent(Connection &con, int64_t id) {
	ExecP(con, "UPDATE quackmail_outbound SET status = 'sent' WHERE id = $1", {Value::BIGINT(id)});
}

void MarkFailed(Connection &con, int64_t id, const std::string &err) {
	ExecP(con, "UPDATE quackmail_outbound SET status = 'failed', last_error = $2 WHERE id = $1",
	      {Value::BIGINT(id), Value(err)});
}

void MarkRetry(Connection &con, int64_t id, int attempts, int backoff_secs, const std::string &err) {
	ExecP(con,
	      "UPDATE quackmail_outbound SET status = 'queued', attempts = $2, last_error = $3, "
	      "next_attempt_at = now() + ($4 * INTERVAL 1 SECOND) WHERE id = $1",
	      {Value::BIGINT(id), Value::INTEGER(attempts), Value(err), Value::INTEGER(backoff_secs)});
}

} // namespace store
} // namespace quackmail
