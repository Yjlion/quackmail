#define DUCKDB_EXTENSION_MAIN

#include "quackmail_nntp_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/citadel_msg.hpp"
#include "quackmail/citadel_store.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/util.hpp"
#include "quackmail/wildmat.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace duckdb {
namespace {

using namespace quackmail;

// nntp (119; dev 1119, STARTTLS) and nntps (563; dev 1563, implicit TLS) — the
// same pair a real Citadel server listens on. Rooms are newsgroups; the room's
// message pointers are the article numbers.
ServerController g_nntp;
ServerController g_nntps;

constexpr size_t kMaxArticleBytes = 4 * 1024 * 1024;

int64_t NowEpoch() {
	return (int64_t)std::time(nullptr);
}

struct Nntp {
	bool authed = false;
	std::string username;
	std::string pending_user;

	bool have_group = false;
	citadel::Room room;
	std::vector<int64_t> articles;    // room message numbers, ascending
	int64_t current_article = 0;
};

// A word of a space-delimited command line ("" past the end).
std::string Word(const std::string &line, size_t n) {
	size_t start = 0;
	for (size_t i = 0; i <= n; i++) {
		size_t sp = line.find(' ', start);
		if (i == n) {
			return sp == std::string::npos ? line.substr(start) : line.substr(start, sp - start);
		}
		if (sp == std::string::npos) {
			return "";
		}
		start = sp + 1;
	}
	return "";
}

void WriteDotStuffed(net::ClientStream &stream, const std::string &text) {
	std::string line;
	auto flush = [&]() {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		stream.WriteLine(!line.empty() && line[0] == '.' ? "." + line : line);
		line.clear();
	};
	for (char c : text) {
		if (c == '\n') {
			flush();
		} else {
			line.push_back(c);
		}
	}
	if (!line.empty()) {
		flush();
	}
	stream.WriteLine(".");
}

// The rooms this session may see, as newsgroups.
std::vector<citadel::Room> VisibleRooms(Connection &con, const Nntp &s) {
	return citadel::ListRooms(con, s.username, -1, "all");
}

// Article numbers in a room, ascending.
std::vector<int64_t> RoomArticles(Connection &con, int64_t room_num) {
	return citadel::RoomMessages(con, room_num, "all", 0, 0);
}

// Posting is allowed for an authenticated user in a writable room. A real
// Citadel server hardcodes "n" here because it has no NNTP posting at all; we
// do, so this reports the truth.
bool CanPost(const Nntp &s, const citadel::Room &room) {
	if (!s.authed || (room.qr_flags & citadel::QR_READONLY)) {
		return false;
	}
	return true;
}

// "<low>-<high>" / "<num>-" / "<num>" ranges used by LISTGROUP, OVER and XOVER.
void ParseRange(const std::string &spec, int64_t &lo, int64_t &hi) {
	lo = 0;
	hi = 0;
	if (spec.empty()) {
		return;
	}
	lo = std::atoll(spec.c_str());
	size_t dash = spec.find('-');
	if (dash == std::string::npos) {
		hi = lo;
	} else {
		hi = std::atoll(spec.c_str() + dash + 1);
		if (hi == 0) {
			hi = INT64_MAX;
		}
		if (hi < lo) {
			hi = lo;
		}
	}
}

std::string MsgId(Connection &con, int64_t msgnum, const std::string &node) {
	citadel::Message msg;
	if (!citadel::LoadMessage(con, msgnum, msg)) {
		return "<0@" + node + ">";
	}
	return citadel::MessageId(msg, node);
}

// ------------------------------------------------------------------ LIST

void HandleList(Connection &con, Nntp &s, net::ClientStream &stream, const std::string &line) {
	std::string format = util::Upper(Word(line, 1));
	std::string pattern = Word(line, 2);

	if (format == "OVERVIEW.FMT") {
		stream.WriteLine("215 Order of fields in overview database.");
		for (const char *f : {"Subject:", "From:", "Date:", "Message-ID:", "References:", "Bytes:", "Lines:"}) {
			stream.WriteLine(f);
		}
		stream.WriteLine(".");
		return;
	}
	if (!format.empty() && format != "ACTIVE" && format != "NEWSGROUPS") {
		stream.WriteLine("501 syntax error , unsupported list format");
		return;
	}

	stream.WriteLine("215 list of newsgroups follows");
	for (auto &room : VisibleRooms(con, s)) {
		std::string group = citadel::RoomToNewsgroup(room.name);
		if (!pattern.empty() && !WildmatMatch(group, pattern)) {
			continue;
		}
		if (format == "NEWSGROUPS") {
			stream.WriteLine(group + " " + room.name);
			continue;
		}
		auto nums = RoomArticles(con, room.room_num);
		int64_t low = nums.empty() ? 0 : nums.front();
		int64_t high = nums.empty() ? 0 : nums.back();
		stream.WriteLine(group + " " + std::to_string(high) + " " + std::to_string(low) + " " +
		                 (CanPost(s, room) ? "y" : "n"));
	}
	stream.WriteLine(".");
}

// --------------------------------------------------------- GROUP/LISTGROUP

void HandleGroup(Connection &con, Nntp &s, net::ClientStream &stream, const std::string &line) {
	bool listgroup = util::Upper(Word(line, 0)) == "LISTGROUP";
	std::string group = Word(line, 1);
	if (group.empty() && listgroup && s.have_group) {
		group = citadel::RoomToNewsgroup(s.room.name);
	}

	std::string room_name = citadel::NewsgroupToRoom(group);
	citadel::Room room;
	bool found = citadel::ResolveRoom(con, s.username, room_name, room);
	if (!found) {
		// Mailbox rooms are stored as "<usernum>.<name>"; a client may ask for
		// either that or the bare room name.
		size_t dot = room_name.find('.');
		if (dot != std::string::npos) {
			found = citadel::ResolveRoom(con, s.username, room_name.substr(dot + 1), room);
		}
	}
	if (!found) {
		stream.WriteLine("411 no such newsgroup");
		return;
	}

	s.room = room;
	s.have_group = true;
	s.articles = RoomArticles(con, room.room_num);
	int64_t low = s.articles.empty() ? 0 : s.articles.front();
	int64_t high = s.articles.empty() ? 0 : s.articles.back();
	if (s.articles.empty()) {
		low = -1;
		high = -1;
	}
	s.current_article = s.articles.empty() ? 0 : s.articles.front();
	stream.WriteLine("211 " + std::to_string(s.articles.size()) + " " + std::to_string(low) + " " +
	                 std::to_string(high) + " " + citadel::RoomToNewsgroup(room.name));

	if (listgroup) {
		int64_t lo, hi;
		ParseRange(Word(line, 2), lo, hi);
		for (int64_t n : s.articles) {
			if (n < lo || (hi != 0 && n > hi)) {
				continue;
			}
			stream.WriteLine(std::to_string(n));
		}
		stream.WriteLine(".");
	}
}

// ------------------------------------------------- ARTICLE/HEAD/BODY/STAT

void HandleArticle(Connection &con, Nntp &s, net::ClientStream &stream, const std::string &line,
                   const std::string &node) {
	std::string verb = util::Upper(Word(line, 0));
	std::string arg = Word(line, 1);

	if (!s.have_group && arg.find('<') == std::string::npos) {
		stream.WriteLine("412 no newsgroup has been selected");
		return;
	}

	int64_t msgnum = 0;
	bool move_pointer = false;
	if (arg.empty()) {
		if (s.current_article < 1) {
			stream.WriteLine("420 No current article selected");
			return;
		}
		msgnum = s.current_article;
		move_pointer = true;
	} else if (arg.front() == '<') {
		// Fetch by Message-ID. A real Citadel server answers "500 I don't know
		// how to fetch by message-id yet"; ours resolves it against the room.
		std::string want = arg;
		for (int64_t n : (s.have_group ? s.articles : std::vector<int64_t>())) {
			if (MsgId(con, n, node) == want) {
				msgnum = n;
				break;
			}
		}
		if (msgnum == 0) {
			stream.WriteLine("430 no such article found");
			return;
		}
	} else {
		msgnum = std::atoll(arg.c_str());
		move_pointer = true;
		if (std::find(s.articles.begin(), s.articles.end(), msgnum) == s.articles.end()) {
			stream.WriteLine("423 no article with that number");
			return;
		}
	}

	citadel::Message msg;
	if (!citadel::LoadMessage(con, msgnum, msg)) {
		stream.WriteLine("423 no article with that number");
		return;
	}
	if (move_pointer) {
		s.current_article = msgnum;
	}

	std::string id = citadel::MessageId(msg, node);
	std::string rfc822 = citadel::RenderRfc822(msg, node);
	size_t split = rfc822.find("\r\n\r\n");
	std::string headers = split == std::string::npos ? rfc822 : rfc822.substr(0, split + 2);
	std::string body = split == std::string::npos ? "" : rfc822.substr(split + 4);

	if (verb == "STAT") {
		stream.WriteLine("223 " + std::to_string(msgnum) + " " + id);
		return;
	}
	if (verb == "HEAD") {
		stream.WriteLine("221 " + std::to_string(msgnum) + " " + id);
		WriteDotStuffed(stream, headers);
		return;
	}
	if (verb == "BODY") {
		stream.WriteLine("222 " + std::to_string(msgnum) + " " + id);
		WriteDotStuffed(stream, body);
		return;
	}
	stream.WriteLine("220 " + std::to_string(msgnum) + " " + id);
	WriteDotStuffed(stream, rfc822);
}

void HandleLastNext(Connection &con, Nntp &s, net::ClientStream &stream, bool next,
                    const std::string &node) {
	if (!s.have_group) {
		stream.WriteLine("412 no newsgroup has been selected");
		return;
	}
	const char *err = next ? "421 no next article in this group" : "422 no previous article in this group";
	if (s.articles.empty() || s.current_article < 1) {
		stream.WriteLine(err);
		return;
	}
	auto it = std::find(s.articles.begin(), s.articles.end(), s.current_article);
	if (it == s.articles.end()) {
		stream.WriteLine(err);
		return;
	}
	if (next) {
		if (it + 1 == s.articles.end()) {
			stream.WriteLine(err);
			return;
		}
		s.current_article = *(it + 1);
	} else {
		if (it == s.articles.begin()) {
			stream.WriteLine(err);
			return;
		}
		s.current_article = *(it - 1);
	}
	stream.WriteLine("223 " + std::to_string(s.current_article) + " " + MsgId(con, s.current_article, node));
}

// ------------------------------------------------------------- OVER/XOVER

void HandleOver(Connection &con, Nntp &s, net::ClientStream &stream, const std::string &line,
                const std::string &node) {
	if (!s.have_group) {
		stream.WriteLine("412 no newsgroup has been selected");
		return;
	}
	int64_t lo, hi;
	ParseRange(Word(line, 1), lo, hi);
	if (lo <= 0) {
		lo = hi = s.current_article;
	}

	stream.WriteLine("224 Overview information follows");
	for (int64_t n : s.articles) {
		if (n < lo || (hi != 0 && n > hi)) {
			continue;
		}
		citadel::Message msg;
		if (!citadel::LoadMessage(con, n, msg)) {
			continue;
		}
		std::string rfc822 = citadel::RenderRfc822(msg, node);
		int64_t lines = (int64_t)std::count(rfc822.begin(), rfc822.end(), '\n');
		time_t t = (time_t)msg.msgtime;
		char when[32];
		struct tm tm {};
		localtime_r(&t, &tm);
		if (std::strftime(when, sizeof when, "%a %b %e %H:%M:%S %Y", &tm) == 0) {
			when[0] = 0;
		}
		// Subject, From, Date, Message-ID, References, :bytes, :lines. Citadel
		// hardcodes bytes/lines as 100/10; we report the real values.
		stream.WriteLine(std::to_string(n) + "\t" + msg.subject + "\t" + msg.author + " <" +
		                 util::LocalPart(msg.author) + "@" + node + ">\t" + when + "\t" +
		                 citadel::MessageId(msg, node) + "\t" + msg.references + "\t" +
		                 std::to_string((int64_t)rfc822.size()) + "\t" + std::to_string(lines));
	}
	stream.WriteLine(".");
}

// ------------------------------------------------------------------ POST

void HandlePost(Connection &con, Nntp &s, net::ClientStream &stream, const std::string &node) {
	if (!s.authed) {
		stream.WriteLine("440 posting not permitted");
		return;
	}
	stream.WriteLine("340 Send article to be posted");

	std::string article;
	if (!stream.ReadDotStuffed(article, kMaxArticleBytes)) {
		stream.WriteLine("441 posting failed: article too large or connection lost");
		return;
	}

	auto parsed = mime::Parse(article);
	auto header = [&parsed](const std::string &name) {
		for (auto &h : parsed.headers) {
			if (util::Upper(h.first) == util::Upper(name)) {
				return h.second;
			}
		}
		return std::string();
	};

	// Target rooms come from the Newsgroups: header, falling back to the
	// currently selected group.
	std::vector<std::string> groups;
	std::string ng = header("Newsgroups");
	if (ng.empty() && s.have_group) {
		ng = citadel::RoomToNewsgroup(s.room.name);
	}
	size_t start = 0;
	while (start <= ng.size() && !ng.empty()) {
		size_t comma = ng.find(',', start);
		std::string one = ng.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		while (!one.empty() && (one.front() == ' ' || one.front() == '\t')) {
			one.erase(0, 1);
		}
		while (!one.empty() && (one.back() == ' ' || one.back() == '\t' || one.back() == '\r')) {
			one.pop_back();
		}
		if (!one.empty()) {
			groups.push_back(one);
		}
		if (comma == std::string::npos) {
			break;
		}
		start = comma + 1;
	}
	if (groups.empty()) {
		stream.WriteLine("441 posting failed: no newsgroup specified");
		return;
	}

	std::vector<int64_t> rooms;
	for (auto &g : groups) {
		citadel::Room room;
		if (!citadel::ResolveRoom(con, s.username, citadel::NewsgroupToRoom(g), room)) {
			stream.WriteLine("441 posting failed: no such newsgroup " + g);
			return;
		}
		if (!CanPost(s, room)) {
			stream.WriteLine("440 posting not permitted to " + g);
			return;
		}
		rooms.push_back(room.room_num);
	}

	citadel::Message msg;
	msg.author = s.username;
	msg.author_usernum = citadel::GetOrAssignUserNum(con, s.username);
	msg.msgtime = NowEpoch();
	msg.format_type = 4; // the article is stored as the RFC822 bytes we received
	msg.subject = parsed.subject.empty() ? header("Subject") : parsed.subject;
	msg.references = header("References");
	msg.node = header("Path");
	msg.origin_room = rooms.empty() ? "" : s.room.display_name;
	msg.raw = article;

	std::string err;
	int64_t msgnum = citadel::InsertMessage(con, msg, rooms, err);
	if (msgnum < 0) {
		stream.WriteLine("441 posting failed: " + err);
		return;
	}
	// Keep the session's view of the group current.
	if (s.have_group) {
		s.articles = RoomArticles(con, s.room.room_num);
	}
	stream.WriteLine("240 Article received OK");
}

// ---------------------------------------------------------- command loop

void HandleNntp(DatabaseInstance &db, net::ClientStream &stream, ServerController &ctrl) {
	Connection con(db);
	store::EnsureSchema(con);

	std::string node = citadel::GetConfig(con, "c_fqdn", "quackcit");
	std::string version = citadel::GetConfig(con, "c_version", "QuackCit");

	Nntp s;
	// Unlike a real Citadel server (which cannot post over NNTP and greets with
	// "200 ... not finished yet"), posting is available here.
	stream.WriteLine("200 " + node + " NNTP QuackCit server ready (posting allowed)");

	std::string line;
	while (stream.ReadLine(line, 8192)) {
		std::string verb = util::Upper(Word(line, 0));
		if (verb.empty()) {
			continue;
		}

		if (verb == "QUIT") {
			stream.WriteLine("205 Goodbye...");
			return;
		} else if (verb == "HELP") {
			stream.WriteLine("100 This is the QuackCit NNTP service.");
			stream.WriteLine("RTFM http://www.ietf.org/rfc/rfc3977.txt");
			stream.WriteLine(".");
		} else if (verb == "DATE") {
			time_t now = std::time(nullptr);
			struct tm utc {};
			gmtime_r(&now, &utc);
			char buf[32];
			std::strftime(buf, sizeof buf, "%Y%m%d%H%M%S", &utc);
			stream.WriteLine("111 " + std::string(buf));
		} else if (verb == "CAPABILITIES") {
			stream.WriteLine("101 Capability list:");
			stream.WriteLine("IMPLEMENTATION " + version);
			stream.WriteLine("VERSION 2");
			stream.WriteLine("READER");
			stream.WriteLine("MODE-READER");
			stream.WriteLine("LIST ACTIVE NEWSGROUPS OVERVIEW.FMT");
			stream.WriteLine("OVER");
			stream.WriteLine("POST");
			if (!stream.IsTls() && ctrl.StartTlsEnabled()) {
				stream.WriteLine("STARTTLS");
			}
			if (!s.authed) {
				stream.WriteLine("AUTHINFO USER");
			}
			stream.WriteLine(".");
		} else if (verb == "STARTTLS") {
			if (stream.IsTls()) {
				stream.WriteLine("502 command unavailable");
			} else if (!ctrl.StartTlsEnabled()) {
				stream.WriteLine("580 can not initiate TLS negotiation");
			} else {
				stream.WriteLine("382 Begin TLS negotiation now");
				std::string terr;
				if (!stream.StartTls(ctrl.TlsCtx(), terr)) {
					return;
				}
			}
		} else if (verb == "MODE") {
			if (util::Upper(Word(line, 1)) == "READER") {
				// 200 (not Citadel's 201) because posting is permitted.
				stream.WriteLine("200 Reader mode, posting permitted");
			} else {
				stream.WriteLine("501 unknown mode");
			}
		} else if (verb == "AUTHINFO") {
			std::string sub = util::Upper(Word(line, 1));
			std::string arg = Word(line, 2);
			if (s.authed) {
				stream.WriteLine("482 Already logged in");
			} else if (sub == "USER") {
				if (citadel::GetOrAssignUserNum(con, arg) <= 0) {
					stream.WriteLine("481 " + arg + " not found");
				} else {
					s.pending_user = arg;
					stream.WriteLine("381 Password required for " + arg);
				}
			} else if (sub == "PASS") {
				if (s.pending_user.empty()) {
					stream.WriteLine("482 Authentication commands issued out of sequence");
				} else if (!auth::Verify(con, s.pending_user, arg)) {
					stream.WriteLine("481 Authentication failed");
				} else {
					s.authed = true;
					s.username = s.pending_user;
					s.pending_user.clear();
					citadel::EnsureUserRooms(con, s.username);
					stream.WriteLine("281 Authentication accepted");
				}
			} else {
				stream.WriteLine("501 syntax error");
			}
		} else if (!s.authed) {
			stream.WriteLine("480 authentication required");
		} else if (verb == "LIST") {
			HandleList(con, s, stream, line);
		} else if (verb == "NEWGROUPS") {
			// Every visible group, in LIST ACTIVE format. Room creation times
			// are not tracked, so the date argument cannot filter anything.
			stream.WriteLine("231 list of new newsgroups follows");
			for (auto &room : VisibleRooms(con, s)) {
				auto nums = RoomArticles(con, room.room_num);
				stream.WriteLine(citadel::RoomToNewsgroup(room.name) + " " +
				                 std::to_string(nums.empty() ? 0 : nums.back()) + " " +
				                 std::to_string(nums.empty() ? 0 : nums.front()) + " " +
				                 (CanPost(s, room) ? "y" : "n"));
			}
			stream.WriteLine(".");
		} else if (verb == "GROUP" || verb == "LISTGROUP") {
			HandleGroup(con, s, stream, line);
		} else if (verb == "ARTICLE" || verb == "HEAD" || verb == "BODY" || verb == "STAT") {
			HandleArticle(con, s, stream, line, node);
		} else if (verb == "NEXT" || verb == "LAST") {
			HandleLastNext(con, s, stream, verb == "NEXT", node);
		} else if (verb == "OVER" || verb == "XOVER") {
			HandleOver(con, s, stream, line, node);
		} else if (verb == "POST") {
			HandlePost(con, s, stream, node);
		} else {
			stream.WriteLine("500 I'm afraid I can't do that.");
		}
	}
}

void HandleNntpConn(DatabaseInstance &db, net::ClientStream &stream) {
	HandleNntp(db, stream, g_nntp);
}
void HandleNntpsConn(DatabaseInstance &db, net::ClientStream &stream) {
	HandleNntp(db, stream, g_nntps);
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "qm_nntp", 1119, g_nntp, HandleNntpConn);
	RegisterServerControls(loader, "qm_nntps", 1563, g_nntps, HandleNntpsConn);
}

} // namespace

void QuackmailNntpExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailNntpExtension::Name() {
	return "quackmail_nntp";
}
std::string QuackmailNntpExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_NNTP
	return EXT_VERSION_QUACKMAIL_NNTP;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_nntp, loader) {
	duckdb::LoadInternal(loader);
}
}
