#include "quackmail/listserv.hpp"

#include "quackmail/citadel_msg.hpp"
#include "quackmail/delivery.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/util.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>

namespace quackmail {
namespace listserv {

using duckdb::Connection;
using duckdb::idx_t;
using duckdb::MaterializedQueryResult;
using duckdb::QueryResult;
using duckdb::Value;

namespace {

// How long a confirmation token is good for. Long enough that a mailbox checked
// once a day still works, short enough that a leaked token goes stale.
constexpr int64_t kTokenTtlSecs = 7 * 24 * 3600;

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
	return mat.RowCount() < 1 ? Value() : mat.GetValue(0, 0);
}

std::string Str(const Value &v) {
	return v.IsNull() ? std::string() : v.ToString();
}

int64_t Int(const Value &v, int64_t dflt = 0) {
	return v.IsNull() ? dflt : v.GetValue<int64_t>();
}

bool Bool(const Value &v, bool dflt = false) {
	return v.IsNull() ? dflt : v.GetValue<bool>();
}

int64_t NowEpoch() {
	return (int64_t)std::time(nullptr);
}

std::string ModeName(Mode m) {
	switch (m) {
	case Mode::Digest:
		return "digest";
	case Mode::Both:
		return "both";
	default:
		return "post";
	}
}

Mode ParseMode(const std::string &s) {
	std::string v = util::Lower(s);
	if (v == "digest") {
		return Mode::Digest;
	}
	if (v == "both") {
		return Mode::Both;
	}
	return Mode::Post;
}

std::string PolicyName(PostPolicy p) {
	switch (p) {
	case PostPolicy::Anyone:
		return "anyone";
	case PostPolicy::Moderated:
		return "moderated";
	default:
		return "subscribers";
	}
}

PostPolicy ParsePolicy(const std::string &s) {
	std::string v = util::Lower(s);
	if (v == "anyone") {
		return PostPolicy::Anyone;
	}
	if (v == "moderated") {
		return PostPolicy::Moderated;
	}
	return PostPolicy::Subscribers;
}

std::string KindName(SubKind k) {
	return k == SubKind::Digest ? "digest" : "post";
}

std::string StateName(SubState s) {
	switch (s) {
	case SubState::Active:
		return "active";
	case SubState::UnsubPending:
		return "unsub_pending";
	default:
		return "pending";
	}
}

SubState ParseState(const std::string &s) {
	if (s == "active") {
		return SubState::Active;
	}
	if (s == "unsub_pending") {
		return SubState::UnsubPending;
	}
	return SubState::Pending;
}

// Addresses are compared and stored case-folded. The local part of an address
// is technically case sensitive, but no mail system on earth relies on that,
// and folding is what stops "Bob@x" subscribing twice as "bob@x".
std::string NormAddr(const std::string &addr) {
	std::string a = addr;
	// Tolerate "Display Name <addr@host>" and surrounding angle brackets.
	auto lt = a.find('<');
	auto gt = a.rfind('>');
	if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
		a = a.substr(lt + 1, gt - lt - 1);
	}
	while (!a.empty() && std::isspace((unsigned char)a.front())) {
		a.erase(a.begin());
	}
	while (!a.empty() && std::isspace((unsigned char)a.back())) {
		a.pop_back();
	}
	return util::Lower(a);
}

std::string Fqdn(Connection &con) {
	std::string f = citadel::GetConfig(con, "c_fqdn", "");
	return f.empty() ? "localhost" : f;
}

std::string NodeName(Connection &con) {
	return citadel::GetConfig(con, "c_nodename", "quackcit");
}

// ---- header surgery -----------------------------------------------------
//
// Everything below works on the raw RFC822 bytes rather than a parsed model,
// because the whole point is to hand subscribers the original message with a
// few headers changed — reserializing a parse would rewrite parts we have no
// business touching.

// Split at the header/body boundary. `headers` keeps its trailing CRLF.
void SplitMessage(const std::string &raw, std::string &headers, std::string &body) {
	size_t cut = raw.find("\r\n\r\n");
	size_t skip = 4;
	if (cut == std::string::npos) {
		cut = raw.find("\n\n");
		skip = 2;
	}
	if (cut == std::string::npos) {
		headers = raw;
		body.clear();
		return;
	}
	headers = raw.substr(0, cut + skip - 2);
	body = raw.substr(cut + skip);
}

bool HeaderNameIs(const std::string &line, const std::string &name) {
	if (line.size() < name.size() + 1) {
		return false;
	}
	for (size_t i = 0; i < name.size(); i++) {
		if (std::tolower((unsigned char)line[i]) != std::tolower((unsigned char)name[i])) {
			return false;
		}
	}
	return line[name.size()] == ':';
}

// Split a header block into whole fields, each including its continuation lines.
std::vector<std::string> SplitFields(const std::string &headers) {
	std::vector<std::string> fields;
	size_t i = 0;
	while (i < headers.size()) {
		size_t nl = headers.find('\n', i);
		std::string line = nl == std::string::npos ? headers.substr(i) : headers.substr(i, nl - i + 1);
		if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && !fields.empty()) {
			fields.back() += line; // folded continuation
		} else if (!line.empty() && line != "\r\n" && line != "\n") {
			fields.push_back(line);
		}
		if (nl == std::string::npos) {
			break;
		}
		i = nl + 1;
	}
	return fields;
}

std::string FieldValue(const std::string &field) {
	auto colon = field.find(':');
	if (colon == std::string::npos) {
		return "";
	}
	std::string v = field.substr(colon + 1);
	while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) {
		v.pop_back();
	}
	while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) {
		v.erase(v.begin());
	}
	return v;
}

