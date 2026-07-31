#include "quackmail/fetch.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/delivery.hpp"
#include "quackmail/feed.hpp"
#include "quackmail/http_client.hpp"
#include "quackmail/mail_client.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/util.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>

namespace quackmail {
namespace fetch {

using duckdb::Connection;
using duckdb::idx_t;
using duckdb::MaterializedQueryResult;
using duckdb::QueryResult;
using duckdb::Value;

namespace {

// How many seen-uids to keep per feed. Enough that a mailbox or feed can shrink
// and regrow without re-posting, small enough that the table stays bounded.
constexpr int64_t kSeenKeep = 2000;

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

const char *const kFeedColumns =
    "id, name, kind, enabled, url, host, port, tls, username, password, mailbox, "
    "target_room, target_user, author_override, subject_prefix, interval_secs, "
    "leave_on_server, max_per_run, max_bytes, last_run_at, last_status, last_error, "
    "uidvalidity, last_uid, etag, last_modified, messages_pulled";

Feed RowToFeed(MaterializedQueryResult &mat, idx_t row) {
	Feed f;
	f.id = Int(mat.GetValue(0, row));
	f.name = Str(mat.GetValue(1, row));
	f.kind = ParseKind(Str(mat.GetValue(2, row)));
	f.enabled = Bool(mat.GetValue(3, row), true);
	f.url = Str(mat.GetValue(4, row));
	f.host = Str(mat.GetValue(5, row));
	f.port = Int(mat.GetValue(6, row));
	f.tls = ParseTls(Str(mat.GetValue(7, row)));
	f.username = Str(mat.GetValue(8, row));
	f.password = Str(mat.GetValue(9, row));
	f.mailbox = Str(mat.GetValue(10, row));
	f.target_room = Int(mat.GetValue(11, row), -1);
	f.target_user = Str(mat.GetValue(12, row));
	f.author_override = Str(mat.GetValue(13, row));
	f.subject_prefix = Str(mat.GetValue(14, row));
	f.interval_secs = Int(mat.GetValue(15, row), 900);
	f.leave_on_server = Bool(mat.GetValue(16, row), true);
	f.max_per_run = Int(mat.GetValue(17, row), 50);
	f.max_bytes = Int(mat.GetValue(18, row), 5 * 1024 * 1024);
	f.last_run_at = Int(mat.GetValue(19, row));
	f.last_status = Str(mat.GetValue(20, row));
	f.last_error = Str(mat.GetValue(21, row));
	f.uidvalidity = Int(mat.GetValue(22, row));
	f.last_uid = Int(mat.GetValue(23, row));
	f.etag = Str(mat.GetValue(24, row));
	f.last_modified = Str(mat.GetValue(25, row));
	f.messages_pulled = Int(mat.GetValue(26, row));
	return f;
}

mailclient::TlsMode ToClientTls(Tls t) {
	switch (t) {
	case Tls::None:
		return mailclient::TlsMode::None;
	case Tls::Implicit:
		return mailclient::TlsMode::Implicit;
	default:
		return mailclient::TlsMode::StartTls;
	}
}

bool Seen(Connection &con, int64_t feed_id, const std::string &uid) {
	return Int(ScalarP(con, "SELECT count(*) FROM quackmail_feed_seen WHERE feed_id = $1 AND uid = $2",
	                   {Value::BIGINT(feed_id), Value(uid)})) > 0;
}

void MarkSeen(Connection &con, int64_t feed_id, const std::string &uid) {
	ExecP(con, "INSERT INTO quackmail_feed_seen (feed_id, uid, seen_at) VALUES ($1, $2, $3) "
	           "ON CONFLICT (feed_id, uid) DO NOTHING",
	      {Value::BIGINT(feed_id), Value(uid), Value::BIGINT(NowEpoch())});
}

void PruneSeen(Connection &con, int64_t feed_id) {
	ExecP(con,
	      "DELETE FROM quackmail_feed_seen WHERE feed_id = $1 AND uid NOT IN "
	      "(SELECT uid FROM quackmail_feed_seen WHERE feed_id = $1 ORDER BY seen_at DESC LIMIT $2)",
	      {Value::BIGINT(feed_id), Value::BIGINT(kSeenKeep)});
}

// Post one message. Either straight into a room, or through the delivery path
// so the target user's Sieve script gets a say.
bool Store(Connection &con, const Feed &f, const std::string &raw, const std::string &uid,
           std::string &err) {
	if (!f.target_user.empty()) {
		deliver::Options opts;
		deliver::Outcome out;
		std::string from = "feed-" + f.name + "@" + citadel::GetConfig(con, "c_fqdn", "localhost");
		if (!deliver::LocalDeliver(con, from, {f.target_user}, raw, opts, out)) {
			err = out.err.empty() ? "delivery failed" : out.err;
			return false;
		}
		return true;
	}

	auto parsed = mime::Parse(raw);
	citadel::Message msg;
	msg.format_type = 4; // RFC822: the bytes are already a message
	msg.raw = raw;
	msg.subject = mime::DecodeEncodedWords(parsed.subject);
	// The EUID is the source identifier, so Citadel's own duplicate suppression
	// backs the seen table rather than merely duplicating it.
	msg.euid = "feed-" + f.name + "-" + uid;
	msg.msgtime = NowEpoch();
	int64_t when = 0;
	for (const auto &h : parsed.headers) {
		if (util::Lower(h.first) == "date" && mime::ParseDate(h.second, when) && when > 0) {
			msg.msgtime = when;
		}
	}
	if (!parsed.from.empty()) {
		auto addrs = mime::ParseAddressList(parsed.from);
		if (!addrs.empty()) {
			msg.author = !addrs[0].name.empty() ? addrs[0].name : addrs[0].addr;
		}
	}
	if (msg.author.empty()) {
		msg.author = f.name;
	}
	citadel::Room room;
	if (citadel::GetRoomByNum(con, f.target_room, room)) {
		msg.origin_room = room.display_name;
	}
	return citadel::InsertMessage(con, msg, {f.target_room}, err) >= 0;
}

void RecordRun(Connection &con, const Feed &f, const RunResult &res, int64_t uidvalidity, int64_t last_uid,
               const std::string &etag, const std::string &last_modified) {
	ExecP(con,
	      "UPDATE quackmail_feeds SET last_run_at = $2, last_status = $3, last_error = $4, "
	      "uidvalidity = $5, last_uid = $6, etag = $7, last_modified = $8, "
	      "messages_pulled = messages_pulled + $9 WHERE id = $1",
	      {Value::BIGINT(f.id), Value::BIGINT(NowEpoch()), Value(res.status), Value(res.error),
	       Value::BIGINT(uidvalidity), Value::BIGINT(last_uid), Value(etag), Value(last_modified),
	       Value::BIGINT(res.stored)});
}

} // namespace

std::string KindName(Kind k) {
	switch (k) {
	case Kind::Pop3:
		return "pop3";
	case Kind::Imap:
		return "imap";
	default:
		return "rss";
	}
}

Kind ParseKind(const std::string &s) {
	std::string v = util::Lower(s);
	if (v == "pop3") {
		return Kind::Pop3;
	}
	if (v == "imap") {
		return Kind::Imap;
	}
	return Kind::Rss;
}

std::string TlsName(Tls t) {
	switch (t) {
	case Tls::None:
		return "none";
	case Tls::Implicit:
		return "implicit";
	default:
		return "starttls";
	}
}

Tls ParseTls(const std::string &s) {
	std::string v = util::Lower(s);
	if (v == "none" || v == "plain") {
		return Tls::None;
	}
	if (v == "implicit" || v == "ssl" || v == "tls") {
		return Tls::Implicit;
	}
	return Tls::StartTls;
}

// ---------------------------------------------------------------------------

void EnsureSchema(Connection &con) {
	con.Query("CREATE SEQUENCE IF NOT EXISTS quackmail_feed_seq START 1");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_feeds (
			id               BIGINT DEFAULT nextval('quackmail_feed_seq'),
			name             VARCHAR PRIMARY KEY,
			kind             VARCHAR DEFAULT 'rss',
			enabled          BOOLEAN DEFAULT true,
			url              VARCHAR DEFAULT '',
			host             VARCHAR DEFAULT '',
			port             BIGINT DEFAULT 0,
			tls              VARCHAR DEFAULT 'starttls',
			username         VARCHAR DEFAULT '',
			password         VARCHAR DEFAULT '',
			mailbox          VARCHAR DEFAULT 'INBOX',
			target_room      BIGINT DEFAULT -1,
			target_user      VARCHAR DEFAULT '',
			author_override  VARCHAR DEFAULT '',
			subject_prefix   VARCHAR DEFAULT '',
			interval_secs    BIGINT DEFAULT 900,
			leave_on_server  BOOLEAN DEFAULT true,
			max_per_run      BIGINT DEFAULT 50,
			max_bytes        BIGINT DEFAULT 5242880,
			last_run_at      BIGINT DEFAULT 0,
			last_status      VARCHAR DEFAULT '',
			last_error       VARCHAR DEFAULT '',
			uidvalidity      BIGINT DEFAULT 0,
			last_uid         BIGINT DEFAULT 0,
			etag             VARCHAR DEFAULT '',
			last_modified    VARCHAR DEFAULT '',
			messages_pulled  BIGINT DEFAULT 0,
			created_at       TIMESTAMP DEFAULT now()
		)
	)");

