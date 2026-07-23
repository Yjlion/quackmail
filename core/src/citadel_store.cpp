#include "quackmail/citadel_store.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <string>

namespace quackmail {
namespace citadel {

using duckdb::Connection;
using duckdb::idx_t;
using duckdb::MaterializedQueryResult;
using duckdb::QueryResult;
using duckdb::Value;

namespace {

constexpr const char *kRoomCols = "room_num, name, display_name, floor_num, qr_flags, password, "
                                  "listorder, default_view, info, mailbox_owner, highest_msg";

// Run a parameterized statement, returning the materialized result (or nullptr).
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

// Run a parameterized statement expecting a single scalar; NULL Value on miss.
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

Room RowToRoom(MaterializedQueryResult &mat, idx_t row) {
	Room r;
	r.room_num = AsBigint(mat.GetValue(0, row));
	r.name = AsString(mat.GetValue(1, row));
	r.display_name = AsString(mat.GetValue(2, row));
	r.floor_num = AsBigint(mat.GetValue(3, row));
	r.qr_flags = AsBigint(mat.GetValue(4, row));
	r.password = AsString(mat.GetValue(5, row));
	r.listorder = AsBigint(mat.GetValue(6, row));
	r.default_view = AsBigint(mat.GetValue(7, row));
	r.info = AsString(mat.GetValue(8, row));
	r.mailbox_owner = AsBigint(mat.GetValue(9, row));
	r.highest_msg = AsBigint(mat.GetValue(10, row));
	return r;
}

} // namespace

void EnsureCitadelSchema(Connection &con) {
	// Reserve low room numbers (0=Lobby, 1=Aide) so seeded system rooms never
	// collide with user-created rooms.
	con.Query("CREATE SEQUENCE IF NOT EXISTS citadel_user_seq START 1");
	con.Query("CREATE SEQUENCE IF NOT EXISTS citadel_floor_seq START 10");
	con.Query("CREATE SEQUENCE IF NOT EXISTS citadel_room_seq START 100");
	con.Query("CREATE SEQUENCE IF NOT EXISTS citadel_msg_seq START 1");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_users (
			username      VARCHAR PRIMARY KEY,
			usernum       BIGINT UNIQUE,
			axlevel       INTEGER DEFAULT 4,
			flags         BIGINT DEFAULT 0,
			times_called  BIGINT DEFAULT 0,
			num_posts     BIGINT DEFAULT 0,
			last_call     TIMESTAMP,
			created_at    TIMESTAMP DEFAULT now()
		)
	)");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_floors (
			floor_num  BIGINT PRIMARY KEY,
			name       VARCHAR,
			created_at TIMESTAMP DEFAULT now()
		)
	)");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_rooms (
			room_num      BIGINT PRIMARY KEY,
			name          VARCHAR UNIQUE,
			display_name  VARCHAR,
			floor_num     BIGINT DEFAULT 0,
			qr_flags      BIGINT DEFAULT 0,
			password      VARCHAR DEFAULT '',
			listorder     BIGINT DEFAULT 0,
			default_view  BIGINT DEFAULT 0,
			info          VARCHAR DEFAULT '',
			mailbox_owner BIGINT DEFAULT 0,
			highest_msg   BIGINT DEFAULT 0,
			created_at    TIMESTAMP DEFAULT now()
		)
	)");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_messages (
			msgnum         BIGINT PRIMARY KEY,
			euid           VARCHAR,
			author         VARCHAR,
			author_usernum BIGINT DEFAULT 0,
			recipient      VARCHAR,
			node           VARCHAR,
			msgtime        BIGINT,
			subject        VARCHAR,
			format_type    INTEGER DEFAULT 0,
			refs           VARCHAR,
			origin_room    VARCHAR,
			raw            BLOB,
			created_at     TIMESTAMP DEFAULT now()
		)
	)");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_room_msgs (
			room_num BIGINT,
			msgnum   BIGINT,
			PRIMARY KEY (room_num, msgnum)
		)
	)");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_room_state (
			username  VARCHAR,
			room_num  BIGINT,
			last_read BIGINT DEFAULT 0,
			flags     BIGINT DEFAULT 0,
			PRIMARY KEY (username, room_num)
		)
	)");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_msg_flags (
			msgnum   BIGINT,
			username VARCHAR,
			flag     VARCHAR
		)
	)");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_config (
			name  VARCHAR PRIMARY KEY,
			value VARCHAR
		)
	)");

	// Seed the base floor and system rooms (fixed ids -> idempotent, no seq churn).
	// These mirror a stock Citadel install's public/system rooms (see LKRA on a
	// real server: Lobby, Aide, Global Address Book, Trashcan) with matching
	// qr_flags and default_view so clients see the same room set.
	con.Query("INSERT OR IGNORE INTO citadel_floors (floor_num, name) VALUES (0, 'Main Floor')");
	con.Query("INSERT OR IGNORE INTO citadel_rooms "
	          "(room_num, name, display_name, floor_num, qr_flags, default_view, listorder, info) VALUES "
	          "(0, 'Lobby', 'Lobby', 0, 2, 0, 0, 'Welcome to the Lobby.'), "
	          "(1, 'Aide', 'Aide', 0, 6, 0, 10, 'Room for system administrators.'), "
	          "(2, 'Global Address Book', 'Global Address Book', 0, 6, 2, 20, 'Shared address book.'), "
	          "(3, 'Trashcan', 'Trashcan', 0, 2, 0, 30, 'Discarded rooms land here.')");

	con.Query("INSERT OR IGNORE INTO citadel_config (name, value) VALUES "
	          "('c_nodename', 'quackcit'), "
	          "('c_humannode', 'QuackCit BBS'), "
	          "('c_fqdn', 'quackmail.test'), "
	          "('c_sysadm', 'admin'), "
	          "('c_version', 'QuackCit 0.1.0'), "
	          "('c_bbs_city', 'The Cloud')");
}

