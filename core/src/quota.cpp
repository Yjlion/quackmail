#include "quackmail/quota.hpp"

#include "quackmail/citadel_msg.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <ctime>

namespace quackmail {
namespace quota {

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

int64_t Int(const Value &v, int64_t dflt = 0) {
	if (v.IsNull()) {
		return dflt;
	}
	try {
		return v.GetValue<int64_t>();
	} catch (...) {
		return dflt;
	}
}

std::string Str(const Value &v) {
	return v.IsNull() ? std::string() : v.ToString();
}

int64_t NowEpoch() {
	return (int64_t)std::time(nullptr);
}

// Bytes this user is keeping. See the header for why the join runs through
// citadel_room_msgs and why the semi-join matters.
int64_t ComputeUsage(Connection &con, const std::string &username) {
	int64_t usernum = citadel::GetOrAssignUserNum(con, username);
	if (usernum <= 0) {
		return 0; // not a local user; GetOrAssignUserNum does not create one
	}
	return Int(ScalarP(con,
	                   "SELECT coalesce(sum(m.size_bytes), 0) FROM citadel_messages m "
	                   // > 0 skips the -1 "not computed yet" sentinel. Without it
	                   // an unbackfilled row contributes minus one byte, which is
	                   // a silent negative-usage bug.
	                   "WHERE m.size_bytes > 0 AND m.msgnum IN ("
	                   "  SELECT rm.msgnum FROM citadel_room_msgs rm "
	                   "  JOIN citadel_rooms r ON r.room_num = rm.room_num "
	                   "  WHERE r.mailbox_owner = $1)",
	                   {Value::BIGINT(usernum)}));
}

Info Compute(Connection &con, const std::string &username, bool always) {
	Info info;
	info.username = username;
	Limit lim = LimitFor(con, username);
	info.limit_bytes = lim.enabled ? lim.limit_bytes : 0;
	info.limited = info.limit_bytes > 0;
	if (!info.limited && !always) {
		return info;
	}
	info.used_bytes = ComputeUsage(con, username);
	info.over = info.limited && info.used_bytes >= info.limit_bytes;
	return info;
}

} // namespace

void EnsureSchema(Connection &con) {
	// Per-user storage quotas, in bytes. The row with username '' is the site
	// default applied to everyone without a row of their own — the same shape
	// quackmail_rate_limits uses, and for the same reason: a site sets one
	// number once, and an exception is a second row rather than a second table.
	//
	// 0 is unlimited, and 0 is the shipped default. A quota that appeared the
	// day a server was upgraded and started deferring mail would be a data-loss
	// bug wearing a feature's clothes.
	con.Query("CREATE TABLE IF NOT EXISTS quackmail_quotas ("
	          "username VARCHAR PRIMARY KEY, "
	          "limit_bytes BIGINT DEFAULT 0, "
	          "enabled BOOLEAN DEFAULT true, "
	          // Bumped on every SetQuota. JMAP's Quota/changes needs a state that
	          // moves when the *limit* moves, and a message-derived account state
	          // cannot see an admin editing a number.
	          "changed_at BIGINT DEFAULT 0)");
	con.Query("INSERT OR IGNORE INTO quackmail_quotas (username, limit_bytes) VALUES ('', 0)");
}

Limit LimitFor(Connection &con, const std::string &username) {
	Limit lim;
	lim.username = username;
	auto r = ExecP(con,
	               "SELECT username, limit_bytes, enabled, changed_at FROM quackmail_quotas "
	               "WHERE username IN ($1, '') "
	               // The user's own row (non-empty username) sorts first, so it
	               // wins over the default row when both exist.
	               "ORDER BY username DESC LIMIT 1",
	               {Value(username)});
	if (!r) {
		return lim;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return lim;
	}
	lim.username = Str(mat.GetValue(0, 0));
	lim.limit_bytes = Int(mat.GetValue(1, 0));
	Value e = mat.GetValue(2, 0);
	lim.enabled = e.IsNull() ? true : e.GetValue<bool>();
	lim.changed_at = Int(mat.GetValue(3, 0));
	return lim;
}

Info Usage(Connection &con, const std::string &username) {
	return Compute(con, username, false);
}

Info UsageAlways(Connection &con, const std::string &username) {
	return Compute(con, username, true);
}

bool WouldExceed(Connection &con, const std::string &username, int64_t extra_bytes) {
	Info info = Usage(con, username);
	if (!info.limited) {
		return false;
	}
	if (extra_bytes < 0) {
		extra_bytes = 0;
	}
	return info.used_bytes + extra_bytes > info.limit_bytes;
}

bool IsOver(Connection &con, const std::string &username) {
	return Usage(con, username).over;
}

int64_t MessageSize(const citadel::Message &msg, const std::string &node) {
	// Format 4 is stored as the RFC822 bytes and served unchanged, so rendering
	// it would be a copy of the whole body to measure something already known.
	if (msg.format_type == 4) {
		return (int64_t)msg.raw.size();
	}
	return (int64_t)citadel::RenderRfc822(msg, node).size();
}

bool SetQuota(Connection &con, const std::string &username, int64_t limit_bytes, std::string &err) {
	if (limit_bytes < 0) {
		err = "a quota cannot be negative";
		return false;
	}
	// A monotonic generation seeded from the clock, not the clock itself.
	//
	// changed_at is what JMAP's Quota/changes compares, and the clock has
	// one-second resolution: two edits inside the same second would produce the
	// same state, and a client polling between them would be told nothing had
	// happened. Taking one past the highest value in the table guarantees the
	// number strictly increases while still tracking wall time on a quiet system.
	int64_t gen = Int(ScalarP(con,
	                          "SELECT greatest($1, coalesce(max(changed_at), 0) + 1) "
	                          "FROM quackmail_quotas",
	                          {Value::BIGINT(NowEpoch())}),
	                  NowEpoch());
	auto r = ExecP(con,
	               "INSERT INTO quackmail_quotas (username, limit_bytes, changed_at) "
	               "VALUES ($1, $2, $3) ON CONFLICT (username) DO UPDATE SET "
	               "limit_bytes = excluded.limit_bytes, changed_at = excluded.changed_at, "
	               "enabled = true",
	               {Value(username), Value::BIGINT(limit_bytes), Value::BIGINT(gen)});
	if (!r) {
		err = "could not store the quota";
		return false;
	}
	return true;
}

std::vector<Limit> ListQuotas(Connection &con) {
	std::vector<Limit> out;
	auto r = con.Query("SELECT username, limit_bytes, enabled, changed_at FROM quackmail_quotas "
	                   "ORDER BY username");
	if (!r || r->HasError()) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
		Limit lim;
		lim.username = Str(mat.GetValue(0, i));
		lim.limit_bytes = Int(mat.GetValue(1, i));
		Value e = mat.GetValue(2, i);
		lim.enabled = e.IsNull() ? true : e.GetValue<bool>();
		lim.changed_at = Int(mat.GetValue(3, i));
		out.push_back(lim);
	}
	return out;
}