	// What has already been posted. POP3 UIDLs, IMAP "<uidvalidity>.<uid>", and
	// RSS guids all land here, which is what makes a poll idempotent.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_feed_seen (
			feed_id BIGINT,
			uid     VARCHAR,
			seen_at BIGINT DEFAULT 0,
			PRIMARY KEY (feed_id, uid)
		)
	)");

	// A feed targets a room or a local user, never both (SetFeed enforces it), so
	// a room-targeted feed whose room has been deleted has nowhere left to put
	// what it pulls — RunDue would go on polling the far end every interval and
	// throwing the result away. The room going takes the feed with it. Same
	// reason as listserv's hook: citadel_store cannot reach this table without
	// depending on the layer above it, so it calls back here.
	citadel::RegisterRoomDeletedHook("fetch", [](Connection &con, int64_t room_num) {
		auto r = ExecP(con, "SELECT name FROM quackmail_feeds WHERE target_room = $1",
		               {Value::BIGINT(room_num)});
		if (!r) {
			return;
		}
		auto &mat = r->Cast<MaterializedQueryResult>();
		for (idx_t i = 0; i < mat.RowCount(); i++) {
			std::string err;
			// Through RemoveFeed so the seen-uid rows go too, in one place.
			RemoveFeed(con, Str(mat.GetValue(0, i)), err);
		}
	});
}