std::string FindHeader(const std::vector<std::string> &fields, const std::string &name) {
	for (const auto &f : fields) {
		if (HeaderNameIs(f, name)) {
			return FieldValue(f);
		}
	}
	return "";
}

// A field re-emitted verbatim, with its line ending normalized to CRLF. Fields
// come out of SplitFields carrying whatever the source used, and a bare LF in
// an outgoing header is what makes some MTAs reject a message outright.
std::string CrlfField(const std::string &field) {
	std::string f = field;
	while (!f.empty() && (f.back() == '\r' || f.back() == '\n')) {
		f.pop_back();
	}
	return f + "\r\n";
}

} // namespace

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

void EnsureSchema(Connection &con) {
	con.Query("CREATE SEQUENCE IF NOT EXISTS citadel_list_held_seq START 1");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_lists (
			room_num             BIGINT PRIMARY KEY,
			address              VARCHAR,
			enabled              BOOLEAN DEFAULT true,
			mode                 VARCHAR DEFAULT 'post',
			post_policy          VARCHAR DEFAULT 'subscribers',
			reply_to_list        BOOLEAN DEFAULT false,
			subject_tag          VARCHAR DEFAULT '',
			footer               VARCHAR DEFAULT '',
			digest_interval_secs BIGINT DEFAULT 86400,
			digest_max           BIGINT DEFAULT 50,
			last_sent            BIGINT DEFAULT 0,
			last_digest          BIGINT DEFAULT 0,
			last_digest_at       BIGINT DEFAULT 0,
			created_at           TIMESTAMP DEFAULT now()
		)
	)");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_list_subs (
			room_num      BIGINT,
			address       VARCHAR,
			kind          VARCHAR DEFAULT 'post',
			state         VARCHAR DEFAULT 'pending',
			token         VARCHAR DEFAULT '',
			token_expires BIGINT DEFAULT 0,
			created_at    BIGINT DEFAULT 0,
			confirmed_at  BIGINT DEFAULT 0,
			PRIMARY KEY (room_num, address)
		)
	)");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_list_held (
			id          BIGINT DEFAULT nextval('citadel_list_held_seq'),
			room_num    BIGINT,
			mail_from   VARCHAR,
			subject     VARCHAR,
			raw         BLOB,
			received_at BIGINT DEFAULT 0,
			state       VARCHAR DEFAULT 'held'
		)
	)");
}

// ---------------------------------------------------------------------------
// Lists
// ---------------------------------------------------------------------------

namespace {

// Fill a List from a result row laid out in the SELECT order used below.
List RowToList(MaterializedQueryResult &mat, idx_t row) {
	List l;
	l.room_num = Int(mat.GetValue(0, row));
	l.address = Str(mat.GetValue(1, row));
	l.enabled = Bool(mat.GetValue(2, row), true);
	l.mode = ParseMode(Str(mat.GetValue(3, row)));
	l.post_policy = ParsePolicy(Str(mat.GetValue(4, row)));
	l.reply_to_list = Bool(mat.GetValue(5, row));
	l.subject_tag = Str(mat.GetValue(6, row));
	l.footer = Str(mat.GetValue(7, row));
	l.digest_interval_secs = Int(mat.GetValue(8, row), 86400);
	l.digest_max = Int(mat.GetValue(9, row), 50);
	l.last_sent = Int(mat.GetValue(10, row));
	l.last_digest = Int(mat.GetValue(11, row));
	l.last_digest_at = Int(mat.GetValue(12, row));
	return l;
}

const char *const kListColumns = "room_num, address, enabled, mode, post_policy, reply_to_list, "
                                 "subject_tag, footer, digest_interval_secs, digest_max, "
                                 "last_sent, last_digest, last_digest_at";

// The address a list defaults to when none was configured: the same
// room_<name> form citadel::ResolveMailRoom already accepts, so a list created
// on an already-mail-reachable room keeps working at the address people know.
std::string DefaultAddress(const citadel::Room &room) {
	std::string a = util::Lower(room.display_name);
	std::replace(a.begin(), a.end(), ' ', '_');
	return "room_" + a;
}

} // namespace

bool GetList(Connection &con, int64_t room_num, List &out) {
	auto r = ExecP(con, std::string("SELECT ") + kListColumns + " FROM citadel_lists WHERE room_num = $1",
	               {Value::BIGINT(room_num)});
	if (!r) {
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return false;
	}
	out = RowToList(mat, 0);
	citadel::Room room;
	if (citadel::GetRoomByNum(con, out.room_num, room)) {
		out.display_name = room.display_name;
	}
	return true;
}

std::vector<List> ListLists(Connection &con, bool enabled_only) {
	std::vector<List> out;
	std::string sql = std::string("SELECT ") + kListColumns + " FROM citadel_lists";
	if (enabled_only) {
		sql += " WHERE enabled";
	}
	sql += " ORDER BY room_num";
	auto r = con.Query(sql);
	if (!r || r->HasError()) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		List l = RowToList(mat, i);
		citadel::Room room;
		if (citadel::GetRoomByNum(con, l.room_num, room)) {
			l.display_name = room.display_name;
		}
		out.push_back(l);
	}
	return out;
}

