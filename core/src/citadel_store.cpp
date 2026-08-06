#include "quackmail/citadel_store.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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

int64_t NowEpoch() {
	return (int64_t)std::time(nullptr);
}

// A personal room's internal key. Citadel names these "<usernum>.<room>" with
// the user number zero-padded to ten digits ("0000000002.Mail"), which is what
// shows up on the wire in NNTP group names, so match it exactly.
std::string MailboxRoomName(int64_t usernum, const std::string &display_name) {
	char prefix[16];
	std::snprintf(prefix, sizeof prefix, "%010lld", (long long)usernum);
	return std::string(prefix) + "." + display_name;
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

	// Terminal geometry was added after the table shipped; keep older files working.
	con.Query("ALTER TABLE citadel_users ADD COLUMN IF NOT EXISTS screenwidth INTEGER DEFAULT 80");
	con.Query("ALTER TABLE citadel_users ADD COLUMN IF NOT EXISTS screenheight INTEGER DEFAULT 24");

	// Citadel's REGI/GREG registration record plus the EBIO/RBIO biography. Kept
	// out of citadel_users because it is optional and much wider than the rest.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_user_reg (
			username  VARCHAR PRIMARY KEY,
			real_name VARCHAR DEFAULT '',
			street    VARCHAR DEFAULT '',
			city      VARCHAR DEFAULT '',
			state     VARCHAR DEFAULT '',
			zipcode   VARCHAR DEFAULT '',
			phone     VARCHAR DEFAULT '',
			email     VARCHAR DEFAULT '',
			country   VARCHAR DEFAULT '',
			bio       VARCHAR DEFAULT ''
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

	// Groupware looks messages up by euid rather than by number — that is how a
	// contact or event is replaced in place. Without this every such lookup is a
	// full scan of citadel_messages, which a calendar does once per item.
	con.Query("CREATE INDEX IF NOT EXISTS idx_msg_euid ON citadel_messages(euid)");

	// Removals, so a synchronizing client can be told about them.
	//
	// An addition is already discoverable: msgnum comes from a monotone sequence,
	// so "what is new since token N" is a range scan of citadel_room_msgs and the
	// insert path needs no help. A removal leaves nothing behind at all —
	// DeleteMessage unlinks the pointer row, and citadel_rooms.highest_msg only
	// ever grows via greatest() — so without this table a deletion is invisible
	// to any token derived from the store, and a CalDAV client would keep showing
	// an event the user cancelled.
	//
	// `seq` comes from citadel_msg_seq, the same sequence msgnum does, so the two
	// share one monotone space and a single integer is a valid sync token for
	// both halves.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_room_tombstones (
			room_num BIGINT,
			seq      BIGINT,
			euid       VARCHAR DEFAULT '',
			msgnum     BIGINT,
			-- Not `at`: DuckDB parses AT as a keyword (AT TIME ZONE, and the
			-- time-travel FROM ... AT clause), so the column name would take
			-- the whole CREATE down — silently, because con.Query's result is
			-- not checked here any more than it is for the tables above.
			deleted_at BIGINT,
			PRIMARY KEY (room_num, seq)
		)
	)");

	// What a DAV client calls each object.
	//
	// The store keys a groupware object by its own UID, and that invariant is
	// load-bearing: the native ENT0/EUID path, the web UI and the DAV module all
	// rely on it to agree about what a second save means. But a WebDAV client
	// owns the URL space, and several shipping ones — vdirsyncer among them —
	// name a new resource with a random UUID of their own rather than with the
	// object's UID. Refusing that is refusing the client.
	//
	// So the two names are separated: euid stays the object's UID, and this
	// records the resource name it is served under. A row exists only where the
	// two differ from the default encoding, so objects created through the web
	// UI or by the native protocol need none.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_dav_names (
			room_num BIGINT,
			name     VARCHAR,
			euid     VARCHAR,
			PRIMARY KEY (room_num, name)
		)
	)");
	con.Query("CREATE INDEX IF NOT EXISTS idx_dav_names_euid ON citadel_dav_names(room_num, euid)");

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

	// Cross-session presence (RWHO) and instant messages (SEXP/GEXP). Per the
	// "tables are the bus" design, live session state lives in DuckDB rather than
	// shared C++ memory, so any connection/extension sees a consistent view.
	con.Query("CREATE SEQUENCE IF NOT EXISTS citadel_session_seq START 1");
	con.Query("CREATE SEQUENCE IF NOT EXISTS citadel_express_seq START 1");
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_sessions (
			session_id BIGINT PRIMARY KEY,
			username   VARCHAR DEFAULT '',
			host       VARCHAR DEFAULT '',
			room       VARCHAR DEFAULT '',
			last_cmd   VARCHAR DEFAULT '',
			client     VARCHAR DEFAULT '',
			axlevel    BIGINT DEFAULT 0,
			since      BIGINT DEFAULT 0,
			last_seen  BIGINT DEFAULT 0
		)
	)");
	// `client` was added after the table shipped; keep older database files working.
	con.Query("ALTER TABLE citadel_sessions ADD COLUMN IF NOT EXISTS client VARCHAR DEFAULT ''");

	// String-valued per-user preferences. The US_* bit field covers the boolean
	// BBS toggles; anything with a value (the web theme, so far) lives here.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_user_prefs (
			username VARCHAR,
			name     VARCHAR,
			value    VARCHAR DEFAULT '',
			PRIMARY KEY (username, name)
		)
	)");

	// RFC 4314 access control lists. Only explicit grants live here; ordinary
	// permissions are derived from the room itself (see EffectiveRights).
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_room_acl (
			room_num   BIGINT,
			identifier VARCHAR,
			rights     VARCHAR DEFAULT '',
			PRIMARY KEY (room_num, identifier)
		)
	)");
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_express (
			id        BIGINT PRIMARY KEY,
			to_user   VARCHAR,
			from_user VARCHAR,
			text      VARCHAR,
			sent_at   BIGINT,
			delivered BOOLEAN DEFAULT false
		)
	)");

	// Personal room keys used to be "<usernum>.<room>" without padding; Citadel
	// pads the user number to ten digits and that name is visible over NNTP.
	con.Query("UPDATE citadel_rooms "
	          "SET name = lpad(mailbox_owner::VARCHAR, 10, '0') || '.' || display_name "
	          "WHERE mailbox_owner > 0 "
	          "  AND name <> lpad(mailbox_owner::VARCHAR, 10, '0') || '.' || display_name");

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
	          "('c_version', 'QuackCit 0.6.0'), "
	          "('c_bbs_city', 'The Cloud'), "
	          // System messages to the Aide room. Rejected inbound mail is off by
	          // default: on a live MX it would bury everything else.
	          "('qm_aide_log', '1'), "
	          "('qm_aide_log_rejects', '0'), "
	          // Public room addresses (room_<name>@) and RFC 5233 subaddressing.
	          // Room mail is on, but no room is reachable until its ACL grants
	          // "anyone" the `p` right, so this only decides whether the lookup
	          // happens at all. Subaddressed folders are never auto-created:
	          // the sender picks the name.
	          "('qm_room_mail', '1'), "
	          "('qm_subaddress_sep', '+'), "
	          "('qm_subaddress_create', '0'), "
	          // Mailing lists. As with room mail this only decides whether the
	          // lookup happens: no room is a list until an aide makes one, and
	          // nothing is distributed until the spooler runs. The archive base
	          // is the site's web root; empty omits the List-Archive header
	          // rather than guessing a URL that may not resolve.
	          "('qm_listserv', '1'), "
	          "('qm_list_archive_base', '')");
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