std::vector<Feed> ListFeeds(Connection &con, bool enabled_only) {
	std::vector<Feed> out;
	std::string sql = std::string("SELECT ") + kFeedColumns + " FROM quackmail_feeds";
	if (enabled_only) {
		sql += " WHERE enabled";
	}
	sql += " ORDER BY name";
	auto r = con.Query(sql);
	if (!r || r->HasError()) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		out.push_back(RowToFeed(mat, i));
	}
	return out;
}

bool GetFeed(Connection &con, const std::string &name, Feed &out) {
	auto r = ExecP(con, std::string("SELECT ") + kFeedColumns + " FROM quackmail_feeds WHERE name = $1",
	               {Value(name)});
	if (!r) {
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return false;
	}
	out = RowToFeed(mat, 0);
	return true;
}

bool SetFeed(Connection &con, const Feed &in, std::string &err) {
	Feed f = in;
	if (f.name.empty()) {
		err = "a feed needs a name";
		return false;
	}
	// The name becomes part of a From: address and an EUID, so it has to be
	// something an address can hold.
	for (char c : f.name) {
		if (!std::isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') {
			err = "a feed name may only contain letters, digits, '-', '_' and '.'";
			return false;
		}
	}
	if (f.kind == Kind::Rss) {
		httpc::Url u;
		if (!u.Parse(f.url)) {
			err = "an rss feed needs an http:// or https:// url";
			return false;
		}
	} else if (f.host.empty()) {
		err = KindName(f.kind) + " needs a host";
		return false;
	} else if (f.username.empty()) {
		err = KindName(f.kind) + " needs a username";
		return false;
	}

	if (f.target_room < 0 && f.target_user.empty()) {
		err = "a feed needs a target room or a target user";
		return false;
	}
	if (f.target_room >= 0 && !f.target_user.empty()) {
		err = "a feed takes a target room or a target user, not both";
		return false;
	}
	if (f.target_room >= 0) {
		citadel::Room room;
		if (!citadel::GetRoomByNum(con, f.target_room, room)) {
			err = "no room " + std::to_string(f.target_room);
			return false;
		}
	} else if (citadel::GetOrAssignUserNum(con, f.target_user) == 0) {
		err = "'" + f.target_user + "' is not a local user";
		return false;
	}
	if (f.interval_secs <= 0) {
		f.interval_secs = 900;
	}
	if (f.max_per_run <= 0) {
		f.max_per_run = 50;
	}
	if (f.max_bytes <= 0) {
		f.max_bytes = 5 * 1024 * 1024;
	}
	if (f.mailbox.empty()) {
		f.mailbox = "INBOX";
	}

	duckdb::vector<Value> params = {Value(f.name),
	                                Value(KindName(f.kind)),
	                                Value::BOOLEAN(f.enabled),
	                                Value(f.url),
	                                Value(f.host),
	                                Value::BIGINT(f.port),
	                                Value(TlsName(f.tls)),
	                                Value(f.username),
	                                Value(f.password),
	                                Value(f.mailbox),
	                                Value::BIGINT(f.target_room),
	                                Value(f.target_user),
	                                Value(f.author_override),
	                                Value(f.subject_prefix),
	                                Value::BIGINT(f.interval_secs),
	                                Value::BOOLEAN(f.leave_on_server),
	                                Value::BIGINT(f.max_per_run),
	                                Value::BIGINT(f.max_bytes)};
	bool exists = Int(ScalarP(con, "SELECT count(*) FROM quackmail_feeds WHERE name = $1", {Value(f.name)})) > 0;
	if (exists) {
		if (!ExecP(con,
		           "UPDATE quackmail_feeds SET kind = $2, enabled = $3, url = $4, host = $5, port = $6, "
		           "tls = $7, username = $8, password = $9, mailbox = $10, target_room = $11, "
		           "target_user = $12, author_override = $13, subject_prefix = $14, "
		           "interval_secs = $15, leave_on_server = $16, max_per_run = $17, max_bytes = $18 "
		           "WHERE name = $1",
		           params)) {
			err = "could not update the feed";
			return false;
		}
		return true;
	}
	if (!ExecP(con,
	           "INSERT INTO quackmail_feeds (name, kind, enabled, url, host, port, tls, username, "
	           "password, mailbox, target_room, target_user, author_override, subject_prefix, "
	           "interval_secs, leave_on_server, max_per_run, max_bytes) VALUES "
	           "($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, $17, $18)",
	           params)) {
		err = "could not create the feed";
		return false;
	}
	return true;
}

