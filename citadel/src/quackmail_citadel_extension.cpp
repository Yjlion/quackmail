#define DUCKDB_EXTENSION_MAIN

#include "quackmail_citadel_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/citadel_msg.hpp"
#include "quackmail/citadel_store.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/util.hpp"
#include "quackmail/wiki.hpp"

#include <ctime>
#include <string>
#include <vector>

namespace duckdb {
namespace {

using namespace quackmail;

ServerController g_citadel;

constexpr size_t kMaxPostBytes = 1024 * 1024; // 1 MiB per posted message

// Per-connection Citadel session state.
struct Session {
	bool authed = false;
	std::string username;
	std::string pending_user; // set by USER, consumed by PASS
	int64_t usernum = 0;
	int64_t axlevel = 4;
	// Captured at login, for the PASS/NEWU reply. `last_call` is deliberately
	// the *previous* login: it is what the client prints as "Last login:".
	int64_t flags = 0; // US_* bitmask
	int64_t times_called = 0;
	int64_t num_posts = 0;
	int64_t last_call = 0;
	bool have_room = false;
	citadel::Room room;      // current room when have_room
	int64_t session_id = 0;  // row in citadel_sessions (presence/RWHO)
};

// Split a command line into an upper-cased 4-char-ish verb + the remainder.
void SplitCommand(const std::string &line, std::string &verb, std::string &rest) {
	size_t sp = line.find(' ');
	if (sp == std::string::npos) {
		verb = util::Upper(line);
		rest.clear();
	} else {
		verb = util::Upper(line.substr(0, sp));
		rest = line.substr(sp + 1);
	}
}

// Split a pipe-delimited parameter string.
std::vector<std::string> SplitPipe(const std::string &s) {
	std::vector<std::string> out;
	size_t start = 0;
	while (true) {
		size_t bar = s.find('|', start);
		if (bar == std::string::npos) {
			out.push_back(s.substr(start));
			break;
		}
		out.push_back(s.substr(start, bar - start));
		start = bar + 1;
	}
	return out;
}

std::string Field(const std::vector<std::string> &v, size_t i) {
	return i < v.size() ? v[i] : std::string();
}

int64_t ToInt(const std::string &s, int64_t dflt = 0) {
	try {
		return std::stoll(s);
	} catch (...) {
		return dflt;
	}
}

int64_t NowEpoch() {
	return (int64_t)std::time(nullptr);
}

// Process start time, reported by TIME as the server-uptime reference.
const int64_t g_server_start = (int64_t)std::time(nullptr);

// TIME reply: "200 <unixtime>|<gmtoffset_secs>|<isdst>|<serverstart>".
std::string TimeLine() {
	std::time_t now = std::time(nullptr);
	std::tm local {};
#if defined(_WIN32)
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif
	long gmtoff = 0;
#if !defined(_WIN32)
	gmtoff = local.tm_gmtoff;
#endif
	return "200 " + std::to_string((int64_t)now) + "|" + std::to_string(gmtoff) + "|" +
	       std::to_string(local.tm_isdst > 0 ? 1 : 0) + "|" + std::to_string(g_server_start);
}

// A `100` listing: preamble, lines, terminating `000`.
//
// The text after the code is per-verb in a real Citadel server ("Known rooms:",
// "Server info:", "msg:", ...), and while no client parses it, mirroring the
// wire exactly is the rule this project follows. The preamble is therefore a
// required argument rather than a default: a new listing verb should have to
// look up what the real server says instead of silently inheriting a generic
// string. Values taken from the oracle and from citadel/server/*.
void WriteListing(net::ClientStream &stream, const std::vector<std::string> &lines,
                  const std::string &preamble) {
	stream.WriteLine("100 " + preamble);
	for (auto &l : lines) {
		stream.WriteLine(l);
	}
	stream.WriteLine("000");
}

// One room's line in an LKR* / room listing.
std::string RoomListLine(const citadel::Room &r) {
	return r.display_name + "|" + std::to_string(r.qr_flags) + "|" + std::to_string(r.floor_num) + "|" +
	       std::to_string(r.listorder) + "|0|" + std::to_string(r.default_view) + "|" +
	       std::to_string(r.default_view) + "|0";
}

// The PASS/NEWU success line:
//   name|axlevel|timescalled|posts|flags|usernum|lastcall|email
//
// Field 5 is the US_* bitmask, and it is not decoration: the official text
// client reads its expert-mode and floor-mode settings straight out of it. A
// hardcoded 0 here means a user who turns on expert mode from the BBS shell or
// the web console still gets the full menu in the real client. Verified
// against the oracle, which answers `leo|4|0|0|10800|2|<epoch>|leo@debian`.
std::string LoginLine(Connection &con, const Session &s) {
	return "200 " + s.username + "|" + std::to_string(s.axlevel) + "|" +
	       std::to_string(s.times_called) + "|" + std::to_string(s.num_posts) + "|" +
	       std::to_string(s.flags) + "|" + std::to_string(s.usernum) + "|" +
	       std::to_string(s.last_call) + "|" + s.username + "@" +
	       citadel::GetConfig(con, "c_fqdn", "");
}

// Finish logging a user in: populate session, ensure a usernum + Mail room.
// `username` is taken by value on purpose: callers pass s.pending_user, which we
// clear below — a reference would be emptied before it is used.
void CompleteLogin(Connection &con, Session &s, std::string username) {
	s.authed = true;
	s.username = username;
	s.pending_user.clear();
	s.usernum = citadel::GetOrAssignUserNum(con, username);
	s.axlevel = citadel::GetAxLevel(con, username);
	// Read the user record before stamping this call, so `last_call` still
	// holds the previous one — that is what the client shows as "Last login:".
	citadel::UserInfo info;
	if (citadel::GetUser(con, username, info)) {
		s.flags = info.flags;
		s.times_called = info.times_called;
		s.num_posts = info.num_posts;
		s.last_call = info.last_call;
	}
	// Provision the full default room set (Mail, Sent Items, Calendar, ...) the
	// way a real Citadel server does on first login.
	citadel::EnsureUserRooms(con, username);
	citadel::RecordCall(con, username);
}

void HandleGoto(Connection &con, Session &s, net::ClientStream &stream, const std::vector<std::string> &p) {
	std::string wanted = Field(p, 0);
	// Citadel clients auto-navigate using magic room aliases rather than literal
	// names; translate the common ones to our seeded/per-user rooms.
	std::string alias = util::Upper(wanted);
	if (alias == "_BASEROOM_") {
		wanted = "Lobby";
	} else if (alias == "_MAIL_") {
		wanted = "Mail"; // the logged-in user's personal mailbox room
	} else if (alias == "_AIDE_") {
		wanted = "Aide";
	}
	citadel::Room room;
	if (!citadel::ResolveRoom(con, s.username, wanted, room)) {
		// 550 = generic error. Do NOT use 540 here: 540 means "password
		// required" to clients, which makes them prompt for a room password
		// and loop instead of reporting the room as missing.
		stream.WriteLine("550 No such room.");
		return;
	}
	if (room.qr_flags & citadel::QR_PASSWORDED) {
		if (Field(p, 1) != room.password) {
			stream.WriteLine("540 Wrong or missing password.");
			return;
		}
	}
	s.room = room;
	s.have_room = true;
	auto st = citadel::GetRoomStats(con, s.username, room.room_num);
	int is_mail = room.mailbox_owner > 0 ? 1 : 0;
	int is_aide = (room.qr_flags & citadel::QR_PRIVATE) ? 1 : 0;
	stream.WriteLine("200 " + room.display_name + "|" + std::to_string(st.new_count) + "|" +
	                 std::to_string(st.total) + "|" + (room.info.empty() ? "0" : "1") + "|" +
	                 std::to_string(room.qr_flags) + "|" + std::to_string(st.highest) + "|" +
	                 std::to_string(st.last_read) + "|" + std::to_string(is_mail) + "|" +
	                 std::to_string(is_aide) + "||" + std::to_string(room.floor_num) + "|" +
	                 std::to_string(room.default_view) + "|" + std::to_string(room.default_view) + "|0");
}

void HandleMsgs(Connection &con, Session &s, net::ClientStream &stream, const std::vector<std::string> &p) {
	if (!s.have_room) {
		stream.WriteLine("540 Not in a room; use GOTO first.");
		return;
	}
	std::string mode = util::Upper(Field(p, 0));
	std::string filter = "all";
	int64_t param = 0;
	if (mode == "NEW") {
		filter = "new";
	} else if (mode == "OLD") {
		filter = "old";
	} else if (mode == "LAST") {
		filter = "last";
		param = ToInt(Field(p, 1), 100);
	} else if (mode == "FIRST") {
		filter = "first";
		param = ToInt(Field(p, 1), 100);
	} else if (mode == "GT") {
		filter = "gt";
		param = ToInt(Field(p, 1), 0);
	} else if (mode == "LT") {
		filter = "lt";
		param = ToInt(Field(p, 1), 0);
	}
	auto st = citadel::GetRoomStats(con, s.username, s.room.room_num);
	auto nums = citadel::RoomMessages(con, s.room.room_num, filter, param, st.last_read);
	std::vector<std::string> lines;
	lines.reserve(nums.size());
	for (int64_t n : nums) {
		lines.push_back(std::to_string(n));
	}
	// serv_messages.c emits the bare code followed by two spaces.
	WriteListing(stream, lines, " ");
}

void HandleMsg0(Connection &con, net::ClientStream &stream, const std::vector<std::string> &p) {
	int64_t num = ToInt(Field(p, 0), -1);
	int mode = (int)ToInt(Field(p, 1), 0);
	citadel::Message msg;
	if (num < 0 || !citadel::LoadMessage(con, num, msg)) {
		stream.WriteLine("500 No such message.");
		return;
	}
	WriteListing(stream, citadel::FormatMsg0(msg, mode), "msg:"); // msgbase.c
}

// WIKI — page history and old revisions, as serv_wiki.c defines them.
//
//   WIKI history|<page>                     one memo per line, newest first
//   WIKI rev|<page>|<rev>|showrev|revert    a revision, or restore it
//
// This is the payoff for storing history in Citadel's own shape rather than in
// a table of our own: a real WebCit or Citadel client can browse the history a
// QuackCit user wrote, and this server can read theirs.
void HandleWiki(Connection &con, Session &s, net::ClientStream &stream,
                const std::vector<std::string> &p) {
	if (!s.have_room) {
		stream.WriteLine("540 Not in a room.");
		return;
	}
	if (!quackmail::wiki::IsWikiView(s.room.default_view)) {
		stream.WriteLine("550 '" + s.room.display_name + "' is not a Wiki room.");
		return;
	}
	const std::string sub = util::Lower(Field(p, 0));
	const std::string page = quackmail::wiki::NormalizeName(Field(p, 1));

	if (sub == "history") {
		auto revs = quackmail::wiki::History(con, s.room.room_num, page);
		if (revs.empty()) {
			stream.WriteLine("540 Revision history for '" + page + "' was not found.");
			return;
		}
		// The memo, decoded, one per line — the same shape wiki_history_callback
		// emits, so a client parses it the same way.
		std::vector<std::string> lines;
		for (const auto &r : revs) {
			lines.push_back(std::to_string(r.rev) + "|" + std::to_string(r.timestamp) + "|" +
			                r.author + "|");
		}
		WriteListing(stream, lines, "Revision history for '" + page + "'");
		return;
	}

	if (sub == "rev") {
		const int64_t rev = ToInt(Field(p, 2), 0);
		const std::string op = util::Lower(Field(p, 3));
		std::string raw;
		std::string err;
		if (!quackmail::wiki::RevisionRaw(con, s.room.room_num, page, rev, raw, err)) {
			stream.WriteLine("540 " + err);
			return;
		}
		if (op == "revert") {
			if (!citadel::CanPost(con, s.username, s.room)) {
				stream.WriteLine("550 You cannot write to this room.");
				return;
			}
			citadel::Message msg;
			msg.euid = page;
			msg.subject = page;
			msg.author = s.username;
			msg.author_usernum = citadel::GetOrAssignUserNum(con, s.username);
			msg.msgtime = (int64_t)std::time(nullptr);
			msg.format_type = 4;
			msg.origin_room = s.room.display_name;
			msg.raw = raw;
			std::string upsert_err;
			// Saved as a new revision, never by rewriting history: a restore is
			// an edit like any other and must itself be undoable.
			int64_t num = citadel::UpsertByEuid(con, msg, s.room.room_num, upsert_err);
			if (num < 0) {
				stream.WriteLine("550 " + upsert_err);
				return;
			}
			stream.WriteLine("200 " + std::to_string(num));
			return;
		}
		// "showrev" (and the deprecated "fetch", which stashed the revision in a
		// scratch room purely so the client could MSG0 it — pointless when the
		// text can simply be sent).
		std::vector<std::string> lines;
		std::string cur;
		for (char c : raw) {
			if (c == '\n') {
				if (!cur.empty() && cur.back() == '\r') {
					cur.pop_back();
				}
				lines.push_back(cur);
				cur.clear();
			} else {
				cur.push_back(c);
			}
		}
		if (!cur.empty()) {
			lines.push_back(cur);
		}
		WriteListing(stream, lines, "msg:");
		return;
	}

	stream.WriteLine("500 Invalid subcommand.");
}

void HandleMsg2(Connection &con, net::ClientStream &stream, const std::vector<std::string> &p) {
	int64_t num = ToInt(Field(p, 0), -1);
	citadel::Message msg;
	if (num < 0 || !citadel::LoadMessage(con, num, msg)) {
		stream.WriteLine("500 No such message.");
		return;
	}
	// Raw source, split into lines (RFC822 for format 4, body text otherwise).
	std::vector<std::string> lines;
	std::string cur;
	for (char c : msg.raw) {
		if (c == '\n') {
			if (!cur.empty() && cur.back() == '\r') {
				cur.pop_back();
			}
			lines.push_back(cur);
			cur.clear();
		} else {
			cur.push_back(c);
		}
	}
	if (!cur.empty()) {
		lines.push_back(cur);
	}
	WriteListing(stream, lines, "msg:"); // same message-output path as MSG0
}

// REGI — the client sends the registration as a listing, in the field order
// cmd_regi() reads it in the real server (serv_vcard.c): name, address, city,
// state, zip, telephone, e-mail, country. The same record the BBS shell's
// <.E>nter re<G>istration and the web console's preferences page write.
void HandleRegi(Connection &con, Session &s, net::ClientStream &stream) {
	if (!s.authed) {
		stream.WriteLine("530 You must log in first.");
		return;
	}
	stream.WriteLine("400 Send registration...");
	std::vector<std::string> fields;
	std::string line;
	while (stream.ReadLine(line, 8192)) {
		if (line == "000") {
			break;
		}
		if (fields.size() < 8) {
			fields.push_back(line);
		}
	}
	auto at = [&fields](size_t i) { return i < fields.size() ? fields[i] : std::string(); };

	citadel::Registration reg;
	citadel::GetRegistration(con, s.username, reg);
	reg.real_name = at(0);
	reg.street = at(1);
	reg.city = at(2);
	reg.state = at(3);
	reg.zipcode = at(4);
	reg.phone = at(5);
	reg.email = at(6);
	if (!at(7).empty()) {
		reg.country = at(7);
	}
	citadel::SetRegistration(con, s.username, reg);
}

// GREG <user> — read registration back. A user may read their own; an aide may
// read anyone's.
void HandleGreg(Connection &con, Session &s, net::ClientStream &stream, const std::vector<std::string> &p) {
	if (!s.authed) {
		stream.WriteLine("530 You must log in first.");
		return;
	}
	std::string who = Field(p, 0);
	if (who.empty()) {
		who = s.username;
	}
	if (who != s.username && s.axlevel < citadel::kAideAxLevel) {
		stream.WriteLine("540 Higher access required.");
		return;
	}
	citadel::UserInfo info;
	if (!citadel::GetUser(con, who, info)) {
		stream.WriteLine("550 No such user.");
		return;
	}
	citadel::Registration reg;
	citadel::GetRegistration(con, who, reg);
	stream.WriteLine("100 " + who);
	// The field order GREG reports in the real server.
	std::vector<std::string> lines = {std::to_string(info.usernum),
	                                  reg.real_name,
	                                  reg.street,
	                                  reg.city,
	                                  reg.state,
	                                  reg.zipcode,
	                                  reg.phone,
	                                  std::to_string(info.axlevel),
	                                  reg.email,
	                                  reg.country};
	for (auto &l : lines) {
		stream.WriteLine(l);
	}
	stream.WriteLine("000");
}

// EBIO / RBIO — the free-text biography that lives beside the registration.
void HandleEbio(Connection &con, Session &s, net::ClientStream &stream) {
	if (!s.authed) {
		stream.WriteLine("530 You must log in first.");
		return;
	}
	stream.WriteLine("400 Send new bio; terminate with '000'.");
	std::string bio, line;
	while (stream.ReadLine(line, 8192)) {
		if (line == "000") {
			break;
		}
		if (bio.size() + line.size() + 1 > 64 * 1024) {
			continue; // keep draining, but stop growing
		}
		bio += line;
		bio += '\n';
	}
	citadel::SetBio(con, s.username, bio);
}

void HandleRbio(Connection &con, Session &s, net::ClientStream &stream, const std::vector<std::string> &p) {
	std::string who = Field(p, 0);
	if (who.empty()) {
		who = s.username;
	}
	citadel::UserInfo info;
	if (!citadel::GetUser(con, who, info)) {
		stream.WriteLine("550 No such user.");
		return;
	}
	citadel::Registration reg;
	if (!citadel::GetRegistration(con, who, reg) || reg.bio.empty()) {
		stream.WriteLine("550 No bio on file.");
		return;
	}
	std::vector<std::string> lines;
	size_t pos = 0;
	while (pos < reg.bio.size()) {
		size_t nl = reg.bio.find('\n', pos);
		lines.push_back(reg.bio.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
		if (nl == std::string::npos) {
			break;
		}
		pos = nl + 1;
	}
	// serv_bio.c: "OK|<fullname>|<usernum>|<axlevel>|<lastcall>|0|0".
	WriteListing(stream, lines,
	             "OK|" + who + "|" + std::to_string(info.usernum) + "|" +
	                 std::to_string(info.axlevel) + "|" + std::to_string(info.last_call) + "|0|0");
}

// LIST — the user directory. US_UNLISTED hides an entry from everyone but an
// aide, which is exactly what the BBS shell's <.R>ead <U>ser listing honours.
void HandleList(Connection &con, Session &s, net::ClientStream &stream) {
	std::vector<std::string> lines;
	for (auto &u : citadel::ListUsers(con)) {
		if ((u.flags & citadel::US_UNLISTED) && s.axlevel < citadel::kAideAxLevel &&
		    u.username != s.username) {
			continue;
		}
		lines.push_back(u.username + "|" + std::to_string(u.axlevel) + "|" +
		                std::to_string(u.usernum) + "|" + std::to_string(u.last_call) + "|" +
		                std::to_string(u.times_called) + "|" + std::to_string(u.num_posts));
	}
	WriteListing(stream, lines, "");
}

void HandleEnt0(Connection &con, Session &s, net::ClientStream &stream, const std::vector<std::string> &p) {
	if (!s.authed) {
		stream.WriteLine("530 You must log in first.");
		return;
	}
	if (!s.have_room) {
		stream.WriteLine("540 Not in a room; use GOTO first.");
		return;
	}
	// ENT0 with a 0 first field is the client asking "may I post here?" — and
	// until now the answer was an unconditional yes, with no check on the way in
	// either. Both paths go through the shared predicate now.
	if (!citadel::CanPost(con, s.username, s.room)) {
		stream.WriteLine("550 You may not enter messages in this room.");
		return;
	}
	int post = (int)ToInt(Field(p, 0), 0);
	if (post == 0) {
		stream.WriteLine("200 Ok to post here.");
		return;
	}
	stream.WriteLine("400 Enter message; terminate with '000' on a line by itself.");

	std::string body;
	std::string line;
	while (stream.ReadLine(line, 8192)) {
		if (line == "000") {
			break;
		}
		if (body.size() + line.size() + 1 > kMaxPostBytes) {
			stream.WriteLine("550 Message too large.");
			return;
		}
		body += line;
		body += '\n';
	}

	citadel::Message msg;
	msg.author = s.username;
	msg.author_usernum = s.usernum;
	msg.recipient = Field(p, 1);
	msg.msgtime = NowEpoch();
	msg.format_type = 0;
	msg.subject = Field(p, 4);
	msg.euid = Field(p, 9);
	msg.references = Field(p, 11);
	msg.origin_room = s.room.display_name;
	msg.raw = body;

	std::vector<int64_t> rooms = {s.room.room_num};
	// Personal mail addressed to another user: also drop into their Mail room.
	if ((s.room.mailbox_owner > 0) && !msg.recipient.empty()) {
		int64_t rcpt_room = citadel::GetOrCreateMailRoom(con, msg.recipient);
		if (rcpt_room >= 0) {
			rooms.push_back(rcpt_room);
		}
	}

	std::string err;
	int64_t msgnum = citadel::InsertMessage(con, msg, rooms, err);
	if (msgnum < 0) {
		stream.WriteLine("550 " + err);
		return;
	}
	stream.WriteLine("200 " + std::to_string(msgnum));
}

void HandleCre8(Connection &con, Session &s, net::ClientStream &stream, const std::vector<std::string> &p) {
	if (!s.authed) {
		stream.WriteLine("530 You must log in first.");
		return;
	}
	int create = (int)ToInt(Field(p, 0), 0);
	std::string name = Field(p, 1);
	int access = (int)ToInt(Field(p, 2), 0);
	std::string password = Field(p, 3);
	int64_t floor = ToInt(Field(p, 4), 0);
	if (create == 0) {
		stream.WriteLine("200 Ok to create.");
		return;
	}
	if (name.empty()) {
		stream.WriteLine("500 Room name required.");
		return;
	}
	int64_t qr_flags = 0;
	int64_t owner = 0;
	switch (access) {
	case 1:
		qr_flags |= citadel::QR_GUESSNAME;
		break;
	case 2:
		qr_flags |= citadel::QR_PASSWORDED;
		break;
	case 3:
		qr_flags |= citadel::QR_PRIVATE;
		break;
	case 4:
		qr_flags |= citadel::QR_MAILBOX | citadel::QR_PRIVATE;
		owner = s.usernum;
		break;
	default:
		break;
	}
	std::string err;
	int64_t num = citadel::CreateRoom(con, name, floor, qr_flags, password, owner, err);
	if (num < 0) {
		stream.WriteLine("574 " + err);
		return;
	}
	stream.WriteLine("200 Room created.");
}

void HandleSetr(Connection &con, Session &s, net::ClientStream &stream, const std::vector<std::string> &p) {
	if (!s.have_room) {
		stream.WriteLine("540 Not in a room.");
		return;
	}
	std::string name = Field(p, 0);
	std::string password = Field(p, 1);
	int64_t flags = ToInt(Field(p, 3), s.room.qr_flags);
	int64_t floor = ToInt(Field(p, 5), s.room.floor_num);
	int64_t listorder = ToInt(Field(p, 6), s.room.listorder);
	int64_t defview = ToInt(Field(p, 7), s.room.default_view);
	auto stmt = con.Prepare("UPDATE citadel_rooms SET display_name=$1, password=$2, qr_flags=$3, floor_num=$4, "
	                        "listorder=$5, default_view=$6 WHERE room_num=$7");
	if (stmt->HasError()) {
		stream.WriteLine("550 " + stmt->GetError());
		return;
	}
	duckdb::vector<Value> params = {Value(name.empty() ? s.room.display_name : name),
	                                Value(password),
	                                Value::BIGINT(flags),
	                                Value::BIGINT(floor),
	                                Value::BIGINT(listorder),
	                                Value::BIGINT(defview),
	                                Value::BIGINT(s.room.room_num)};
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		stream.WriteLine("550 " + r->GetError());
		return;
	}
	// Refresh the cached current room.
	citadel::GetRoomByNum(con, s.room.room_num, s.room);
	stream.WriteLine("200 Room saved.");
}

std::vector<std::string> InfoLines(Connection &con) {
	std::string node = citadel::GetConfig(con, "c_nodename", "quackcit");
	std::string human = citadel::GetConfig(con, "c_humannode", "QuackCit BBS");
	std::string fqdn = citadel::GetConfig(con, "c_fqdn", "quackmail.test");
	std::string city = citadel::GetConfig(con, "c_bbs_city", "The Cloud");
	std::string admin = citadel::GetConfig(con, "c_sysadm", "admin");
	std::string ver = citadel::GetConfig(con, "c_version", "QuackCit 0.1.0");
	return {
	    "1",        // session id
	    node,       // node name
	    human,      // human-readable node name
	    fqdn,       // fully-qualified domain name
	    "QuackCit", // server software
	    "951",      // software revision level
	    city,       // geographic location
	    admin,      // system administrator
	    "0",        // server type code
	    "Lobby",    // default landing room
	    "1",        // floors enabled
	    "0",        // paging level
	    "0",        // ok to send express messages
	    ver,        // full version string
	};
}

// ---- presence + instant messages (tables are the bus) --------------------

void ExecParams(Connection &con, const std::string &sql, duckdb::vector<Value> params) {
	auto stmt = con.Prepare(sql);
	if (!stmt->HasError()) {
		stmt->Execute(params, false);
	}
}

// Presence is core's (citadel::RegisterSession/TouchSession/UnregisterSession)
// so every front-end writes the same columns; this file only adapts the Session
// struct to the shared signature.
void TouchSession(Connection &con, const Session &s, const std::string &last_cmd) {
	citadel::TouchSession(con, s.session_id, s.username,
	                      s.have_room ? s.room.display_name : std::string(), last_cmd, s.axlevel);
}

// RWHO listing line: session|user|room|host|client|idletime|lastcmd|.||||flags|axlevel
//
// The "idletime" field carries the absolute last-command timestamp, not a
// duration — that is what a real Citadel server emits (test/parity/
// real_citadel/native_extra.txt), and an empty host is legitimate there too.
std::vector<std::string> WhoLines(Connection &con) {
	std::vector<std::string> out;
	for (const auto &s : citadel::ListSessions(con)) {
		std::string user = s.username.empty() ? "(not logged in)" : s.username;
		out.push_back(std::to_string(s.session_id) + "|" + user + "|" + s.room + "|" + s.host + "|" +
		              s.client + "|" + std::to_string(s.last_seen) + "|" + s.last_cmd + "|.||||1|" +
		              std::to_string(s.axlevel));
	}
	return out;
}

int64_t ExpressCount(Connection &con, const std::string &user) {
	if (user.empty()) {
		return 0;
	}
	auto stmt = con.Prepare(
	    "SELECT count(*) FROM citadel_express WHERE lower(to_user)=lower($1) AND delivered=false");
	if (stmt->HasError()) {
		return 0;
	}
	duckdb::vector<Value> p = {Value(user)};
	auto r = stmt->Execute(p, false);
	if (r->HasError()) {
		return 0;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	return mat.RowCount() ? mat.GetValue(0, 0).GetValue<int64_t>() : 0;
}

void HandleCitadel(DatabaseInstance &db, net::ClientStream &stream) {
	Connection con(db);
	store::EnsureSchema(con);

	Session s;
	s.session_id = citadel::RegisterSession(con, "Citadel client protocol", stream.PeerIp());
	std::string node = citadel::GetConfig(con, "c_nodename", "quackcit");
	// Greeting must match a real Citadel server's format ("200 <node> Citadel
	// server ready.") — the official text client parses this line and rejects a
	// pipe-delimited variant. Node/version details are exposed via INFO instead.
	stream.WriteLine("200 " + node + " Citadel server ready.");

	std::string line;
	while (stream.ReadLine(line, 8192)) {
		std::string verb, rest;
		SplitCommand(line, verb, rest);
		auto p = SplitPipe(rest);

		if (verb.empty()) {
			continue;
		}
		if (verb == "NOOP") {
			stream.WriteLine("200 OK");
		} else if (verb == "ECHO") {
			stream.WriteLine("200 " + rest);
		} else if (verb == "IDEN") {
			stream.WriteLine("200 OK");
		} else if (verb == "QUIT") {
			stream.WriteLine("200 Goodbye.");
			break;
		} else if (verb == "LOUT") {
			int64_t sid = s.session_id;
			s = Session();
			s.session_id = sid;
			stream.WriteLine("200 Logged out.");
		} else if (verb == "USER") {
			s.pending_user = rest;
			stream.WriteLine("300 Password required for " + rest + ".");
		} else if (verb == "PASS") {
			if (s.pending_user.empty()) {
				stream.WriteLine("530 Send USER first.");
			} else if (auth::Verify(con, s.pending_user, rest)) {
				CompleteLogin(con, s, s.pending_user);
				stream.WriteLine(LoginLine(con, s));
			} else {
				stream.WriteLine("500 Wrong password.");
			}
		} else if (verb == "NEWU") {
			std::string name = rest;
			if (name.empty()) {
				stream.WriteLine("500 Username required.");
			} else if (auth::Verify(con, name, "") || citadel::GetOrAssignUserNum(con, name) > 0) {
				stream.WriteLine("570 That user already exists.");
			} else {
				std::string err;
				if (!auth::AddUser(con, name, "", err)) {
					stream.WriteLine("550 " + err);
				} else {
					CompleteLogin(con, s, name);
					citadel::PostAideMessage(con, "New user: " + name,
					                         "A new account was created from the Citadel client "
					                         "protocol.\n\nUser: " + name + "\n");
					stream.WriteLine(LoginLine(con, s));
				}
			}
		} else if (verb == "SETP") {
			if (!s.authed) {
				stream.WriteLine("530 You must log in first.");
			} else {
				std::string err;
				if (auth::AddUser(con, s.username, rest, err)) {
					stream.WriteLine("200 Password changed.");
				} else {
					stream.WriteLine("550 " + err);
				}
			}
		} else if (verb == "LKRA" || verb == "LKRN" || verb == "LKRO") {
			int64_t floor = rest.empty() ? -1 : ToInt(Field(p, 0), -1);
			std::string which = verb == "LKRN" ? "new" : (verb == "LKRO" ? "old" : "all");
			auto rooms = citadel::ListRooms(con, s.username, floor, which);
			std::vector<std::string> lines;
			for (auto &r : rooms) {
				lines.push_back(RoomListLine(r));
			}
			// Each of the three has its own wording in serv_rooms.c.
			std::string preamble = verb == "LKRN"   ? "Rooms with new msgs:"
			                       : verb == "LKRO" ? "Rooms with no new msgs:"
			                                        : "Known rooms:";
			WriteListing(stream, lines, preamble);
		} else if (verb == "LFLR") {
			auto floors = citadel::ListFloors(con);
			std::vector<std::string> lines;
			for (auto &f : floors) {
				lines.push_back(std::to_string(f.floor_num) + "|" + f.name + "|" +
				                std::to_string(f.room_count));
			}
			WriteListing(stream, lines, "Known floors:");
		} else if (verb == "CFLR") {
			if (s.axlevel < 6) {
				stream.WriteLine("500 Higher access required.");
			} else {
				std::string err;
				int64_t num = citadel::CreateFloor(con, Field(p, 0), err);
				stream.WriteLine(num >= 0 ? ("200 " + std::to_string(num)) : ("500 " + err));
			}
		} else if (verb == "GOTO") {
			HandleGoto(con, s, stream, p);
		} else if (verb == "MSGS") {
			HandleMsgs(con, s, stream, p);
		} else if (verb == "MSG0" || verb == "MSG3" || verb == "MSG4") {
			HandleMsg0(con, stream, p);
		} else if (verb == "MSG2") {
			HandleMsg2(con, stream, p);
		} else if (verb == "ENT0") {
			HandleEnt0(con, s, stream, p);
		} else if (verb == "CRE8") {
			HandleCre8(con, s, stream, p);
		} else if (verb == "KILL") {
			if (!s.have_room) {
				stream.WriteLine("540 Not in a room.");
			} else if ((int)ToInt(Field(p, 0), 0) == 0) {
				stream.WriteLine("200 Ok to delete.");
			} else {
				std::string err;
				if (citadel::KillRoom(con, s.room.room_num, err)) {
					s.have_room = false;
					stream.WriteLine("200 Room deleted.");
				} else {
					stream.WriteLine("550 " + err);
				}
			}
		} else if (verb == "GETR") {
			if (!s.have_room) {
				stream.WriteLine("540 Not in a room.");
			} else {
				auto &r = s.room;
				stream.WriteLine("200 " + r.display_name + "|" + r.password + "||" +
				                 std::to_string(r.qr_flags) + "|" + std::to_string(r.floor_num) + "|" +
				                 std::to_string(r.listorder) + "|" + std::to_string(r.default_view) + "|0");
			}
		} else if (verb == "SETR") {
			HandleSetr(con, s, stream, p);
		} else if (verb == "WIKI") {
			HandleWiki(con, s, stream, p);
		} else if (verb == "RINF") {
			if (s.have_room && !s.room.info.empty()) {
				WriteListing(stream, {s.room.info}, "Info:");
			} else {
				stream.WriteLine("500 No info file for this room.");
			}
		} else if (verb == "SLRP") {
			if (!s.have_room) {
				stream.WriteLine("540 Not in a room.");
			} else {
				auto st = citadel::GetRoomStats(con, s.username, s.room.room_num);
				int64_t n = util::Upper(Field(p, 0)) == "HIGHEST" ? st.highest : ToInt(Field(p, 0), st.highest);
				citadel::SetLastRead(con, s.username, s.room.room_num, n);
				stream.WriteLine("200 " + std::to_string(n));
			}
		} else if (verb == "MESG") {
			// The client requests named system banners (e.g. "MESG hello" for the
			// login banner) during handshake; a real server returns the text.
			// Returning a banner here avoids a spurious "unrecognized command".
			std::string human = citadel::GetConfig(con, "c_humannode", "QuackCit BBS");
			// serv_file.c echoes the requested banner's name back in the
			// preamble, so "MESG hello" answers "100 hello".
			WriteListing(stream, {"Welcome to " + human + "!", "",
			                      "You are connected to a QuackCit server speaking the Citadel protocol."},
			             rest.empty() ? std::string("hello") : rest);
		} else if (verb == "TIME") {
			stream.WriteLine(TimeLine());
		} else if (verb == "RWHO") {
			WriteListing(stream, WhoLines(con), " "); // serv_rwho.c: code, express flag, space
		} else if (verb == "SEXP" || verb == "SEND_EXPRESS") {
			// SEXP <recipient>|<message text>  (page a user an instant message)
			if (!s.authed) {
				stream.WriteLine("530 You must log in first.");
			} else {
				std::string to = Field(p, 0);
				std::string text = Field(p, 1);
				if (to.empty()) {
					stream.WriteLine("500 Recipient required.");
				} else if (citadel::GetOrAssignUserNum(con, to) <= 0) {
					stream.WriteLine("550 No such user.");
				} else {
					auto num = con.Query("SELECT nextval('citadel_express_seq')");
					int64_t id = num->HasError() ? 0 : num->GetValue(0, 0).GetValue<int64_t>();
					ExecParams(con,
					           "INSERT INTO citadel_express (id, to_user, from_user, text, sent_at) "
					           "VALUES ($1, $2, $3, $4, $5)",
					           {Value::BIGINT(id), Value(to), Value(s.username), Value(text),
					            Value::BIGINT(NowEpoch())});
					stream.WriteLine("200 Message sent.");
				}
			}
		} else if (verb == "GEXP") {
			// Retrieve and clear this user's pending instant messages.
			if (!s.authed) {
				stream.WriteLine("530 You must log in first.");
			} else {
				auto stmt = con.Prepare("SELECT id, from_user, text, sent_at FROM citadel_express "
				                        "WHERE lower(to_user)=lower($1) AND delivered=false ORDER BY id");
				duckdb::vector<Value> pr = {Value(s.username)};
				auto r = stmt->HasError() ? nullptr : stmt->Execute(pr, false);
				if (!r || r->HasError() || r->Cast<MaterializedQueryResult>().RowCount() == 0) {
					stream.WriteLine("511 No messages waiting.");
				} else {
					auto &mat = r->Cast<MaterializedQueryResult>();
					// Deliver the oldest message; header line then body listing.
					int64_t id = mat.GetValue(0, 0).GetValue<int64_t>();
					std::string from = mat.GetValue(1, 0).ToString();
					std::string text = mat.GetValue(2, 0).ToString();
					int64_t remaining = (int64_t)mat.RowCount() - 1;
					stream.WriteLine("100 " + std::to_string(remaining) + "|" + std::to_string(NowEpoch()) +
					                 "|" + from + "|0|");
					stream.WriteLine(text);
					stream.WriteLine("000");
					ExecParams(con, "UPDATE citadel_express SET delivered=true WHERE id=$1", {Value::BIGINT(id)});
				}
			}
		} else if (verb == "CHEK") {
			// Client status poll: newmail|regis_needed|express_waiting|username
			int64_t express = ExpressCount(con, s.username);
			stream.WriteLine("200 0|0|" + std::to_string(express) + "|" + s.username);
		} else if (verb == "DELE") {
			// Delete a message from the current room (distinct from KILL = delete room).
			if (!s.have_room) {
				stream.WriteLine("540 Not in a room.");
			} else {
				int64_t msgnum = ToInt(Field(p, 0), -1);
				std::string err;
				if (!citadel::DeleteMessage(con, s.room.room_num, msgnum, err)) {
					stream.WriteLine("550 " + err);
				} else {
					stream.WriteLine("200 Message deleted.");
				}
			}
		} else if (verb == "MOVE") {
			// MOVE <msgnum>|<target_room>|<is_copy>  — move/copy a message.
			if (!s.have_room) {
				stream.WriteLine("540 Not in a room.");
			} else {
				int64_t msgnum = ToInt(Field(p, 0), -1);
				citadel::Room target;
				bool is_copy = ToInt(Field(p, 2), 0) != 0;
				std::string err;
				if (!citadel::ResolveRoom(con, s.username, Field(p, 1), target)) {
					stream.WriteLine("550 No such room.");
				} else if (!citadel::MoveMessage(con, s.room.room_num, target.room_num, msgnum, is_copy, err)) {
					stream.WriteLine("550 " + err);
				} else {
					stream.WriteLine("200 Message " + std::string(is_copy ? "copied." : "moved."));
				}
			}
		} else if (verb == "REGI") {
			HandleRegi(con, s, stream);
		} else if (verb == "GREG") {
			HandleGreg(con, s, stream, p);
		} else if (verb == "EBIO") {
			HandleEbio(con, s, stream);
		} else if (verb == "RBIO") {
			HandleRbio(con, s, stream, p);
		} else if (verb == "LIST") {
			HandleList(con, s, stream);
		} else if (verb == "INFO") {
			WriteListing(stream, InfoLines(con), "Server info:");
		} else if (verb == "MSGP") {
			// Client message-format preference hint (which MIME types it wants
			// decoded). We deliver messages as-is and let the client decode, so
			// acknowledge without storing prefs. Text clients abort login if
			// this returns non-2xx.
			stream.WriteLine("200 OK");
		} else {
			stream.WriteLine("500 Unrecognized or unsupported command.");
		}
		// Update presence after the command so RWHO reflects the post-login user
		// and the room just entered.
		TouchSession(con, s, verb);
	}
	citadel::UnregisterSession(con, s.session_id);
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "cit", 5040, g_citadel, HandleCitadel);
}

} // namespace

void QuackmailCitadelExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailCitadelExtension::Name() {
	return "quackmail_citadel";
}
std::string QuackmailCitadelExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_CITADEL
	return EXT_VERSION_QUACKMAIL_CITADEL;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_citadel, loader) {
	duckdb::LoadInternal(loader);
}
}