bool SetList(Connection &con, const List &in, std::string &err) {
	List list = in;
	citadel::Room room;
	if (!citadel::GetRoomByNum(con, list.room_num, room)) {
		err = "no such room";
		return false;
	}
	// A personal or invitation-only room cannot be a list: the whole point is
	// that anyone may be handed its contents, which those flags say they may not.
	if (room.mailbox_owner > 0 ||
	    (room.qr_flags & (citadel::QR_MAILBOX | citadel::QR_PRIVATE | citadel::QR_PASSWORDED))) {
		err = "a mailbox, private or passworded room cannot be a mailing list";
		return false;
	}
	if (list.address.empty()) {
		list.address = DefaultAddress(room);
	}
	list.address = util::Lower(list.address);
	if (list.address.find('@') != std::string::npos) {
		err = "address must be a local part, without a domain";
		return false;
	}
	// The suffixes below are reserved, or a list called "x-bounces" would
	// intercept another list's bounce address.
	for (const char *suffix : {"-subscribe", "-unsubscribe", "-bounces", "-request", "-help"}) {
		std::string s = suffix;
		if (list.address.size() > s.size() &&
		    list.address.compare(list.address.size() - s.size(), s.size(), s) == 0) {
			err = "address may not end in '" + s + "' (reserved for list commands)";
			return false;
		}
	}
	auto clash = ScalarP(con, "SELECT count(*) FROM citadel_lists WHERE address = $1 AND room_num <> $2",
	                     {Value(list.address), Value::BIGINT(list.room_num)});
	if (Int(clash) > 0) {
		err = "another list already uses the address '" + list.address + "'";
		return false;
	}
	if (citadel::GetOrAssignUserNum(con, list.address) > 0) {
		err = "'" + list.address + "' is a local user";
		return false;
	}
	if (list.digest_interval_secs <= 0) {
		list.digest_interval_secs = 86400;
	}
	if (list.digest_max <= 0) {
		list.digest_max = 50;
	}

	bool exists = Int(ScalarP(con, "SELECT count(*) FROM citadel_lists WHERE room_num = $1",
	                          {Value::BIGINT(list.room_num)})) > 0;
	duckdb::vector<Value> params = {Value(list.address),
	                                Value::BOOLEAN(list.enabled),
	                                Value(ModeName(list.mode)),
	                                Value(PolicyName(list.post_policy)),
	                                Value::BOOLEAN(list.reply_to_list),
	                                Value(list.subject_tag),
	                                Value(list.footer),
	                                Value::BIGINT(list.digest_interval_secs),
	                                Value::BIGINT(list.digest_max),
	                                Value::BIGINT(list.room_num)};
	if (exists) {
		if (!ExecP(con,
		           "UPDATE citadel_lists SET address = $1, enabled = $2, mode = $3, post_policy = $4, "
		           "reply_to_list = $5, subject_tag = $6, footer = $7, digest_interval_secs = $8, "
		           "digest_max = $9 WHERE room_num = $10",
		           params)) {
			err = "could not update the list";
			return false;
		}
		return true;
	}
	// A brand new list starts its watermark at the room's current high-water
	// mark, so creating a list on a room with ten years of history does not mail
	// all of it to the first subscriber.
	params.push_back(Value::BIGINT(room.highest_msg));
	if (!ExecP(con,
	           "INSERT INTO citadel_lists (address, enabled, mode, post_policy, reply_to_list, "
	           "subject_tag, footer, digest_interval_secs, digest_max, room_num, last_sent, "
	           "last_digest, last_digest_at) "
	           "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $11, 0)",
	           params)) {
		err = "could not create the list";
		return false;
	}
	return true;
}

bool RemoveList(Connection &con, int64_t room_num, std::string &err) {
	if (!ExecP(con, "DELETE FROM citadel_lists WHERE room_num = $1", {Value::BIGINT(room_num)})) {
		err = "could not remove the list";
		return false;
	}
	ExecP(con, "DELETE FROM citadel_list_subs WHERE room_num = $1", {Value::BIGINT(room_num)});
	ExecP(con, "DELETE FROM citadel_list_held WHERE room_num = $1", {Value::BIGINT(room_num)});
	return true;
}

bool SetField(Connection &con, int64_t room_num, const std::string &key, const std::string &value,
              std::string &err) {
	List l;
	if (!GetList(con, room_num, l)) {
		err = "no such list";
		return false;
	}
	std::string k = util::Lower(key);
	auto truthy = [&]() {
		std::string v = util::Lower(value);
		return v == "1" || v == "true" || v == "yes" || v == "on";
	};
	if (k == "address") {
		l.address = value;
	} else if (k == "enabled") {
		l.enabled = truthy();
	} else if (k == "mode") {
		l.mode = ParseMode(value);
	} else if (k == "post_policy") {
		l.post_policy = ParsePolicy(value);
	} else if (k == "reply_to") {
		l.reply_to_list = util::Lower(value) == "list";
	} else if (k == "subject_tag") {
		l.subject_tag = value;
	} else if (k == "footer") {
		l.footer = value;
	} else if (k == "digest_interval") {
		l.digest_interval_secs = std::atoll(value.c_str());
	} else if (k == "digest_max") {
		l.digest_max = std::atoll(value.c_str());
	} else {
		err = "unknown setting '" + key + "'";
		return false;
	}
	return SetList(con, l, err);
}

std::string ListAddress(Connection &con, const List &list) {
	return list.address + "@" + Fqdn(con);
}

std::string BounceAddress(Connection &con, const List &list) {
	return list.address + "-bounces@" + Fqdn(con);
}

// ---------------------------------------------------------------------------
// Addressing
// ---------------------------------------------------------------------------