bool RemoveFeed(Connection &con, const std::string &name, std::string &err) {
	Feed f;
	if (!GetFeed(con, name, f)) {
		err = "no feed called '" + name + "'";
		return false;
	}
	ExecP(con, "DELETE FROM quackmail_feed_seen WHERE feed_id = $1", {Value::BIGINT(f.id)});
	if (!ExecP(con, "DELETE FROM quackmail_feeds WHERE name = $1", {Value(name)})) {
		err = "could not remove the feed";
		return false;
	}
	return true;
}

bool SetField(Connection &con, const std::string &name, const std::string &key, const std::string &value,
              std::string &err) {
	Feed f;
	if (!GetFeed(con, name, f)) {
		err = "no feed called '" + name + "'";
		return false;
	}
	std::string k = util::Lower(key);
	auto truthy = [&]() {
		std::string v = util::Lower(value);
		return v == "1" || v == "true" || v == "yes" || v == "on";
	};
	if (k == "kind") {
		f.kind = ParseKind(value);
	} else if (k == "enabled") {
		f.enabled = truthy();
	} else if (k == "url") {
		f.url = value;
	} else if (k == "host") {
		f.host = value;
	} else if (k == "port") {
		f.port = std::atoll(value.c_str());
	} else if (k == "tls") {
		f.tls = ParseTls(value);
	} else if (k == "username") {
		f.username = value;
	} else if (k == "password") {
		f.password = value;
	} else if (k == "mailbox") {
		f.mailbox = value;
	} else if (k == "room") {
		citadel::Room room;
		if (!citadel::ResolveRoom(con, "", value, room)) {
			err = "no such public room";
			return false;
		}
		f.target_room = room.room_num;
		f.target_user.clear();
	} else if (k == "user") {
		f.target_user = value;
		f.target_room = -1;
	} else if (k == "author") {
		f.author_override = value;
	} else if (k == "subject_prefix") {
		f.subject_prefix = value;
	} else if (k == "interval") {
		f.interval_secs = std::atoll(value.c_str());
	} else if (k == "leave_on_server") {
		f.leave_on_server = truthy();
	} else if (k == "max_per_run") {
		f.max_per_run = std::atoll(value.c_str());
	} else {
		err = "unknown setting '" + key + "'";
		return false;
	}
	return SetFeed(con, f, err);
}