namespace {

// citadel_users joined with the credential row, in UserInfo field order. The
// credential table is the authority on existence, so this is a LEFT JOIN from
// quackmail_users: a user always shows up, even before a Citadel record exists.
constexpr const char *kUserSelect =
    "SELECT q.username, coalesce(c.usernum, 0), coalesce(c.axlevel, 4), coalesce(c.flags, 0), "
    "coalesce(c.times_called, 0), coalesce(c.num_posts, 0), "
    "coalesce(epoch(c.last_call), 0)::BIGINT, coalesce(q.enabled, false), "
    "coalesce(c.screenwidth, 80)::BIGINT, coalesce(c.screenheight, 24)::BIGINT "
    "FROM quackmail_users q LEFT JOIN citadel_users c ON c.username = q.username";

UserInfo RowToUser(MaterializedQueryResult &mat, idx_t row) {
	UserInfo u;
	u.username = AsString(mat.GetValue(0, row));
	u.usernum = AsBigint(mat.GetValue(1, row));
	u.axlevel = AsBigint(mat.GetValue(2, row), 4);
	u.flags = AsBigint(mat.GetValue(3, row));
	u.times_called = AsBigint(mat.GetValue(4, row));
	u.num_posts = AsBigint(mat.GetValue(5, row));
	u.last_call = AsBigint(mat.GetValue(6, row));
	auto en = mat.GetValue(7, row);
	u.enabled = !en.IsNull() && en.GetValue<bool>();
	u.screenwidth = AsBigint(mat.GetValue(8, row), 80);
	u.screenheight = AsBigint(mat.GetValue(9, row), 24);
	return u;
}

} // namespace

std::vector<UserInfo> ListUsers(Connection &con) {
	std::vector<UserInfo> out;
	auto r = con.Query(std::string(kUserSelect) + " ORDER BY q.username");
	if (r->HasError()) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		out.push_back(RowToUser(mat, i));
	}
	return out;
}

bool GetUser(Connection &con, const std::string &username, UserInfo &out) {
	auto r = ExecP(con, std::string(kUserSelect) + " WHERE q.username = $1", {Value(username)});
	if (!r) {
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return false;
	}
	out = RowToUser(mat, 0);
	return true;
}

bool SetAxLevel(Connection &con, const std::string &username, int64_t axlevel, std::string &err) {
	if (axlevel < 0 || axlevel > 6) {
		err = "access level must be 0-6";
		return false;
	}
	// GetOrAssignUserNum both validates that this is a real local user and
	// materializes the citadel_users row the UPDATE needs.
	if (GetOrAssignUserNum(con, username) <= 0) {
		err = "no such user";
		return false;
	}
	auto r = ExecP(con, "UPDATE citadel_users SET axlevel = $2 WHERE username = $1",
	               {Value(username), Value::BIGINT(axlevel)});
	if (!r) {
		err = "update failed";
		return false;
	}
	return true;
}

bool SetUserFlags(Connection &con, const std::string &username, int64_t flags) {
	if (GetOrAssignUserNum(con, username) <= 0) {
		return false;
	}
	return ExecP(con, "UPDATE citadel_users SET flags = $2 WHERE username = $1",
	             {Value(username), Value::BIGINT(flags)}) != nullptr;
}

bool SetScreenSize(Connection &con, const std::string &username, int64_t width, int64_t height) {
	if (GetOrAssignUserNum(con, username) <= 0) {
		return false;
	}
	// Clamp to something a terminal could plausibly be, so a bogus NAWS
	// subnegotiation cannot make the pager divide oddly or wrap at zero.
	width = std::max<int64_t>(20, std::min<int64_t>(width, 1000));
	height = std::max<int64_t>(5, std::min<int64_t>(height, 500));
	return ExecP(con, "UPDATE citadel_users SET screenwidth = $2, screenheight = $3 WHERE username = $1",
	             {Value(username), Value::BIGINT(width), Value::BIGINT(height)}) != nullptr;
}

void RecordCall(Connection &con, const std::string &username) {
	if (GetOrAssignUserNum(con, username) <= 0) {
		return;
	}
	ExecP(con, "UPDATE citadel_users SET times_called = times_called + 1, last_call = now() "
	           "WHERE username = $1",
	      {Value(username)});
}

bool GetRegistration(Connection &con, const std::string &username, Registration &out) {
	auto r = ExecP(con,
	               "SELECT real_name, street, city, state, zipcode, phone, email, country, bio "
	               "FROM citadel_user_reg WHERE username = $1",
	               {Value(username)});
	if (!r) {
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return false;
	}
	out.real_name = AsString(mat.GetValue(0, 0));
	out.street = AsString(mat.GetValue(1, 0));
	out.city = AsString(mat.GetValue(2, 0));
	out.state = AsString(mat.GetValue(3, 0));
	out.zipcode = AsString(mat.GetValue(4, 0));
	out.phone = AsString(mat.GetValue(5, 0));
	out.email = AsString(mat.GetValue(6, 0));
	out.country = AsString(mat.GetValue(7, 0));
	out.bio = AsString(mat.GetValue(8, 0));
	return true;
}