bool ResolveAddress(Connection &con, const std::string &local_part, List &list, Command &cmd) {
	std::string local = util::Lower(local_part);
	cmd = Command();

	auto load = [&](const std::string &address) {
		auto v = ScalarP(con, "SELECT room_num FROM citadel_lists WHERE address = $1 AND enabled",
		                 {Value(address)});
		if (v.IsNull()) {
			return false;
		}
		return GetList(con, Int(v), list);
	};

	// Suffix forms first: a list named "announce" and a list named
	// "announce-subscribe" could otherwise both claim "announce-subscribe@",
	// and the command form has to win (SetList refuses to create the latter).
	struct Suffix {
		const char *text;
		Command::Kind kind;
	};
	static const Suffix kSuffixes[] = {
	    {"-subscribe", Command::Subscribe}, {"-unsubscribe", Command::Unsubscribe},
	    {"-bounces", Command::Bounce},      {"-request", Command::Help},
	    {"-help", Command::Help},
	};
	for (const auto &s : kSuffixes) {
		std::string suffix = s.text;
		if (local.size() > suffix.size() &&
		    local.compare(local.size() - suffix.size(), suffix.size(), suffix) == 0) {
			if (load(local.substr(0, local.size() - suffix.size()))) {
				cmd.kind = s.kind;
				cmd.room_num = list.room_num;
				return true;
			}
		}
	}
	// "<list>-confirm-<token>": the token is opaque and may contain '-', so the
	// split is on the first "-confirm-" from the left of a known list name.
	auto conf = local.find("-confirm-");
	if (conf != std::string::npos && load(local.substr(0, conf))) {
		cmd.kind = Command::Confirm;
		cmd.room_num = list.room_num;
		cmd.token = local_part.substr(conf + 9); // case preserved: tokens are base64url
		return true;
	}

	if (load(local)) {
		cmd.kind = Command::Post;
		cmd.room_num = list.room_num;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Subscribers
// ---------------------------------------------------------------------------

std::vector<Sub> Subscribers(Connection &con, int64_t room_num, const std::string &state) {
	std::vector<Sub> out;
	std::string sql = "SELECT address, kind, state, created_at, confirmed_at FROM citadel_list_subs "
	                  "WHERE room_num = $1";
	duckdb::vector<Value> params = {Value::BIGINT(room_num)};
	if (!state.empty()) {
		sql += " AND state = $2";
		params.push_back(Value(state));
	}
	sql += " ORDER BY address";
	auto r = ExecP(con, sql, params);
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		Sub s;
		s.room_num = room_num;
		s.address = Str(mat.GetValue(0, i));
		s.kind = Str(mat.GetValue(1, i)) == "digest" ? SubKind::Digest : SubKind::Post;
		s.state = ParseState(Str(mat.GetValue(2, i)));
		s.created_at = Int(mat.GetValue(3, i));
		s.confirmed_at = Int(mat.GetValue(4, i));
		out.push_back(s);
	}
	return out;
}

bool IsSubscriber(Connection &con, int64_t room_num, const std::string &address) {
	return Int(ScalarP(con,
	                   "SELECT count(*) FROM citadel_list_subs WHERE room_num = $1 AND address = $2 "
	                   "AND state = 'active'",
	                   {Value::BIGINT(room_num), Value(NormAddr(address))})) > 0;
}

bool Subscribe(Connection &con, const List &list, const std::string &address, SubKind kind, bool confirmed,
               std::string &token, std::string &err) {
	std::string addr = NormAddr(address);
	if (addr.empty() || addr.find('@') == std::string::npos) {
		err = "'" + address + "' is not an e-mail address";
		return false;
	}
	std::string existing = Str(ScalarP(con,
	                                   "SELECT state FROM citadel_list_subs WHERE room_num = $1 AND "
	                                   "address = $2",
	                                   {Value::BIGINT(list.room_num), Value(addr)}));
	if (existing == "active" && confirmed) {
		err = "already subscribed";
		return false;
	}

	token.clear();
	int64_t expires = 0;
	if (!confirmed) {
		token = util::RandomBase64Url(24);
		if (token.empty()) {
			// RandomBase64Url only fails if the RNG did. Minting a guessable
			// confirmation token would be worse than refusing.
			err = "could not generate a confirmation token";
			return false;
		}
		expires = NowEpoch() + kTokenTtlSecs;
	}
	std::string state = confirmed ? "active" : "pending";
	int64_t now = NowEpoch();
	duckdb::vector<Value> params = {Value::BIGINT(list.room_num), Value(addr),   Value(KindName(kind)),
	                                Value(state),                 Value(token),  Value::BIGINT(expires),
	                                Value::BIGINT(now),           Value::BIGINT(confirmed ? now : 0)};
	if (!existing.empty()) {
		if (!ExecP(con,
		           "UPDATE citadel_list_subs SET kind = $3, state = $4, token = $5, token_expires = $6, "
		           "confirmed_at = $8 WHERE room_num = $1 AND address = $2",
		           params)) {
			err = "could not update the subscription";
			return false;
		}
		return true;
	}
	if (!ExecP(con,
	           "INSERT INTO citadel_list_subs (room_num, address, kind, state, token, token_expires, "
	           "created_at, confirmed_at) VALUES ($1, $2, $3, $4, $5, $6, $7, $8)",
	           params)) {
		err = "could not record the subscription";
		return false;
	}
	return true;
}

bool Unsubscribe(Connection &con, const List &list, const std::string &address, bool confirmed,
                 std::string &token, std::string &err) {
	std::string addr = NormAddr(address);
	std::string existing = Str(ScalarP(con,
	                                   "SELECT state FROM citadel_list_subs WHERE room_num = $1 AND "
	                                   "address = $2",
	                                   {Value::BIGINT(list.room_num), Value(addr)}));
	if (existing.empty()) {
		err = "not subscribed";
		return false;
	}
	token.clear();
	if (confirmed) {
		if (!ExecP(con, "DELETE FROM citadel_list_subs WHERE room_num = $1 AND address = $2",
		           {Value::BIGINT(list.room_num), Value(addr)})) {
			err = "could not remove the subscription";
			return false;
		}
		return true;
	}
	token = util::RandomBase64Url(24);
	if (token.empty()) {
		err = "could not generate a confirmation token";
		return false;
	}
	if (!ExecP(con,
	           "UPDATE citadel_list_subs SET state = 'unsub_pending', token = $3, token_expires = $4 "
	           "WHERE room_num = $1 AND address = $2",
	           {Value::BIGINT(list.room_num), Value(addr), Value(token),
	            Value::BIGINT(NowEpoch() + kTokenTtlSecs)})) {
		err = "could not record the request";
		return false;
	}
	return true;
}

bool Confirm(Connection &con, const std::string &token, std::string &what, std::string &err) {
	if (token.empty()) {
		err = "no token given";
		return false;
	}
	auto r = ExecP(con,
	               "SELECT room_num, address, state, token, token_expires FROM citadel_list_subs "
	               "WHERE token <> '' AND token_expires >= $1",
	               {Value::BIGINT(NowEpoch())});
	if (!r) {
		err = "could not read the subscription";
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	// Compared in constant time against every live token rather than looked up
	// by equality, so a timing difference cannot be used to walk the token
	// space. The set of unconfirmed subscriptions is small by nature.
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		if (!util::SecureEquals(Str(mat.GetValue(3, i)), token)) {
			continue;
		}
		int64_t room_num = Int(mat.GetValue(0, i));
		std::string addr = Str(mat.GetValue(1, i));
		std::string state = Str(mat.GetValue(2, i));
		List list;
		std::string name = GetList(con, room_num, list) ? list.address : ("room " + std::to_string(room_num));
		if (state == "unsub_pending") {
			ExecP(con, "DELETE FROM citadel_list_subs WHERE room_num = $1 AND address = $2",
			      {Value::BIGINT(room_num), Value(addr)});
			what = addr + " unsubscribed from " + name;
		} else {
			ExecP(con,
			      "UPDATE citadel_list_subs SET state = 'active', token = '', token_expires = 0, "
			      "confirmed_at = $3 WHERE room_num = $1 AND address = $2",
			      {Value::BIGINT(room_num), Value(addr), Value::BIGINT(NowEpoch())});
			what = addr + " subscribed to " + name;
		}
		return true;
	}
	err = "that confirmation link is not valid, or has expired";
	return false;
}

// ---------------------------------------------------------------------------
// Moderation
// ---------------------------------------------------------------------------

bool Hold(Connection &con, const List &list, const std::string &mail_from, const std::string &raw,
          std::string &err) {
	auto parsed = mime::Parse(raw);
	std::string subject = mime::DecodeEncodedWords(parsed.subject);
	if (!ExecP(con,
	           "INSERT INTO citadel_list_held (room_num, mail_from, subject, raw, received_at, state) "
	           "VALUES ($1, $2, $3, $4, $5, 'held')",
	           {Value::BIGINT(list.room_num), Value(mail_from), Value(subject),
	            Value::BLOB(reinterpret_cast<const duckdb::data_t *>(raw.data()), raw.size()),
	            Value::BIGINT(NowEpoch())})) {
		err = "could not hold the message";
		return false;
	}
	citadel::PostAideMessage(con, "Held for moderation: " + list.address,
	                         "A message from " + mail_from + " to the " + list.address +
	                             " list is waiting for approval.\nSubject: " + subject +
	                             "\n\nApprove or reject it from /admin/lists, or with "
	                             "`quackcitadm.sh list approve <id>`.");
	return true;
}

std::vector<Held> HeldMessages(Connection &con, int64_t room_num, const std::string &state) {
	std::vector<Held> out;
	std::string sql = "SELECT id, room_num, mail_from, subject, raw, received_at, state "
	                  "FROM citadel_list_held WHERE 1 = 1";
	duckdb::vector<Value> params;
	if (room_num >= 0) {
		sql += " AND room_num = $" + std::to_string(params.size() + 1);
		params.push_back(Value::BIGINT(room_num));
	}
	if (!state.empty()) {
		sql += " AND state = $" + std::to_string(params.size() + 1);
		params.push_back(Value(state));
	}
	sql += " ORDER BY id";
	auto r = ExecP(con, sql, params);
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		Held h;
		h.id = Int(mat.GetValue(0, i));
		h.room_num = Int(mat.GetValue(1, i));
		h.mail_from = Str(mat.GetValue(2, i));
		h.subject = Str(mat.GetValue(3, i));
		auto blob = mat.GetValue(4, i);
		h.raw = blob.IsNull() ? "" : duckdb::StringValue::Get(blob);
		h.received_at = Int(mat.GetValue(5, i));
		h.state = Str(mat.GetValue(6, i));
		out.push_back(h);
	}
	return out;
}