std::string GetConfig(Connection &con, const std::string &name, const std::string &dflt) {
	auto v = ScalarP(con, "SELECT value FROM citadel_config WHERE name = $1", {Value(name)});
	return v.IsNull() ? dflt : v.ToString();
}

int64_t GetOrAssignUserNum(Connection &con, const std::string &username) {
	// Only assign numbers to real local users (present in the credential table).
	auto exists = ScalarP(con, "SELECT 1 FROM quackmail_users WHERE username = $1", {Value(username)});
	if (exists.IsNull()) {
		return 0;
	}
	auto existing = ScalarP(con, "SELECT usernum FROM citadel_users WHERE username = $1", {Value(username)});
	if (!existing.IsNull()) {
		return existing.GetValue<int64_t>();
	}
	auto num = ScalarP(con, "SELECT nextval('citadel_user_seq')", {});
	if (num.IsNull()) {
		return 0;
	}
	ExecP(con, "INSERT OR IGNORE INTO citadel_users (username, usernum, axlevel) VALUES ($1, $2, 4)",
	      {Value(username), num});
	// Re-select to settle any race (another session may have assigned first).
	auto settled = ScalarP(con, "SELECT usernum FROM citadel_users WHERE username = $1", {Value(username)});
	return AsBigint(settled);
}

int64_t GetAxLevel(Connection &con, const std::string &username) {
	auto v = ScalarP(con, "SELECT axlevel FROM citadel_users WHERE username = $1", {Value(username)});
	return v.IsNull() ? 4 : v.GetValue<int64_t>();
}

bool IsLocalUser(Connection &con, const std::string &addr) {
	auto at = addr.find('@');
	std::string local = (at == std::string::npos) ? addr : addr.substr(0, at);
	if (at != std::string::npos) {
		// An explicit domain that is not ours means this is a relay target.
		std::string domain = addr.substr(at + 1);
		std::string fqdn = GetConfig(con, "c_fqdn", "");
		auto lower = [](std::string s) {
			for (char &c : s) {
				if (c >= 'A' && c <= 'Z') {
					c = char(c - 'A' + 'a');
				}
			}
			return s;
		};
		if (!domain.empty() && !fqdn.empty() && lower(domain) != lower(fqdn)) {
			return false;
		}
	}
	return GetOrAssignUserNum(con, local) > 0;
}

std::vector<Floor> ListFloors(Connection &con) {
	std::vector<Floor> out;
	auto r = con.Query("SELECT f.floor_num, f.name, "
	                   "(SELECT count(*) FROM citadel_rooms r WHERE r.floor_num = f.floor_num) "
	                   "FROM citadel_floors f ORDER BY f.floor_num");
	if (r->HasError()) {
		return out;
	}
	for (idx_t i = 0; i < r->RowCount(); i++) {
		Floor f;
		f.floor_num = AsBigint(r->GetValue(0, i));
		f.name = AsString(r->GetValue(1, i));
		f.room_count = AsBigint(r->GetValue(2, i));
		out.push_back(std::move(f));
	}
	return out;
}