int64_t LimitGeneration(Connection &con, const std::string &username) {
	return Int(ScalarP(con,
	                   "SELECT coalesce(max(changed_at), 0) FROM quackmail_quotas "
	                   "WHERE username IN ($1, '')",
	                   {Value(username)}));
}

int64_t BackfillSizes(Connection &con) {
	// Format 4 is handled in SQL by EnsureCitadelSchema; what is left is the
	// synthesized-header formats, which have to go through RenderRfc822 one at a
	// time. Batched so a large archive does not build one enormous transaction.
	const std::string node = citadel::GetConfig(con, "c_nodename", "quackcit");
	int64_t updated = 0;
	for (;;) {
		auto r = con.Query("SELECT msgnum FROM citadel_messages WHERE size_bytes < 0 LIMIT 500");
		if (!r || r->HasError()) {
			break;
		}
		auto &mat = r->Cast<MaterializedQueryResult>();
		if (mat.RowCount() == 0) {
			break;
		}
		for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
			int64_t msgnum = Int(mat.GetValue(0, i));
			citadel::Message msg;
			int64_t size = 0;
			if (citadel::LoadMessage(con, msgnum, msg)) {
				size = MessageSize(msg, node);
			}
			// A message that will not load still needs its sentinel cleared, or
			// the batch loop selects it again forever.
			ExecP(con, "UPDATE citadel_messages SET size_bytes = $2 WHERE msgnum = $1",
			      {Value::BIGINT(msgnum), Value::BIGINT(size)});
			updated++;
		}
	}
	return updated;
}

} // namespace quota
} // namespace quackmail