bool Approve(Connection &con, int64_t id, std::string &err) {
	auto r = ExecP(con,
	               "SELECT room_num, mail_from, raw FROM citadel_list_held WHERE id = $1 AND state = 'held'",
	               {Value::BIGINT(id)});
	if (!r) {
		err = "could not read the held message";
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		err = "no message " + std::to_string(id) + " is held";
		return false;
	}
	int64_t room_num = Int(mat.GetValue(0, 0));
	std::string mail_from = Str(mat.GetValue(1, 0));
	auto blob = mat.GetValue(2, 0);
	std::string raw = blob.IsNull() ? "" : duckdb::StringValue::Get(blob);

	// Approval posts into the room and stops. The spooler picks it up on its
	// next pass like any other post, so there is exactly one distribution path
	// and an approved message cannot be sent twice.
	deliver::Options opts;
	opts.extra_rooms.push_back(room_num);
	deliver::Outcome outcome;
	if (!deliver::LocalDeliver(con, mail_from, {}, raw, opts, outcome)) {
		err = outcome.err.empty() ? "could not post the message" : outcome.err;
		return false;
	}
	if (!ExecP(con, "UPDATE citadel_list_held SET state = 'approved' WHERE id = $1", {Value::BIGINT(id)})) {
		err = "posted, but could not mark the held message approved";
		return false;
	}
	return true;
}