int64_t CreateFloor(Connection &con, const std::string &name, std::string &err) {
	auto num = ScalarP(con, "SELECT nextval('citadel_floor_seq')", {});
	if (num.IsNull()) {
		err = "could not allocate floor number";
		return -1;
	}
	auto r = ExecP(con, "INSERT INTO citadel_floors (floor_num, name) VALUES ($1, $2)", {num, Value(name)});
	if (!r) {
		err = "insert failed";
		return -1;
	}
	return num.GetValue<int64_t>();
}

bool GetRoomByNum(Connection &con, int64_t room_num, Room &out) {
	auto r = ExecP(con, std::string("SELECT ") + kRoomCols + " FROM citadel_rooms WHERE room_num = $1",
	               {Value::BIGINT(room_num)});
	if (!r) {
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return false;
	}
	out = RowToRoom(mat, 0);
	return true;
}

bool ResolveRoom(Connection &con, const std::string &username, const std::string &wanted, Room &out) {
	// A public (non-mailbox) room by display name.
	auto r = ExecP(con, std::string("SELECT ") + kRoomCols +
	                        " FROM citadel_rooms WHERE mailbox_owner = 0 AND lower(display_name) = lower($1) LIMIT 1",
	               {Value(wanted)});
	if (r) {
		auto &mat = r->Cast<MaterializedQueryResult>();
		if (mat.RowCount() >= 1) {
			out = RowToRoom(mat, 0);
			return true;
		}
	}
	// The logged-in user's own mailbox room (e.g. "Mail").
	if (!username.empty()) {
		int64_t usernum = GetOrAssignUserNum(con, username);
		if (usernum > 0) {
			auto r2 = ExecP(con, std::string("SELECT ") + kRoomCols +
			                         " FROM citadel_rooms WHERE mailbox_owner = $1 AND lower(display_name) = lower($2) "
			                         "LIMIT 1",
			                {Value::BIGINT(usernum), Value(wanted)});
			if (r2) {
				auto &mat = r2->Cast<MaterializedQueryResult>();
				if (mat.RowCount() >= 1) {
					out = RowToRoom(mat, 0);
					return true;
				}
			}
		}
	}
	return false;
}

std::vector<Room> ListRooms(Connection &con, const std::string &username, int64_t floor,
                            const std::string &which) {
	std::vector<Room> out;
	int64_t usernum = username.empty() ? 0 : GetOrAssignUserNum(con, username);
	bool is_aide = !username.empty() && GetAxLevel(con, username) >= 6;

	std::string sql = std::string("SELECT ") + kRoomCols +
	                  " FROM citadel_rooms WHERE (mailbox_owner = 0 OR mailbox_owner = $1)";
	if (!is_aide) {
		// Hide other people's private rooms from non-aides, but always show a
		// user their own mailbox rooms (which are themselves flagged private).
		sql += " AND ((qr_flags & 4) = 0 OR mailbox_owner = $1)";
	}
	if (floor >= 0) {
		sql += " AND floor_num = $2";
	}
	sql += " ORDER BY floor_num, listorder, display_name";

	duckdb::vector<Value> params = {Value::BIGINT(usernum)};
	if (floor >= 0) {
		params.push_back(Value::BIGINT(floor));
	}
	auto r = ExecP(con, sql, params);
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		Room room = RowToRoom(mat, i);
		if (which == "new" || which == "old") {
			auto stats = GetRoomStats(con, username, room.room_num);
			bool has_new = stats.new_count > 0;
			if ((which == "new") != has_new) {
				continue;
			}
		}
		out.push_back(std::move(room));
	}
	return out;
}

int64_t CreateRoom(Connection &con, const std::string &display_name, int64_t floor, int64_t qr_flags,
                   const std::string &password, int64_t mailbox_owner, std::string &err) {
	auto num = ScalarP(con, "SELECT nextval('citadel_room_seq')", {});
	if (num.IsNull()) {
		err = "could not allocate room number";
		return -1;
	}
	std::string internal =
	    mailbox_owner > 0 ? std::to_string(mailbox_owner) + "." + display_name : display_name;
	auto r = ExecP(con,
	               "INSERT INTO citadel_rooms (room_num, name, display_name, floor_num, qr_flags, password, "
	               "mailbox_owner) VALUES ($1, $2, $3, $4, $5, $6, $7)",
	               {num, Value(internal), Value(display_name), Value::BIGINT(floor), Value::BIGINT(qr_flags),
	                Value(password), Value::BIGINT(mailbox_owner)});
	if (!r) {
		err = "room already exists";
		return -1;
	}
	return num.GetValue<int64_t>();
}