// ---------------------------------------------------------------------------
// Running one feed
// ---------------------------------------------------------------------------

namespace {

bool RunMailbox(Connection &con, const Feed &f, RunResult &res) {
	mailclient::Account acct;
	acct.host = f.host;
	acct.port = (int)f.port;
	acct.tls = ToClientTls(f.tls);
	acct.username = f.username;
	acct.password = f.password;
	acct.mailbox = f.mailbox;

	auto want = [&](const std::string &uid) { return !Seen(con, f.id, uid); };
	auto store = [&](const mailclient::Fetched &m) {
		std::string err;
		if (!Store(con, f, m.raw, m.uid, err)) {
			res.error = err;
			return false;
		}
		MarkSeen(con, f.id, m.uid);
		res.stored++;
		return true;
	};

	mailclient::Result r;
	if (f.kind == Kind::Pop3) {
		r = mailclient::FetchPop3(acct, (int)f.max_per_run, (size_t)f.max_bytes, !f.leave_on_server, want,
		                          store);
	} else {
		r = mailclient::FetchImap(acct, f.uidvalidity, f.last_uid, (int)f.max_per_run,
		                          (size_t)f.max_bytes, !f.leave_on_server, want, store);
	}
	res.fetched = r.fetched;
	res.skipped = std::max<int64_t>(0, r.seen - r.fetched);
	if (!r.ok) {
		res.status = "error";
		res.error = r.error;
		RecordRun(con, f, res, f.uidvalidity, f.last_uid, f.etag, f.last_modified);
		return false;
	}
	res.status = "ok";
	if (!r.info.empty() && res.error.empty()) {
		res.error = r.info; // a note, not a failure; shown in the same column
	}
	int64_t uidv = f.kind == Kind::Imap ? r.uidvalidity : f.uidvalidity;
	int64_t last = f.kind == Kind::Imap ? r.highest_uid : f.last_uid;
	RecordRun(con, f, res, uidv, last, f.etag, f.last_modified);
	PruneSeen(con, f.id);
	return true;
}

bool RunRss(Connection &con, const Feed &f, RunResult &res) {
	httpc::Options opts;
	opts.etag = f.etag;
	opts.last_modified = f.last_modified;
	opts.max_bytes = (size_t)f.max_bytes;
	opts.user_agent = "QuackCit/0.4 (+" + citadel::GetConfig(con, "c_fqdn", "localhost") + ")";

	httpc::Response http = httpc::Get(f.url, opts);
	if (!http.ok) {
		res.status = "error";
		res.error = http.error;
		RecordRun(con, f, res, f.uidvalidity, f.last_uid, f.etag, f.last_modified);
		return false;
	}
	if (http.NotModified()) {
		// The server told us nothing changed, which is the whole point of
		// sending the validators. Keep them: a 304 carries no new ones.
		res.status = "unchanged";
		RecordRun(con, f, res, f.uidvalidity, f.last_uid, f.etag, f.last_modified);
		return true;
	}

	feed::Feed parsed;
	if (!feed::Parse(http.body, parsed)) {
		res.status = "error";
		res.error = "the response is not an RSS or Atom feed";
		RecordRun(con, f, res, f.uidvalidity, f.last_uid, http.etag, http.last_modified);
		return false;
	}

	std::string fqdn = citadel::GetConfig(con, "c_fqdn", "localhost");
	// Oldest first, so message numbers follow publication order in the room.
	for (auto it = parsed.items.rbegin(); it != parsed.items.rend(); ++it) {
		if (res.stored >= f.max_per_run) {
			res.error = "stopped at the per-run limit";
			break;
		}
		if (it->guid.empty()) {
			continue;
		}
		if (Seen(con, f.id, it->guid)) {
			res.skipped++;
			continue;
		}
		res.fetched++;
		std::string raw =
		    feed::ToRfc822(parsed, *it, f.name, fqdn, f.subject_prefix, f.author_override);
		std::string err;
		if (!Store(con, f, raw, it->guid, err)) {
			res.status = "error";
			res.error = err;
			RecordRun(con, f, res, f.uidvalidity, f.last_uid, http.etag, http.last_modified);
			return false;
		}
		MarkSeen(con, f.id, it->guid);
		res.stored++;
	}

	res.status = "ok";
	RecordRun(con, f, res, f.uidvalidity, f.last_uid, http.etag, http.last_modified);
	PruneSeen(con, f.id);
	return true;
}

} // namespace

