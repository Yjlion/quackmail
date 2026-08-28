#include "quackmail/mail_store.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/fetch.hpp"
#include "quackmail/listserv.hpp"
#include "quackmail/mailpolicy.hpp"
#include "quackmail/quota.hpp"
#include "quackmail/websession.hpp"

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

	// Who a Sieve `vacation` has already answered, and when. RFC 5230 §4.1's
	// whole point is that an auto-reply goes out once per correspondent per
	// window rather than once per message, so the window needs somewhere to
	// live between two deliveries — which is to say a table, since two SMTP
	// sessions share no other state.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_vacation_sent (
			username VARCHAR,
			handle   VARCHAR,
			sender   VARCHAR,
			sent_at  TIMESTAMP DEFAULT now()
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

	// Site policy (domains, aliases, ACLs, DNSBL zones, DKIM keys, quotas).
	// Must follow the Citadel schema: it seeds enforcement defaults into
	// citadel_config.
	policy::EnsureSchema(con);

	// Per-user storage quotas. Must follow the Citadel schema: the usage sum
	// reads citadel_rooms and citadel_room_msgs.
	quota::EnsureSchema(con);

	// Finish the size_bytes backfill EnsureCitadelSchema started in SQL. Only
	// the synthesized formats are left, and each has to be rendered to be
	// measured, so this is guarded by a marker rather than run on every
	// connection — EnsureSchema runs on every one of them.
	if (citadel::GetConfig(con, "qm_msgsize_backfill", "0") != "1") {
		quota::BackfillSizes(con);
		con.Query("INSERT INTO citadel_config (name, value) VALUES ('qm_msgsize_backfill', '1') "
		          "ON CONFLICT (name) DO UPDATE SET value = '1'");
	}

	// Mailing lists over rooms, and remote message pulls into them. Both must
	// follow the Citadel schema, which owns the rooms their rows point at.
	listserv::EnsureSchema(con);
	fetch::EnsureSchema(con);

	// Browser sessions for the HTTP front-end. Created here rather than in the
	// http module so the admin CLI can list and revoke them without it loaded.
	web::EnsureSchema(con);

	// JMAP upload blobs: bytes a client has sent but not yet attached to
	// anything. JMAP defines a blob as temporary until some object references
	// it, so this is a staging area rather than a store — PruneBlobs drops what
	// nothing has claimed.
	//
	// Column names avoid `size` and `data`, which read as reserved enough to be
	// worth not finding out about the way citadel_room_tombstones found out
	// about `at`: EnsureSchema does not check con.Query's result, so a statement
	// that will not parse fails in total silence.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_jmap_blobs (
			blob_id      VARCHAR PRIMARY KEY,
			username     VARCHAR,
			content_type VARCHAR DEFAULT '',
			size_bytes   BIGINT DEFAULT 0,
			payload      BLOB,
			created_at   BIGINT DEFAULT 0
		)
	)");
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