bool KillRoom(Connection &con, int64_t room_num, std::string &err) {
	if (room_num == kLobbyRoom) {
		err = "cannot delete the Lobby";
		return false;
	}
	ExecP(con, "DELETE FROM citadel_room_msgs WHERE room_num = $1", {Value::BIGINT(room_num)});
	ExecP(con, "DELETE FROM citadel_room_state WHERE room_num = $1", {Value::BIGINT(room_num)});
	auto r = ExecP(con, "DELETE FROM citadel_rooms WHERE room_num = $1", {Value::BIGINT(room_num)});
	if (!r) {
		err = "delete failed";
		return false;
	}
	return true;
}

int64_t GetOrCreateUserRoom(Connection &con, const std::string &username, const std::string &display_name) {
	int64_t usernum = GetOrAssignUserNum(con, username);
	if (usernum <= 0) {
		return -1;
	}
	std::string internal = std::to_string(usernum) + "." + display_name;
	auto existing = ScalarP(con, "SELECT room_num FROM citadel_rooms WHERE name = $1", {Value(internal)});
	if (!existing.IsNull()) {
		return existing.GetValue<int64_t>();
	}
	std::string err;
	// Personal mailbox rooms are permanent (never auto-purged) — matches the
	// qr_flags a real Citadel server reports for them (16390).
	return CreateRoom(con, display_name, 0, QR_MAILBOX | QR_PRIVATE | QR_PERMANENT, "", usernum, err);
}

int64_t GetOrCreateMailRoom(Connection &con, const std::string &username) {
	return GetOrCreateUserRoom(con, username, "Mail");
}

void EnsureUserRooms(Connection &con, const std::string &username) {
	int64_t usernum = GetOrAssignUserNum(con, username);
	if (usernum <= 0) {
		return;
	}
	// The default groupware rooms Citadel provisions for every user, each with
	// its Citadel view code so clients render them correctly (mail/calendar/...).
	struct DefRoom {
		const char *name;
		int64_t view;
	};
	static const DefRoom kDefaults[] = {
	    {"Mail", VIEW_MAILBOX},       {"Sent Items", VIEW_MAILBOX}, {"Drafts", VIEW_MAILBOX},
	    {"Trash", VIEW_MAILBOX},      {"Calendar", VIEW_CALENDAR},  {"Contacts", VIEW_ADDRESSBOOK},
	    {"Notes", VIEW_NOTES},        {"Tasks", VIEW_TASKS},
	};
	for (const auto &d : kDefaults) {
		int64_t room_num = GetOrCreateUserRoom(con, username, d.name);
		if (room_num >= 0 && d.view != VIEW_BBS) {
			ExecP(con, "UPDATE citadel_rooms SET default_view = $1 WHERE room_num = $2",
			      {Value::BIGINT(d.view), Value::BIGINT(room_num)});
		}
	}
}

RoomStats GetRoomStats(Connection &con, const std::string &username, int64_t room_num) {
	RoomStats s;
	s.total = AsBigint(ScalarP(con, "SELECT count(*) FROM citadel_room_msgs WHERE room_num = $1",
	                           {Value::BIGINT(room_num)}));
	s.highest = AsBigint(ScalarP(con, "SELECT coalesce(max(msgnum), 0) FROM citadel_room_msgs WHERE room_num = $1",
	                             {Value::BIGINT(room_num)}));
	if (!username.empty()) {
		s.last_read = AsBigint(ScalarP(con,
		                               "SELECT last_read FROM citadel_room_state WHERE username = $1 AND room_num = $2",
		                               {Value(username), Value::BIGINT(room_num)}));
	}
	s.new_count = AsBigint(ScalarP(con,
	                               "SELECT count(*) FROM citadel_room_msgs WHERE room_num = $1 AND msgnum > $2",
	                               {Value::BIGINT(room_num), Value::BIGINT(s.last_read)}));
	return s;
}

void SetLastRead(Connection &con, const std::string &username, int64_t room_num, int64_t msgnum) {
	if (username.empty()) {
		return;
	}
	ExecP(con,
	      "INSERT INTO citadel_room_state (username, room_num, last_read) VALUES ($1, $2, $3) "
	      "ON CONFLICT (username, room_num) DO UPDATE SET last_read = excluded.last_read",
	      {Value(username), Value::BIGINT(room_num), Value::BIGINT(msgnum)});
}