bool Reject(Connection &con, int64_t id, std::string &err) {
	if (!ExecP(con, "UPDATE citadel_list_held SET state = 'rejected' WHERE id = $1 AND state = 'held'",
	           {Value::BIGINT(id)})) {
		err = "could not reject the message";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

std::string RenderForList(Connection &con, const List &list, const citadel::Message &msg) {
	std::string node = NodeName(con);
	std::string fqdn = Fqdn(con);
	std::string raw = citadel::RenderRfc822(msg, node);

	std::string headers, body;
	SplitMessage(raw, headers, body);
	auto fields = SplitFields(headers);

	std::string out;
	for (const auto &f : fields) {
		// Dropped on the way out:
		//  - DKIM-Signature / Authentication-Results: the subject tag and footer
		//    change bytes the signature covers, so keeping it would turn every
		//    copy into a verification failure. Outbound signing happens at the
		//    relay, over the message as subscribers will actually receive it.
		//  - Return-Path: rewritten by the receiving MTA from our envelope.
		//  - Any inbound List-*/Precedence: ours are authoritative, and letting a
		//    sender supply them would let them forge an unsubscribe address.
		if (HeaderNameIs(f, "DKIM-Signature") || HeaderNameIs(f, "Authentication-Results") ||
		    HeaderNameIs(f, "Return-Path") || HeaderNameIs(f, "Precedence") ||
		    HeaderNameIs(f, "List-Id") || HeaderNameIs(f, "List-Post") || HeaderNameIs(f, "List-Help") ||
		    HeaderNameIs(f, "List-Subscribe") || HeaderNameIs(f, "List-Unsubscribe") ||
		    HeaderNameIs(f, "List-Archive") || HeaderNameIs(f, "Sender")) {
			continue;
		}
		if (list.reply_to_list && HeaderNameIs(f, "Reply-To")) {
			continue; // replaced below
		}
		if (!list.subject_tag.empty() && HeaderNameIs(f, "Subject")) {
			std::string value = FieldValue(f);
			// Only tag when it is not already there, so a reply to a tagged
			// message does not accumulate "[list] Re: [list] ...".
			if (value.find(list.subject_tag) == std::string::npos) {
				value = list.subject_tag + " " + value;
			}
			out += "Subject: " + value + "\r\n";
			continue;
		}
		out += CrlfField(f);
	}

	std::string addr = list.address + "@" + fqdn;
	std::string display = list.display_name.empty() ? list.address : list.display_name;
	// RFC 2919: the List-Id is a stable, domain-scoped identifier; the phrase
	// before it is optional description.
	out += "List-Id: " + mime::EncodeEncodedWord(display) + " <" + list.address + "." + fqdn + ">\r\n";
	out += "List-Post: <mailto:" + addr + ">\r\n";
	out += "List-Help: <mailto:" + list.address + "-request@" + fqdn + ">\r\n";
	out += "List-Subscribe: <mailto:" + list.address + "-subscribe@" + fqdn + ">\r\n";
	out += "List-Unsubscribe: <mailto:" + list.address + "-unsubscribe@" + fqdn + ">\r\n";
	std::string archive = citadel::GetConfig(con, "qm_list_archive_base", "");
	if (!archive.empty()) {
		if (archive.back() == '/') {
			archive.pop_back();
		}
		out += "List-Archive: <" + archive + "/bbs/room/" + std::to_string(list.room_num) + ">\r\n";
	}
	out += "Precedence: list\r\n";
	out += "Sender: " + BounceAddress(con, list) + "\r\n";
	if (list.reply_to_list) {
		out += "Reply-To: " + addr + "\r\n";
	}

	// The footer is appended only to a plain-text body. Splicing text into a
	// multipart or non-text entity would corrupt it, and silently mangling
	// someone's attachment is worse than omitting a footer.
	std::string footer_body = body;
	if (!list.footer.empty()) {
		std::string ctype = util::Lower(FindHeader(fields, "Content-Type"));
		bool plain = ctype.empty() || ctype.compare(0, 10, "text/plain") == 0;
		std::string cte = util::Lower(FindHeader(fields, "Content-Transfer-Encoding"));
		bool plain_encoding = cte.empty() || cte == "7bit" || cte == "8bit";
		if (plain && plain_encoding) {
			if (!footer_body.empty() && footer_body.back() != '\n') {
				footer_body += "\r\n";
			}
			footer_body += "\r\n-- \r\n" + list.footer + "\r\n";
		}
	}

	return out + "\r\n" + footer_body;
}

// ---------------------------------------------------------------------------
// Distribution
// ---------------------------------------------------------------------------

namespace {

// Build the digest body: an index, then each message as a message/rfc822 part.
std::string BuildDigest(Connection &con, const List &list, const std::vector<citadel::Message> &msgs,
                        const std::string &boundary) {
	std::string node = NodeName(con);
	std::string index = "Messages in this digest:\r\n\r\n";
	for (size_t i = 0; i < msgs.size(); i++) {
		std::string subj = mime::DecodeEncodedWords(msgs[i].subject);
		index += "  " + std::to_string(i + 1) + ". " + (subj.empty() ? "(no subject)" : subj) + "\r\n";
		index += "     " + (msgs[i].author.empty() ? std::string("(unknown)") : msgs[i].author) + "\r\n";
	}

	std::string body;
	body += "--" + boundary + "\r\n";
	body += "Content-Type: text/plain; charset=\"UTF-8\"\r\n\r\n";
	body += index + "\r\n";
	for (const auto &m : msgs) {
		body += "--" + boundary + "\r\n";
		body += "Content-Type: message/rfc822\r\n\r\n";
		body += citadel::RenderRfc822(m, node);
		if (!body.empty() && body.back() != '\n') {
			body += "\r\n";
		}
	}
	body += "--" + boundary + "--\r\n";
	return body;
}

} // namespace

bool SpoolRoom(Connection &con, int64_t room_num, SpoolResult &out, std::string &err) {
	List list;
	if (!GetList(con, room_num, list)) {
		err = "no such list";
		return false;
	}
	if (!list.enabled) {
		return true;
	}
	std::string fqdn = Fqdn(con);
	std::string bounce = BounceAddress(con, list);

	// ---- immediate fan-out ----
	if (list.mode == Mode::Post || list.mode == Mode::Both) {
		auto subs = Subscribers(con, room_num, "active");
		auto msgs = citadel::RoomMessages(con, room_num, "gt", list.last_sent, 0);
		int64_t high = list.last_sent;
		for (int64_t msgnum : msgs) {
			high = std::max(high, msgnum);
			citadel::Message msg;
			if (!citadel::LoadMessage(con, msgnum, msg)) {
				continue;
			}
			std::string rendered = RenderForList(con, list, msg);
			bool any = false;
			for (const auto &s : subs) {
				if (s.kind != SubKind::Post) {
					continue;
				}
				store::EnqueueOutbound(con, bounce, s.address, rendered);
				out.recipients++;
				any = true;
			}
			if (any) {
				out.distributed++;
			}
		}
		if (high != list.last_sent) {
			// Advanced after the batch, and only once: a crash mid-batch resends
			// rather than silently dropping, which is the right way round for a
			// mailing list.
			ExecP(con, "UPDATE citadel_lists SET last_sent = $2 WHERE room_num = $1",
			      {Value::BIGINT(room_num), Value::BIGINT(high)});
			list.last_sent = high;
		}
	}

	// ---- digest ----
	if (list.mode == Mode::Digest || list.mode == Mode::Both) {
		int64_t now = NowEpoch();
		bool due = list.last_digest_at == 0 || now - list.last_digest_at >= list.digest_interval_secs;
		auto pending = citadel::RoomMessages(con, room_num, "gt", list.last_digest, 0);
		if (due && !pending.empty()) {
			std::vector<citadel::Message> msgs;
			int64_t high = list.last_digest;
			for (int64_t msgnum : pending) {
				if ((int64_t)msgs.size() >= list.digest_max) {
					break; // the rest ride in the next digest
				}
				citadel::Message m;
				if (citadel::LoadMessage(con, msgnum, m)) {
					msgs.push_back(m);
					high = std::max(high, msgnum);
				}
			}
			auto subs = Subscribers(con, room_num, "active");
			bool any_digest_sub = std::any_of(subs.begin(), subs.end(),
			                                  [](const Sub &s) { return s.kind == SubKind::Digest; });
			if (!msgs.empty() && any_digest_sub) {
				std::string boundary = "=_qc_digest_" + util::RandomHex(16);
				std::string subject = (list.subject_tag.empty() ? ("[" + list.address + "]")
				                                                : list.subject_tag) +
				                      " digest, " + std::to_string(msgs.size()) + " message" +
				                      (msgs.size() == 1 ? "" : "s");
				std::string head;
				head += "Date: " + util::RfcDate(now) + "\r\n";
				head += "From: " + list.address + "@" + fqdn + "\r\n";
				head += "To: " + list.address + "@" + fqdn + "\r\n";
				head += "Subject: " + mime::EncodeEncodedWord(subject) + "\r\n";
				head += "Message-ID: <digest." + util::RandomHex(12) + "." + std::to_string(now) + "@" +
				        fqdn + ">\r\n";
				head += "MIME-Version: 1.0\r\n";
				head += "Content-Type: multipart/digest; boundary=\"" + boundary + "\"\r\n";
				head += "List-Id: " + mime::EncodeEncodedWord(list.display_name.empty() ? list.address
				                                                                        : list.display_name) +
				        " <" + list.address + "." + fqdn + ">\r\n";
				head += "List-Unsubscribe: <mailto:" + list.address + "-unsubscribe@" + fqdn + ">\r\n";
				head += "Precedence: list\r\n";
				std::string digest = head + "\r\n" + BuildDigest(con, list, msgs, boundary);
				for (const auto &s : subs) {
					if (s.kind != SubKind::Digest) {
						continue;
					}
					store::EnqueueOutbound(con, bounce, s.address, digest);
					out.recipients++;
				}
				out.digests++;
			}
			ExecP(con, "UPDATE citadel_lists SET last_digest = $2, last_digest_at = $3 WHERE room_num = $1",
			      {Value::BIGINT(room_num), Value::BIGINT(high), Value::BIGINT(now)});
		}
	}

	out.held += (int64_t)HeldMessages(con, room_num, "held").size();
	return true;
}

bool SpoolOnce(Connection &con, SpoolResult &out, std::string &err) {
	for (const auto &l : ListLists(con, true)) {
		std::string one_err;
		if (!SpoolRoom(con, l.room_num, out, one_err) && err.empty()) {
			// One broken list must not stop the others; the first failure is
			// reported and the rest still run.
			err = "list " + l.address + ": " + one_err;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Self-service
// ---------------------------------------------------------------------------

namespace {

// Queue a plain-text reply from the list robot.
void QueueReply(Connection &con, const List &list, const std::string &to, const std::string &subject,
                const std::string &text) {
	std::string fqdn = Fqdn(con);
	int64_t now = NowEpoch();
	std::string msg;
	msg += "Date: " + util::RfcDate(now) + "\r\n";
	msg += "From: " + list.address + "-request@" + fqdn + "\r\n";
	msg += "To: " + to + "\r\n";
	msg += "Subject: " + mime::EncodeEncodedWord(subject) + "\r\n";
	msg += "Message-ID: <listbot." + util::RandomHex(12) + "." + std::to_string(now) + "@" + fqdn + ">\r\n";
	msg += "MIME-Version: 1.0\r\n";
	msg += "Content-Type: text/plain; charset=\"UTF-8\"\r\n";
	msg += "Auto-Submitted: auto-replied\r\n";
	msg += "Precedence: bulk\r\n";
	msg += "\r\n";
	msg += text;
	// A robot reply carries a null envelope sender (RFC 5321 §4.5.5), so a
	// bounce of the reply cannot bounce back and start a loop.
	store::EnqueueOutbound(con, "", to, msg);
}

std::string HelpText(Connection &con, const List &list) {
	std::string fqdn = Fqdn(con);
	std::string a = list.address;
	return "This is the " + a + " mailing list at " + fqdn +
	       ".\r\n"
	       "\r\n"
	       "To post:         " + a + "@" + fqdn +
	       "\r\n"
	       "To subscribe:    " + a + "-subscribe@" + fqdn +
	       "\r\n"
	       "To unsubscribe:  " + a + "-unsubscribe@" + fqdn +
	       "\r\n"
	       "For this help:   " + a + "-request@" + fqdn +
	       "\r\n"
	       "\r\n"
	       "Send an empty message to the subscribe or unsubscribe address and you\r\n"
	       "will receive a confirmation request. Nothing changes until you reply to\r\n"
	       "it, so nobody else can subscribe or unsubscribe you.\r\n";
}

} // namespace

bool SendConfirmation(Connection &con, const List &list, const std::string &address,
                      const std::string &token, bool subscribing) {
	std::string fqdn = Fqdn(con);
	std::string confirm = list.address + "-confirm-" + token + "@" + fqdn;
	std::string verb = subscribing ? "subscribe to" : "unsubscribe from";
	std::string text = "Somebody — probably you — asked to " + verb + " the " + list.address +
	                   " mailing list at " + fqdn +
	                   ".\r\n"
	                   "\r\n"
	                   "To confirm, reply to this message, or send any message to:\r\n"
	                   "\r\n"
	                   "    " +
	                   confirm +
	                   "\r\n"
	                   "\r\n"
	                   "If you did not ask for this, ignore this message: nothing will happen\r\n"
	                   "and the request expires in seven days.\r\n";
	// The confirmation address is also the Reply-To, so "reply" is enough.
	std::string now_date = util::RfcDate(NowEpoch());
	std::string msg;
	msg += "Date: " + now_date + "\r\n";
	msg += "From: " + list.address + "-request@" + fqdn + "\r\n";
	msg += "To: " + address + "\r\n";
	msg += "Reply-To: " + confirm + "\r\n";
	msg += "Subject: " + mime::EncodeEncodedWord("Confirm your request to " + verb + " " + list.address) +
	       "\r\n";
	msg += "Message-ID: <confirm." + util::RandomHex(12) + "@" + fqdn + ">\r\n";
	msg += "MIME-Version: 1.0\r\n";
	msg += "Content-Type: text/plain; charset=\"UTF-8\"\r\n";
	msg += "Auto-Submitted: auto-replied\r\n";
	msg += "Precedence: bulk\r\n";
	msg += "\r\n";
	msg += text;
	store::EnqueueOutbound(con, "", address, msg);
	return true;
}

bool HandleCommand(Connection &con, const List &list, const Command &cmd, const std::string &mail_from,
                   const std::string &body, std::string &err) {
	(void)body;
	std::string from = NormAddr(mail_from);
	switch (cmd.kind) {
	case Command::Bounce:
		// Delivery failures land here. They are counted by being logged, not
		// answered — replying to a bounce is how mail loops start.
		citadel::PostAideMessage(con, "List bounce: " + list.address,
		                         "A delivery failure came back to the " + list.address + " list.");
		return true;

	case Command::Help:
		if (from.empty()) {
			return true; // a null sender gets no reply, by the same loop rule
		}
		QueueReply(con, list, from, "About the " + list.address + " mailing list", HelpText(con, list));
		return true;

	case Command::Subscribe: {
		if (from.empty()) {
			return true;
		}
		if (IsSubscriber(con, list.room_num, from)) {
			QueueReply(con, list, from, "Already subscribed to " + list.address,
			           from + " is already subscribed to " + list.address + ".\r\n");
			return true;
		}
		std::string token;
		std::string sub_err;
		if (!Subscribe(con, list, from, SubKind::Post, false, token, sub_err)) {
			err = sub_err;
			return false;
		}
		return SendConfirmation(con, list, from, token, true);
	}

	case Command::Unsubscribe: {
		if (from.empty()) {
			return true;
		}
		std::string token;
		std::string sub_err;
		if (!Unsubscribe(con, list, from, false, token, sub_err)) {
			QueueReply(con, list, from, "Not subscribed to " + list.address,
			           from + " is not subscribed to " + list.address + ".\r\n");
			return true;
		}
		return SendConfirmation(con, list, from, token, false);
	}

	case Command::Confirm: {
		std::string what;
		std::string conf_err;
		if (!Confirm(con, cmd.token, what, conf_err)) {
			if (!from.empty()) {
				QueueReply(con, list, from, "Could not confirm your request", conf_err + "\r\n");
			}
			return true;
		}
		if (!from.empty()) {
			QueueReply(con, list, from, "Confirmed: " + list.address, what + ".\r\n");
		}
		return true;
	}

	default:
		return true;
	}
}

} // namespace listserv
} // namespace quackmail