bool SetRegistration(Connection &con, const std::string &username, const Registration &reg) {
	if (GetOrAssignUserNum(con, username) <= 0) {
		return false;
	}
	auto r = ExecP(con,
	               "INSERT INTO citadel_user_reg "
	               "(username, real_name, street, city, state, zipcode, phone, email, country, bio) "
	               "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10) "
	               "ON CONFLICT (username) DO UPDATE SET real_name = excluded.real_name, "
	               "street = excluded.street, city = excluded.city, state = excluded.state, "
	               "zipcode = excluded.zipcode, phone = excluded.phone, email = excluded.email, "
	               "country = excluded.country, bio = excluded.bio",
	               {Value(username), Value(reg.real_name), Value(reg.street), Value(reg.city), Value(reg.state),
	                Value(reg.zipcode), Value(reg.phone), Value(reg.email), Value(reg.country), Value(reg.bio)});
	if (!r) {
		return false;
	}
	// Filling in registration is what sets US_REGIS, exactly as cmd_regi does.
	UserInfo u;
	if (GetUser(con, username, u)) {
		SetUserFlags(con, username, u.flags | US_REGIS);
	}
	return true;
}

bool SetBio(Connection &con, const std::string &username, const std::string &bio) {
	if (GetOrAssignUserNum(con, username) <= 0) {
		return false;
	}
	return ExecP(con,
	             "INSERT INTO citadel_user_reg (username, bio) VALUES ($1, $2) "
	             "ON CONFLICT (username) DO UPDATE SET bio = excluded.bio",
	             {Value(username), Value(bio)}) != nullptr;
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

bool GetFloor(Connection &con, int64_t floor_num, Floor &out) {
	auto r = ExecP(con,
	               "SELECT f.floor_num, f.name, "
	               "(SELECT count(*) FROM citadel_rooms r WHERE r.floor_num = f.floor_num) "
	               "FROM citadel_floors f WHERE f.floor_num = $1",
	               {Value::BIGINT(floor_num)});
	if (!r) {
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return false;
	}
	out.floor_num = AsBigint(mat.GetValue(0, 0));
	out.name = AsString(mat.GetValue(1, 0));
	out.room_count = AsBigint(mat.GetValue(2, 0));
	return true;
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

bool RenameFloor(Connection &con, int64_t floor_num, const std::string &name, std::string &err) {
	if (name.empty()) {
		err = "floor name may not be empty";
		return false;
	}
	Floor f;
	if (!GetFloor(con, floor_num, f)) {
		err = "no such floor";
		return false;
	}
	if (!ExecP(con, "UPDATE citadel_floors SET name = $2 WHERE floor_num = $1",
	           {Value::BIGINT(floor_num), Value(name)})) {
		err = "update failed";
		return false;
	}
	return true;
}

bool KillFloor(Connection &con, int64_t floor_num, std::string &err) {
	if (floor_num == 0) {
		err = "cannot delete the main floor";
		return false;
	}
	Floor f;
	if (!GetFloor(con, floor_num, f)) {
		err = "no such floor";
		return false;
	}
	// Citadel refuses to delete a floor that still holds rooms rather than
	// silently orphaning them onto floor 0.
	if (f.room_count > 0) {
		err = "floor still contains rooms";
		return false;
	}
	if (!ExecP(con, "DELETE FROM citadel_floors WHERE floor_num = $1", {Value::BIGINT(floor_num)})) {
		err = "delete failed";
		return false;
	}
	return true;
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
	bool is_aide = !username.empty() && GetAxLevel(con, username) >= kAideAxLevel;
	bool want_zapped = (which == "zapped");

	std::string sql = std::string("SELECT ") + kRoomCols +
	                  " FROM citadel_rooms r WHERE (r.mailbox_owner = 0 OR r.mailbox_owner = $1)";
	if (!is_aide) {
		// Hide other people's private rooms from non-aides, but always show a
		// user their own mailbox rooms (which are themselves flagged private).
		sql += " AND ((r.qr_flags & 4) = 0 OR r.mailbox_owner = $1";
		if (!username.empty()) {
			// ...and show an invitation-only room to anyone the access list has
			// actually invited. `l` is RFC 4314's lookup right — literally "this
			// mailbox is visible to you" — so honouring it here is what makes a
			// private room joinable by grant rather than only by an aide's
			// intervention or by guessing its name.
			sql += " OR EXISTS (SELECT 1 FROM citadel_room_acl a WHERE a.room_num = r.room_num "
			       "AND lower(a.identifier) IN (lower($2), 'anyone') AND a.rights LIKE '%l%')";
		}
		sql += ")";
	}
	// A zapped room is one the user has forgotten: it drops out of every listing
	// except the one that exists to show them.
	if (!username.empty()) {
		sql += want_zapped ? " AND EXISTS (" : " AND NOT EXISTS (";
		sql += "SELECT 1 FROM citadel_room_state s WHERE s.username = $2 AND s.room_num = r.room_num "
		       "AND (s.flags & 1) <> 0)";
	} else if (want_zapped) {
		return out; // nobody to have zapped anything
	}
	if (floor >= 0) {
		sql += username.empty() ? " AND r.floor_num = $2" : " AND r.floor_num = $3";
	}
	// Ordered by the internal key, which is what a real Citadel server does:
	// cmd_lkra simply walks the room database, so rooms come back in key order.
	// Personal rooms are keyed "<usernum zero-padded to 10>.<name>", so digits
	// sort ahead of letters and every mailbox lands before the public rooms —
	// matching the oracle's LKRA exactly for both a plain user and an aide.
	// listorder is deliberately not used here: the real server ignores it when
	// listing, and clients that care do their own grouping by floor.
	sql += " ORDER BY r.name";

	duckdb::vector<Value> params = {Value::BIGINT(usernum)};
	if (!username.empty()) {
		params.push_back(Value(username));
	}
	if (floor >= 0) {
		params.push_back(Value::BIGINT(floor));
	}
	auto r = ExecP(con, sql, params);
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	std::vector<Room> rooms;
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		rooms.push_back(RowToRoom(mat, i));
	}
	if (which != "new" && which != "old") {
		return rooms;
	}
	// One stats query for the whole list rather than three or four per room.
	std::vector<int64_t> nums;
	nums.reserve(rooms.size());
	for (auto &room : rooms) {
		nums.push_back(room.room_num);
	}
	auto stats = RoomStatsBulk(con, username, nums);
	for (size_t i = 0; i < rooms.size(); i++) {
		bool has_new = stats[i].new_count > 0;
		if ((which == "new") == has_new) {
			out.push_back(std::move(rooms[i]));
		}
	}
	return out;
}

bool IsReservedRoomName(const std::string &display_name) {
	// The shape MailboxRoomName builds: ten digits, a dot, then anything. Only
	// the prefix matters — what follows is the personal room's own name.
	if (display_name.size() < 12) {
		return false;
	}
	for (size_t i = 0; i < 10; i++) {
		if (!std::isdigit((unsigned char)display_name[i])) {
			return false;
		}
	}
	return display_name[10] == '.';
}

int64_t CreateRoom(Connection &con, const std::string &display_name, int64_t floor, int64_t qr_flags,
                   const std::string &password, int64_t mailbox_owner, std::string &err) {
	if (display_name.empty()) {
		err = "room name may not be empty";
		return -1;
	}
	// Only public rooms are affected: a personal room's key is built from its
	// owner's number, so its display name cannot collide with anything.
	if (mailbox_owner == 0 && IsReservedRoomName(display_name)) {
		err = "that name is reserved for personal mailboxes";
		return -1;
	}
	auto num = ScalarP(con, "SELECT nextval('citadel_room_seq')", {});
	if (num.IsNull()) {
		err = "could not allocate room number";
		return -1;
	}
	std::string internal = mailbox_owner > 0 ? MailboxRoomName(mailbox_owner, display_name) : display_name;
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

bool UpdateRoom(Connection &con, const Room &room, std::string &err) {
	Room existing;
	if (!GetRoomByNum(con, room.room_num, existing)) {
		err = "no such room";
		return false;
	}
	if (room.display_name.empty()) {
		err = "room name may not be empty";
		return false;
	}
	if (existing.mailbox_owner == 0 && IsReservedRoomName(room.display_name)) {
		err = "that name is reserved for personal mailboxes";
		return false;
	}
	if (room.floor_num != existing.floor_num) {
		Floor f;
		if (!GetFloor(con, room.floor_num, f)) {
			err = "no such floor";
			return false;
		}
	}
	// The internal key is derived, never edited directly: personal rooms keep
	// Citadel's zero-padded "<usernum>.<room>" form, public rooms are named by
	// their display name.
	std::string internal = existing.mailbox_owner > 0
	                           ? MailboxRoomName(existing.mailbox_owner, room.display_name)
	                           : room.display_name;
	auto r = ExecP(con,
	               "UPDATE citadel_rooms SET name = $2, display_name = $3, floor_num = $4, qr_flags = $5, "
	               "password = $6, listorder = $7, default_view = $8, info = $9 WHERE room_num = $1",
	               {Value::BIGINT(room.room_num), Value(internal), Value(room.display_name),
	                Value::BIGINT(room.floor_num), Value::BIGINT(room.qr_flags), Value(room.password),
	                Value::BIGINT(room.listorder), Value::BIGINT(room.default_view), Value(room.info)});
	if (!r) {
		err = "a room with that name already exists";
		return false;
	}
	return true;
}

namespace {

// The room-deleted hook registry. Guarded because EnsureSchema runs on every
// session and sessions are threads.
std::mutex &HookMutex() {
	static std::mutex m;
	return m;
}

std::vector<std::pair<std::string, RoomDeletedHook>> &Hooks() {
	static std::vector<std::pair<std::string, RoomDeletedHook>> hooks;
	return hooks;
}

} // namespace

void RegisterRoomDeletedHook(const std::string &name, RoomDeletedHook hook) {
	std::lock_guard<std::mutex> lock(HookMutex());
	for (auto &h : Hooks()) {
		if (h.first == name) {
			h.second = hook;
			return;
		}
	}
	Hooks().emplace_back(name, hook);
}

bool KillRoom(Connection &con, int64_t room_num, std::string &err) {
	if (room_num == kLobbyRoom) {
		err = "cannot delete the Lobby";
		return false;
	}
	if (room_num == kAideRoom) {
		// PostAideMessage writes here unconditionally, so losing it would break
		// the server's own log channel rather than just removing a room.
		err = "cannot delete the Aide room";
		return false;
	}
	// Everything keyed by this room goes before the room itself, children first.
	// What the layers above hung off it is theirs to remove, so they get asked
	// rather than having their table names written in here — see
	// RegisterRoomDeletedHook. Copy the list out from under the lock so a hook is
	// free to register another one.
	std::vector<std::pair<std::string, RoomDeletedHook>> hooks;
	{
		std::lock_guard<std::mutex> lock(HookMutex());
		hooks = Hooks();
	}
	for (auto &h : hooks) {
		h.second(con, room_num);
	}
	ExecP(con, "DELETE FROM citadel_room_msgs WHERE room_num = $1", {Value::BIGINT(room_num)});
	ExecP(con, "DELETE FROM citadel_room_state WHERE room_num = $1", {Value::BIGINT(room_num)});
	ExecP(con, "DELETE FROM citadel_room_acl WHERE room_num = $1", {Value::BIGINT(room_num)});
	// No tombstones either: there is no collection left for anyone to synchronize
	// against, and the room number is reusable.
	ExecP(con, "DELETE FROM citadel_room_tombstones WHERE room_num = $1", {Value::BIGINT(room_num)});
	ExecP(con, "DELETE FROM citadel_dav_names WHERE room_num = $1", {Value::BIGINT(room_num)});
	auto r = ExecP(con, "DELETE FROM citadel_rooms WHERE room_num = $1", {Value::BIGINT(room_num)});
	if (!r) {
		err = "delete failed";
		return false;
	}
	return true;
}

int64_t FindUserRoom(Connection &con, const std::string &username, const std::string &display_name) {
	int64_t usernum = GetOrAssignUserNum(con, username);
	if (usernum <= 0) {
		return -1;
	}
	// Match on the internal key, which is scoped to the user — looking the name
	// up any other way can collide with a public room that happens to share it.
	auto existing = ScalarP(con, "SELECT room_num FROM citadel_rooms WHERE lower(name) = lower($1)",
	                        {Value(MailboxRoomName(usernum, display_name))});
	return existing.IsNull() ? -1 : existing.GetValue<int64_t>();
}

int64_t GetOrCreateUserRoom(Connection &con, const std::string &username, const std::string &display_name) {
	int64_t usernum = GetOrAssignUserNum(con, username);
	if (usernum <= 0) {
		return -1;
	}
	std::string internal = MailboxRoomName(usernum, display_name);
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

namespace {

// is_valid_newsgroup_name() from Citadel's serv_nntp.c.
bool IsValidNewsgroupName(const std::string &name) {
	if (name.size() >= 5 && strncasecmp(name.c_str(), "ctdl.", 5) == 0) {
		return false;
	}
	bool has_letter = false;
	int dots = 0;
	for (unsigned char c : name) {
		if (std::isalpha(c)) {
			has_letter = true;
		}
		if (c == '.') {
			++dots;
		}
		if (!(std::isalnum(c) || c == '.' || c == '+' || c == '-')) {
			return false;
		}
	}
	return has_letter && dots >= 1;
}

} // namespace

std::string RoomToNewsgroup(const std::string &room_name) {
	if (IsValidNewsgroupName(room_name)) {
		return room_name;
	}
	std::string out = "ctdl.";
	char hex[8];
	for (unsigned char c : room_name) {
		if (std::isalnum(c) || c == '.' || c == '-') {
			out += (char)std::tolower(c);
		} else {
			std::snprintf(hex, sizeof hex, "+%02x", c);
			out += hex;
		}
	}
	return out;
}

std::string NewsgroupToRoom(const std::string &newsgroup) {
	if (newsgroup.size() < 5 || strncasecmp(newsgroup.c_str(), "ctdl.", 5) != 0) {
		return newsgroup; // not a converted name; pass through as-is
	}
	std::string out;
	for (size_t i = 5; i < newsgroup.size(); i++) {
		if (newsgroup[i] == '+' && i + 2 < newsgroup.size()) {
			out += (char)std::strtol(newsgroup.substr(i + 1, 2).c_str(), nullptr, 16);
			i += 2;
		} else {
			out += newsgroup[i];
		}
	}
	return out;
}

int64_t RegisterSession(Connection &con, const std::string &client, const std::string &host) {
	auto sid = ScalarP(con, "SELECT nextval('citadel_session_seq')", {});
	if (sid.IsNull()) {
		return 0;
	}
	int64_t id = sid.GetValue<int64_t>();
	// A loopback peer is reported as "localhost", which is the name a real
	// Citadel server puts in RWHO for a local connection.
	std::string where = host;
	if (where == "127.0.0.1" || where == "::1" || where == "::ffff:127.0.0.1") {
		where = "localhost";
	}
	ExecP(con,
	      "INSERT INTO citadel_sessions (session_id, client, host, since, last_seen) "
	      "VALUES ($1, $2, $3, $4, $4)",
	      {Value::BIGINT(id), Value(client), Value(where), Value::BIGINT(NowEpoch())});
	return id;
}

void TouchSession(Connection &con, int64_t session_id, const std::string &username, const std::string &room,
                  const std::string &last_cmd, int64_t axlevel) {
	if (session_id == 0) {
		return;
	}
	ExecP(con,
	      "UPDATE citadel_sessions SET username=$1, room=$2, last_cmd=$3, axlevel=$4, last_seen=$5 "
	      "WHERE session_id=$6",
	      {Value(username), Value(room), Value(last_cmd), Value::BIGINT(axlevel), Value::BIGINT(NowEpoch()),
	       Value::BIGINT(session_id)});
}

void UnregisterSession(Connection &con, int64_t session_id) {
	if (session_id == 0) {
		return;
	}
	ExecP(con, "DELETE FROM citadel_sessions WHERE session_id=$1", {Value::BIGINT(session_id)});
}

std::vector<SessionInfo> ListSessions(Connection &con) {
	std::vector<SessionInfo> out;
	auto r = con.Query("SELECT session_id, username, host, room, last_cmd, client, axlevel, since, last_seen "
	                   "FROM citadel_sessions ORDER BY session_id");
	if (r->HasError()) {
		return out;
	}
	for (idx_t i = 0; i < r->RowCount(); i++) {
		SessionInfo s;
		s.session_id = AsBigint(r->GetValue(0, i));
		s.username = r->GetValue(1, i).IsNull() ? "" : r->GetValue(1, i).ToString();
		s.host = r->GetValue(2, i).IsNull() ? "" : r->GetValue(2, i).ToString();
		s.room = r->GetValue(3, i).IsNull() ? "" : r->GetValue(3, i).ToString();
		s.last_cmd = r->GetValue(4, i).IsNull() ? "" : r->GetValue(4, i).ToString();
		s.client = r->GetValue(5, i).IsNull() ? "" : r->GetValue(5, i).ToString();
		s.axlevel = AsBigint(r->GetValue(6, i));
		s.since = AsBigint(r->GetValue(7, i));
		s.last_seen = AsBigint(r->GetValue(8, i));
		out.push_back(std::move(s));
	}
	return out;
}

bool SendExpress(Connection &con, const std::string &to, const std::string &from, const std::string &text) {
	if (to.empty() || GetOrAssignUserNum(con, to) <= 0) {
		return false;
	}
	auto id = ScalarP(con, "SELECT nextval('citadel_express_seq')", {});
	if (id.IsNull()) {
		return false;
	}
	ExecP(con,
	      "INSERT INTO citadel_express (id, to_user, from_user, text, sent_at) VALUES ($1, $2, $3, $4, $5)",
	      {id, Value(to), Value(from), Value(text), Value::BIGINT(NowEpoch())});
	return true;
}

std::vector<Express> PendingExpress(Connection &con, const std::string &user) {
	std::vector<Express> out;
	if (user.empty()) {
		return out;
	}
	auto r = ExecP(con,
	               "SELECT id, from_user, text, sent_at FROM citadel_express "
	               "WHERE lower(to_user)=lower($1) AND delivered=false ORDER BY id",
	               {Value(user)});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		Express e;
		e.id = AsBigint(mat.GetValue(0, i));
		e.from_user = mat.GetValue(1, i).ToString();
		e.text = mat.GetValue(2, i).ToString();
		e.sent_at = AsBigint(mat.GetValue(3, i));
		out.push_back(std::move(e));
	}
	return out;
}

void MarkExpressDelivered(Connection &con, int64_t id) {
	ExecP(con, "UPDATE citadel_express SET delivered=true WHERE id=$1", {Value::BIGINT(id)});
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

std::vector<RoomStats> RoomStatsBulk(Connection &con, const std::string &username,
                                     const std::vector<int64_t> &room_nums) {
	std::vector<RoomStats> out(room_nums.size());
	if (room_nums.empty()) {
		return out;
	}
	// The room numbers come from our own tables, never from a client, so
	// inlining them is safe — and it keeps this to one query instead of the
	// three or four per room GetRoomStats would cost.
	std::string in_list;
	for (size_t i = 0; i < room_nums.size(); i++) {
		if (i) {
			in_list += ",";
		}
		in_list += std::to_string(room_nums[i]);
	}
	std::string sql =
	    "WITH lr AS (SELECT room_num, last_read FROM citadel_room_state WHERE username = $1) "
	    "SELECT r.room_num, coalesce(lr.last_read, 0), "
	    "(SELECT count(*) FROM citadel_room_msgs m WHERE m.room_num = r.room_num), "
	    "(SELECT coalesce(max(m.msgnum), 0) FROM citadel_room_msgs m WHERE m.room_num = r.room_num), "
	    "(SELECT count(*) FROM citadel_room_msgs m WHERE m.room_num = r.room_num "
	    " AND m.msgnum > coalesce(lr.last_read, 0)) "
	    "FROM citadel_rooms r LEFT JOIN lr ON lr.room_num = r.room_num "
	    "WHERE r.room_num IN (" +
	    in_list + ")";

	auto r = ExecP(con, sql, {Value(username)});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	// Index the result by room number: the IN list does not preserve our order,
	// and a room could have been deleted between listing and counting.
	std::vector<std::pair<int64_t, RoomStats>> rows;
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		RoomStats s;
		int64_t num = AsBigint(mat.GetValue(0, i));
		s.last_read = AsBigint(mat.GetValue(1, i));
		s.total = AsBigint(mat.GetValue(2, i));
		s.highest = AsBigint(mat.GetValue(3, i));
		s.new_count = AsBigint(mat.GetValue(4, i));
		rows.emplace_back(num, s);
	}
	for (size_t i = 0; i < room_nums.size(); i++) {
		for (auto &row : rows) {
			if (row.first == room_nums[i]) {
				out[i] = row.second;
				break;
			}
		}
	}
	return out;
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

bool ZapRoom(Connection &con, const std::string &username, int64_t room_num, bool zapped, std::string &err) {
	if (username.empty()) {
		err = "not logged in";
		return false;
	}
	if (zapped && room_num == kLobbyRoom) {
		// Citadel refuses this: the Lobby is where <G>oto lands when nothing
		// else has unread messages, so forgetting it would strand the user.
		err = "you cannot zap the Lobby";
		return false;
	}
	auto r = ExecP(con,
	               "INSERT INTO citadel_room_state (username, room_num, flags) VALUES ($1, $2, $3) "
	               "ON CONFLICT (username, room_num) DO UPDATE SET flags = "
	               "CASE WHEN $3 <> 0 THEN citadel_room_state.flags | 1 ELSE citadel_room_state.flags & ~1 END",
	               {Value(username), Value::BIGINT(room_num), Value::BIGINT(zapped ? RS_ZAPPED : 0)});
	if (!r) {
		err = "could not update room state";
		return false;
	}
	return true;
}

bool IsZapped(Connection &con, const std::string &username, int64_t room_num) {
	if (username.empty()) {
		return false;
	}
	auto v = ScalarP(con, "SELECT flags FROM citadel_room_state WHERE username = $1 AND room_num = $2",
	                 {Value(username), Value::BIGINT(room_num)});
	return (AsBigint(v) & RS_ZAPPED) != 0;
}

bool RoomUnlocked(Connection &con, const std::string &username, const Room &room) {
	if (!(room.qr_flags & QR_PASSWORDED) || room.password.empty()) {
		return true;
	}
	if (username.empty()) {
		return false;
	}
	// The owner of a personal room never needs its password.
	if (room.mailbox_owner > 0 && room.mailbox_owner == GetOrAssignUserNum(con, username)) {
		return true;
	}
	auto v = ScalarP(con, "SELECT flags FROM citadel_room_state WHERE username = $1 AND room_num = $2",
	                 {Value(username), Value::BIGINT(room.room_num)});
	return (AsBigint(v) & RS_UNLOCKED) != 0;
}

// ---- access control lists (RFC 4314) ------------------------------------

const char *const kAclRights = "lrswipkxtea";

// Union two rights strings, emitting them in kAclRights order with no repeats.
static std::string UnionRights(const std::string &a, const std::string &b) {
	std::string out;
	for (const char *p = kAclRights; *p; p++) {
		if (a.find(*p) != std::string::npos || b.find(*p) != std::string::npos) {
			out += *p;
		}
	}
	return out;
}

static std::string StoredRights(Connection &con, int64_t room_num, const std::string &identifier) {
	auto v = ScalarP(con,
	                 "SELECT rights FROM citadel_room_acl WHERE room_num = $1 AND lower(identifier) = lower($2)",
	                 {Value::BIGINT(room_num), Value(identifier)});
	return v.IsNull() ? std::string() : v.ToString();
}

// The RFC 4314 identifier that covers every caller, authenticated or not.
static bool IsAnyone(const std::string &identifier) {
	if (identifier.size() != 6) {
		return false;
	}
	for (size_t i = 0; i < 6; i++) {
		if (std::tolower((unsigned char)identifier[i]) != "anyone"[i]) {
			return false;
		}
	}
	return true;
}

std::string EffectiveRights(Connection &con, const std::string &username, const Room &room) {
	std::string derived;
	bool anonymous = username.empty() || IsAnyone(username);
	if (!anonymous) {
		int64_t usernum = GetOrAssignUserNum(con, username);
		bool owner = room.mailbox_owner > 0 && room.mailbox_owner == usernum;
		if (owner || GetAxLevel(con, username) >= kAideAxLevel) {
			// The owner of a mailbox, and any aide, hold every right. This is
			// also what lets an aide post into a QR_READONLY announcement room.
			derived = kAclRights;
		} else if (room.mailbox_owner > 0 || (room.qr_flags & QR_PRIVATE) ||
		           !RoomUnlocked(con, username, room)) {
			// Someone else's mailbox, an invitation-only room, or a passworded
			// room whose password has not been given: not even visible.
			derived.clear();
		} else {
			derived = "lrs";
			if (!(room.qr_flags & QR_READONLY)) {
				derived += "wi";
			}
		}
	}
	// Stored grants are additive. "anyone" applies to every caller; a row naming
	// this user adds to it.
	std::string stored = StoredRights(con, room.room_num, "anyone");
	if (!anonymous) {
		stored = UnionRights(stored, StoredRights(con, room.room_num, username));
	}
	return UnionRights(derived, stored);
}

bool SetRights(Connection &con, const Room &room, const std::string &identifier,
               const std::string &rights, std::string &err) {
	if (identifier.empty()) {
		err = "an ACL entry needs an identifier";
		return false;
	}
	for (char c : rights) {
		if (std::strchr(kAclRights, c) == nullptr) {
			err = std::string("unknown right '") + c + "'";
			return false;
		}
	}
	if (rights.empty()) {
		ExecP(con, "DELETE FROM citadel_room_acl WHERE room_num = $1 AND lower(identifier) = lower($2)",
		      {Value::BIGINT(room.room_num), Value(identifier)});
		return true;
	}
	// Normalize to kAclRights order so GETACL output does not depend on the
	// order the client happened to type the letters in.
	std::string normalized = UnionRights(rights, "");
	if (!ExecP(con,
	           "INSERT INTO citadel_room_acl (room_num, identifier, rights) VALUES ($1, $2, $3) "
	           "ON CONFLICT (room_num, identifier) DO UPDATE SET rights = excluded.rights",
	           {Value::BIGINT(room.room_num), Value(identifier), Value(normalized)})) {
		err = "could not store the access control entry";
		return false;
	}
	return true;
}

std::vector<std::pair<std::string, std::string>> ListRights(Connection &con, const Room &room) {
	std::vector<std::pair<std::string, std::string>> out;
	auto r = ExecP(con,
	               "SELECT identifier, rights FROM citadel_room_acl WHERE room_num = $1 ORDER BY identifier",
	               {Value::BIGINT(room.room_num)});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		out.push_back({mat.GetValue(0, i).ToString(), mat.GetValue(1, i).ToString()});
	}
	return out;
}

bool CanPost(Connection &con, const std::string &username, const Room &room) {
	std::string rights = EffectiveRights(con, username, room);
	char needed = username.empty() ? 'p' : 'i';
	return rights.find(needed) != std::string::npos;
}

bool CanAdminister(Connection &con, const std::string &username, const Room &room) {
	if (username.empty()) {
		return false; // an anonymous gateway never administers anything
	}
	return EffectiveRights(con, username, room).find('a') != std::string::npos;
}

int64_t ResolveMailRoom(Connection &con, const std::string &local_part) {
	static const char kPrefix[] = "room_";
	const size_t kPrefixLen = sizeof(kPrefix) - 1;
	if (local_part.size() <= kPrefixLen) {
		return -1;
	}
	for (size_t i = 0; i < kPrefixLen; i++) {
		if (std::tolower((unsigned char)local_part[i]) != kPrefix[i]) {
			return -1;
		}
	}
	std::string rest = local_part.substr(kPrefixLen);
	Room room;
	// Citadel writes a room's spaces as underscores in an address, but a room
	// name may contain a literal underscore, so try the name as given first.
	bool found = ResolveRoom(con, "", rest, room);
	if (!found) {
		std::replace(rest.begin(), rest.end(), '_', ' ');
		found = ResolveRoom(con, "", rest, room);
	}
	if (!found) {
		return -1;
	}
	// Belt and braces: ResolveRoom with no user only returns public rooms, but
	// a personal, invitation-only or passworded room must never be mail-reachable
	// even if someone manages to put a grant on it.
	if (room.mailbox_owner > 0 || (room.qr_flags & (QR_MAILBOX | QR_PRIVATE | QR_PASSWORDED))) {
		return -1;
	}
	return CanPost(con, "", room) ? room.room_num : -1;
}

bool UnlockRoom(Connection &con, const std::string &username, const Room &room,
                const std::string &password) {
	if (username.empty()) {
		return false;
	}
	if (!(room.qr_flags & QR_PASSWORDED) || room.password.empty()) {
		return true;
	}
	if (password != room.password) {
		return false;
	}
	return ExecP(con,
	             "INSERT INTO citadel_room_state (username, room_num, flags) VALUES ($1, $2, 2) "
	             "ON CONFLICT (username, room_num) DO UPDATE SET flags = citadel_room_state.flags | 2",
	             {Value(username), Value::BIGINT(room.room_num)}) != nullptr;
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

bool MessageInRoom(Connection &con, int64_t room_num, int64_t msgnum) {
	auto v = ScalarP(con, "SELECT 1 FROM citadel_room_msgs WHERE room_num = $1 AND msgnum = $2",
	                 {Value::BIGINT(room_num), Value::BIGINT(msgnum)});
	return !v.IsNull();
}

bool DeleteMessage(Connection &con, int64_t room_num, int64_t msgnum, std::string &err) {
	if (!MessageInRoom(con, room_num, msgnum)) {
		err = "message not found in this room";
		return false;
	}
	// The euid is read before the unlink, because the tombstone is keyed by it:
	// a synchronizing client asks about resources, and a resource is an euid.
	std::string euid;
	auto euid_v = ScalarP(con, "SELECT euid FROM citadel_messages WHERE msgnum = $1", {Value::BIGINT(msgnum)});
	if (!euid_v.IsNull()) {
		euid = AsString(euid_v);
	}

	// Only the room pointer goes: the message row may still be pointed into
	// other rooms (a mail message lives in both Mail and Sent Items).
	if (!ExecP(con, "DELETE FROM citadel_room_msgs WHERE room_num = $1 AND msgnum = $2",
	           {Value::BIGINT(room_num), Value::BIGINT(msgnum)})) {
		err = "delete failed";
		return false;
	}

	// Recorded after the unlink succeeds, so a tombstone never claims a removal
	// that did not happen. The reverse failure — an unlink with no tombstone — is
	// the recoverable one: a client re-reads the collection and notices.
	auto seq_v = ScalarP(con, "SELECT nextval('citadel_msg_seq')", {});
	if (!seq_v.IsNull()) {
		ExecP(con,
		      "INSERT OR REPLACE INTO citadel_room_tombstones (room_num, seq, euid, msgnum, deleted_at) "
		      "VALUES ($1, $2, $3, $4, $5)",
		      {Value::BIGINT(room_num), Value::BIGINT(seq_v.GetValue<int64_t>()), Value(euid),
		       Value::BIGINT(msgnum), Value::BIGINT((int64_t)std::time(nullptr))});
	}
	return true;
}

int64_t RoomChangeToken(Connection &con, int64_t room_num) {
	auto v = ScalarP(con,
	                 "SELECT greatest("
	                 "  coalesce((SELECT max(msgnum) FROM citadel_room_msgs WHERE room_num = $1), 0), "
	                 "  coalesce((SELECT max(seq) FROM citadel_room_tombstones WHERE room_num = $1), 0))",
	                 {Value::BIGINT(room_num)});
	return v.IsNull() ? 0 : v.GetValue<int64_t>();
}

std::vector<RoomChange> RoomChangesSince(Connection &con, int64_t room_num, int64_t since) {
	std::vector<RoomChange> out;
	// Both halves in one ordered pass. A resource that was replaced shows up
	// twice — once as the new msgnum and once as the tombstone for the old one —
	// which is correct and why callers resolve the euid afterwards rather than
	// trusting `deleted` on its own.
	auto r = ExecP(con,
	               "SELECT seq, euid, msgnum, deleted FROM ("
	               "  SELECT m.msgnum AS seq, m.euid AS euid, m.msgnum AS msgnum, false AS deleted "
	               "    FROM citadel_room_msgs rm JOIN citadel_messages m ON m.msgnum = rm.msgnum "
	               "   WHERE rm.room_num = $1 AND m.msgnum > $2 "
	               "  UNION ALL "
	               "  SELECT seq, euid, msgnum, true FROM citadel_room_tombstones "
	               "   WHERE room_num = $1 AND seq > $2"
	               ") ORDER BY seq",
	               {Value::BIGINT(room_num), Value::BIGINT(since)});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		RoomChange c;
		c.seq = AsBigint(mat.GetValue(0, i));
		c.euid = AsString(mat.GetValue(1, i));
		c.msgnum = AsBigint(mat.GetValue(2, i));
		Value d = mat.GetValue(3, i);
		c.deleted = !d.IsNull() && d.GetValue<bool>();
		out.push_back(std::move(c));
	}
	return out;
}

void PruneTombstones(Connection &con, int64_t older_than_seconds) {
	if (older_than_seconds <= 0) {
		return;
	}
	int64_t cutoff = (int64_t)std::time(nullptr) - older_than_seconds;
	ExecP(con, "DELETE FROM citadel_room_tombstones WHERE deleted_at < $1", {Value::BIGINT(cutoff)});
}

void PruneDavNames(Connection &con) {
	con.Query("DELETE FROM citadel_dav_names d WHERE NOT EXISTS ("
	          "  SELECT 1 FROM citadel_room_msgs rm JOIN citadel_messages m ON m.msgnum = rm.msgnum"
	          "  WHERE rm.room_num = d.room_num AND m.euid = d.euid)");
}

int64_t FindByEuid(Connection &con, int64_t room_num, const std::string &euid) {
	if (euid.empty()) {
		return -1;
	}
	auto v = ScalarP(con,
	                 "SELECT m.msgnum FROM citadel_messages m "
	                 "JOIN citadel_room_msgs r ON r.msgnum = m.msgnum "
	                 "WHERE r.room_num = $1 AND m.euid = $2 "
	                 "ORDER BY m.msgnum DESC LIMIT 1",
	                 {Value::BIGINT(room_num), Value(euid)});
	return v.IsNull() ? -1 : v.GetValue<int64_t>();
}

int64_t UpsertByEuid(Connection &con, const Message &msg, int64_t room_num, std::string &err) {
	if (msg.euid.empty()) {
		// Refused rather than treated as an insert. An object with no euid
		// cannot be replaced, so accepting it here would quietly turn every
		// subsequent save into a duplicate.
		err = "a groupware object needs an euid";
		return -1;
	}

	int64_t old = FindByEuid(con, room_num, msg.euid);

	// InsertMessage runs its own transaction, so the replace cannot be wrapped
	// in an outer one without nesting. Insert first and unlink second: the
	// failure that leaves two copies of an object is recoverable and visible,
	// whereas unlinking first and then failing to insert would lose it.
	int64_t created = InsertMessage(con, msg, {room_num}, err);
	if (created < 0) {
		return -1;
	}
	if (old >= 0 && old != created) {
		std::string ignored;
		DeleteMessage(con, room_num, old, ignored);
	}
	return created;
}

bool MoveMessage(Connection &con, int64_t from_room, int64_t to_room, int64_t msgnum, bool is_copy,
                 std::string &err) {
	if (!MessageInRoom(con, from_room, msgnum)) {
		err = "message not found in this room";
		return false;
	}
	Room target;
	if (!GetRoomByNum(con, to_room, target)) {
		err = "no such target room";
		return false;
	}
	if (from_room == to_room) {
		return true; // nothing to do; a copy onto itself is a no-op
	}
	if (!ExecP(con, "INSERT OR IGNORE INTO citadel_room_msgs (room_num, msgnum) VALUES ($1, $2)",
	           {Value::BIGINT(to_room), Value::BIGINT(msgnum)})) {
		err = "insert failed";
		return false;
	}
	ExecP(con, "UPDATE citadel_rooms SET highest_msg = greatest(highest_msg, $2) WHERE room_num = $1",
	      {Value::BIGINT(to_room), Value::BIGINT(msgnum)});
	if (!is_copy) {
		if (!ExecP(con, "DELETE FROM citadel_room_msgs WHERE room_num = $1 AND msgnum = $2",
		           {Value::BIGINT(from_room), Value::BIGINT(msgnum)})) {
			err = "delete failed";
			return false;
		}
	}
	return true;
}

std::string GetUserPref(Connection &con, const std::string &username, const std::string &name,
                        const std::string &dflt) {
	if (username.empty()) {
		return dflt;
	}
	auto v = ScalarP(con,
	                 "SELECT value FROM citadel_user_prefs WHERE lower(username) = lower($1) AND name = $2",
	                 {Value(username), Value(name)});
	return v.IsNull() ? dflt : v.ToString();
}

bool SetUserPref(Connection &con, const std::string &username, const std::string &name,
                 const std::string &value) {
	if (username.empty()) {
		return false;
	}
	if (value.empty()) {
		// An empty value means "no preference", which must fall back to the site
		// default rather than pin the user to an empty string.
		ExecP(con, "DELETE FROM citadel_user_prefs WHERE lower(username) = lower($1) AND name = $2",
		      {Value(username), Value(name)});
		return true;
	}
	return ExecP(con,
	             "INSERT INTO citadel_user_prefs (username, name, value) VALUES ($1, $2, $3) "
	             "ON CONFLICT (username, name) DO UPDATE SET value = excluded.value",
	             {Value(username), Value(name), Value(value)}) != nullptr;
}

void PostAideMessage(Connection &con, const std::string &subject, const std::string &text) {
	if (GetConfig(con, "qm_aide_log", "1") != "1") {
		return;
	}
	Message msg;
	msg.author = GetConfig(con, "c_nodename", "quackcit");
	msg.msgtime = NowEpoch();
	msg.format_type = 0; // native Citadel text, so every front-end renders it
	msg.subject = subject;
	msg.origin_room = "Aide";
	msg.raw = text;
	std::string err;
	InsertMessage(con, msg, {kAideRoom}, err); // best effort; never fails the caller
}

} // namespace citadel
} // namespace quackmail