int64_t InsertMessage(Connection &con, const Message &msg, const std::vector<int64_t> &rooms, std::string &err) {
	auto begin = con.Query("BEGIN TRANSACTION");
	if (begin->HasError()) {
		err = begin->GetError();
		return -1;
	}
	auto fail = [&](const std::string &e) -> int64_t {
		err = e;
		con.Query("ROLLBACK");
		return -1;
	};

	auto num_v = ScalarP(con, "SELECT nextval('citadel_msg_seq')", {});
	if (num_v.IsNull()) {
		return fail("could not allocate message number");
	}
	int64_t msgnum = num_v.GetValue<int64_t>();

	auto ins = ExecP(con,
	                 "INSERT INTO citadel_messages (msgnum, euid, author, author_usernum, recipient, node, "
	                 "msgtime, subject, format_type, refs, origin_room, raw) "
	                 "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)",
	                 {Value::BIGINT(msgnum), Value(msg.euid), Value(msg.author),
	                  Value::BIGINT(msg.author_usernum), Value(msg.recipient), Value(msg.node),
	                  Value::BIGINT(msg.msgtime), Value(msg.subject), Value::INTEGER(msg.format_type),
	                  Value(msg.references), Value(msg.origin_room),
	                  Value::BLOB(reinterpret_cast<const duckdb::data_t *>(msg.raw.data()), msg.raw.size())});
	if (!ins) {
		return fail("message insert failed");
	}

	for (int64_t room : rooms) {
		auto p = ExecP(con, "INSERT OR IGNORE INTO citadel_room_msgs (room_num, msgnum) VALUES ($1, $2)",
		               {Value::BIGINT(room), Value::BIGINT(msgnum)});
		if (!p) {
			return fail("room pointer insert failed");
		}
		ExecP(con, "UPDATE citadel_rooms SET highest_msg = greatest(highest_msg, $2) WHERE room_num = $1",
		      {Value::BIGINT(room), Value::BIGINT(msgnum)});
	}

	auto commit = con.Query("COMMIT");
	if (commit->HasError()) {
		return fail(commit->GetError());
	}
	return msgnum;
}

std::vector<int64_t> RoomMessages(Connection &con, int64_t room_num, const std::string &filter, int64_t param,
                                  int64_t last_read) {
	std::vector<int64_t> out;
	std::string sql = "SELECT msgnum FROM citadel_room_msgs WHERE room_num = $1";
	duckdb::vector<Value> params = {Value::BIGINT(room_num)};
	bool reverse = false;

	if (filter == "new") {
		sql += " AND msgnum > $2 ORDER BY msgnum";
		params.push_back(Value::BIGINT(last_read));
	} else if (filter == "old") {
		sql += " AND msgnum <= $2 ORDER BY msgnum";
		params.push_back(Value::BIGINT(last_read));
	} else if (filter == "gt") {
		sql += " AND msgnum > $2 ORDER BY msgnum";
		params.push_back(Value::BIGINT(param));
	} else if (filter == "lt") {
		sql += " AND msgnum < $2 ORDER BY msgnum";
		params.push_back(Value::BIGINT(param));
	} else if (filter == "first") {
		sql += " ORDER BY msgnum LIMIT $2";
		params.push_back(Value::BIGINT(param));
	} else if (filter == "last") {
		sql += " ORDER BY msgnum DESC LIMIT $2";
		params.push_back(Value::BIGINT(param));
		reverse = true;
	} else { // "all"
		sql += " ORDER BY msgnum";
	}

	auto r = ExecP(con, sql, params);
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		out.push_back(AsBigint(mat.GetValue(0, i)));
	}
	if (reverse) {
		std::reverse(out.begin(), out.end());
	}
	return out;
}

bool LoadMessage(Connection &con, int64_t msgnum, Message &out) {
	auto r = ExecP(con,
	               "SELECT msgnum, euid, author, author_usernum, recipient, node, msgtime, subject, "
	               "format_type, refs, origin_room, raw FROM citadel_messages WHERE msgnum = $1",
	               {Value::BIGINT(msgnum)});
	if (!r) {
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return false;
	}
	out.msgnum = AsBigint(mat.GetValue(0, 0));
	out.euid = AsString(mat.GetValue(1, 0));
	out.author = AsString(mat.GetValue(2, 0));
	out.author_usernum = AsBigint(mat.GetValue(3, 0));
	out.recipient = AsString(mat.GetValue(4, 0));
	out.node = AsString(mat.GetValue(5, 0));
	out.msgtime = AsBigint(mat.GetValue(6, 0));
	out.subject = AsString(mat.GetValue(7, 0));
	out.format_type = (int)AsBigint(mat.GetValue(8, 0));
	out.references = AsString(mat.GetValue(9, 0));
	out.origin_room = AsString(mat.GetValue(10, 0));
	Value raw_v = mat.GetValue(11, 0);
	out.raw = raw_v.IsNull() ? std::string() : duckdb::StringValue::Get(raw_v);
	return true;
}

} // namespace citadel
} // namespace quackmail