bool RunFeed(Connection &con, const Feed &f, RunResult &res) {
	res = RunResult();
	res.feed = f.name;
	if (f.kind == Kind::Rss) {
		return RunRss(con, f, res);
	}
	return RunMailbox(con, f, res);
}

std::vector<RunResult> RunDue(Connection &con, bool force) {
	std::vector<RunResult> out;
	int64_t now = NowEpoch();
	for (const auto &f : ListFeeds(con, true)) {
		if (!force && f.last_run_at > 0 && now - f.last_run_at < f.interval_secs) {
			continue;
		}
		RunResult res;
		RunFeed(con, f, res);
		out.push_back(res);
	}
	return out;
}

bool TestFeed(Connection &con, const std::string &name, std::string &info, std::string &err) {
	Feed f;
	if (!GetFeed(con, name, f)) {
		err = "no feed called '" + name + "'";
		return false;
	}
	if (f.kind == Kind::Rss) {
		httpc::Options opts;
		opts.max_bytes = (size_t)f.max_bytes;
		httpc::Response http = httpc::Get(f.url, opts);
		if (!http.ok) {
			err = http.error;
			return false;
		}
		feed::Feed parsed;
		if (!feed::Parse(http.body, parsed)) {
			err = "the response is not an RSS or Atom feed";
			return false;
		}
		info = "\"" + parsed.title + "\", " + std::to_string(parsed.items.size()) + " item(s)";
		return true;
	}

	mailclient::Account acct;
	acct.host = f.host;
	acct.port = (int)f.port;
	acct.tls = ToClientTls(f.tls);
	acct.username = f.username;
	acct.password = f.password;
	acct.mailbox = f.mailbox;
	auto r = f.kind == Kind::Pop3 ? mailclient::TestPop3(acct) : mailclient::TestImap(acct);
	if (!r.ok) {
		err = r.error;
		return false;
	}
	info = r.info;
	return true;
}

} // namespace fetch
} // namespace quackmail
