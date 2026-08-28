#define DUCKDB_EXTENSION_MAIN

#include "quackmail_imap_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/citadel_msg.hpp"
#include "quackmail/citadel_store.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/quota.hpp"
#include "quackmail/sasl.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace {

using namespace quackmail;

// Two listeners over one implementation, mirroring Citadel's imap/imaps pair:
// STARTTLS-capable plaintext (143; dev 1143) and implicit TLS (993; dev 1993).
ServerController g_imap;
ServerController g_imaps;

// IMAP is deliberately a minimal but real subset: LOGIN, CAPABILITY, LIST,
// SELECT/EXAMINE, FETCH (FLAGS/UID/RFC822.SIZE/INTERNALDATE/ENVELOPE and
// BODY[]/[HEADER]/[TEXT]), STORE flags, EXPUNGE, CLOSE, LOGOUT, IDLE.
// BODYSTRUCTURE, CONDSTORE and friends are a later iteration.
struct Session {
	bool authed = false;
	std::string user;
	bool selected = false;
	bool read_only = false;
	int64_t room = -1;
	std::vector<int64_t> uids; // msgnum per 1-based sequence position
};

std::string ImapQuote(const std::string &s) {
	std::string out = "\"";
	for (char c : s) {
		if (c == '"' || c == '\\') {
			out.push_back('\\');
		}
		out.push_back(c);
	}
	out.push_back('"');
	return out;
}

std::string QOrNil(const std::string &s) {
	return s.empty() ? "NIL" : ImapQuote(s);
}

// Split a command's argument tail into atoms, honouring double-quoted strings.
// The ACL commands take up to three arguments, any of which may be quoted (a
// mailbox name like "Main Floor/Lobby" always is).
std::vector<std::string> ImapArgs(const std::string &in) {
	std::vector<std::string> out;
	size_t i = 0;
	while (i < in.size()) {
		while (i < in.size() && in[i] == ' ') {
			i++;
		}
		if (i >= in.size()) {
			break;
		}
		std::string tok;
		if (in[i] == '"') {
			for (i++; i < in.size() && in[i] != '"'; i++) {
				if (in[i] == '\\' && i + 1 < in.size()) {
					i++;
				}
				tok.push_back(in[i]);
			}
			if (i < in.size()) {
				i++; // closing quote
			}
		} else {
			for (; i < in.size() && in[i] != ' '; i++) {
				tok.push_back(in[i]);
			}
		}
		out.push_back(tok);
	}
	return out;
}

// Load the ordered msgnum list for a room.
std::vector<int64_t> RoomUids(Connection &con, int64_t room) {
	std::vector<int64_t> out;
	auto stmt = con.Prepare("SELECT m.msgnum FROM citadel_messages m "
	                        "JOIN citadel_room_msgs rm ON rm.msgnum = m.msgnum "
	                        "WHERE rm.room_num = $1 ORDER BY m.msgnum");
	if (stmt->HasError()) {
		return out;
	}
	duckdb::vector<Value> params = {Value::BIGINT(room)};
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		out.push_back(mat.GetValue(0, i).GetValue<int64_t>());
	}
	return out;
}

// Resolve an IMAP mailbox name to a Citadel room. Citadel exposes personal
// rooms under an "INBOX/" prefix (with the Mail room itself as "INBOX") and
// public rooms under their floor path ("<Floor>/<Room>"). We resolve by the
// final path segment, which uniquely identifies a room in the default set.
int64_t ResolveMailbox(Connection &con, const std::string &user, const std::string &name) {
	if (util::Upper(name) == "INBOX") {
		return citadel::GetOrCreateMailRoom(con, user);
	}
	std::string leaf = name;
	auto slash = name.rfind('/');
	if (slash != std::string::npos) {
		leaf = name.substr(slash + 1);
	}
	if (util::Upper(leaf) == "INBOX") {
		return citadel::GetOrCreateMailRoom(con, user);
	}
	citadel::Room room;
	if (citadel::ResolveRoom(con, user, leaf, room)) {
		return room.room_num;
	}
	return -1;
}

std::vector<std::string> LoadFlags(Connection &con, int64_t msgnum, const std::string &user) {
	std::vector<std::string> out;
	auto stmt = con.Prepare("SELECT flag FROM citadel_msg_flags WHERE msgnum = $1 AND username = $2");
	if (stmt->HasError()) {
		return out;
	}
	duckdb::vector<Value> params = {Value::BIGINT(msgnum), Value(user)};
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		out.push_back(mat.GetValue(0, i).ToString());
	}
	return out;
}

void AddFlag(Connection &con, int64_t msgnum, const std::string &user, const std::string &flag) {
	auto existing = LoadFlags(con, msgnum, user);
	for (auto &f : existing) {
		if (f == flag) {
			return;
		}
	}
	auto stmt = con.Prepare("INSERT INTO citadel_msg_flags (msgnum, username, flag) VALUES ($1, $2, $3)");
	if (stmt->HasError()) {
		return;
	}
	duckdb::vector<Value> params = {Value::BIGINT(msgnum), Value(user), Value(flag)};
	stmt->Execute(params, false);
}

std::string FlagsList(const std::vector<std::string> &flags) {
	std::string out = "(";
	for (size_t i = 0; i < flags.size(); i++) {
		out += (i ? " " : "") + flags[i];
	}
	out += ")";
	return out;
}

std::string InternalDate(int64_t epoch) {
	std::time_t t = (std::time_t)epoch;
	std::tm tm_utc {};
#if defined(_WIN32)
	gmtime_s(&tm_utc, &t);
#else
	gmtime_r(&t, &tm_utc);
#endif
	char buf[40];
	std::strftime(buf, sizeof(buf), "%d-%b-%Y %H:%M:%S +0000", &tm_utc);
	return std::string(buf);
}

// Split raw message into header block (through the blank line) and body.
void SplitHeaderBody(const std::string &raw, std::string &header, std::string &body) {
	size_t pos = raw.find("\r\n\r\n");
	size_t skip = 4;
	if (pos == std::string::npos) {
		pos = raw.find("\n\n");
		skip = 2;
	}
	if (pos == std::string::npos) {
		header = raw;
		body.clear();
	} else {
		header = raw.substr(0, pos + skip);
		body = raw.substr(pos + skip);
	}
}

// Format one ENVELOPE address-list field from a header value.
std::string EnvAddrs(const std::string &header_value) {
	if (header_value.empty()) {
		return "NIL";
	}
	auto addrs = mime::ParseAddressList(header_value);
	if (addrs.empty()) {
		return "NIL";
	}
	std::string out = "(";
	for (auto &a : addrs) {
		std::string local, domain;
		auto at = a.addr.find('@');
		if (at == std::string::npos) {
			local = a.addr;
		} else {
			local = a.addr.substr(0, at);
			domain = a.addr.substr(at + 1);
		}
		out += "(" + QOrNil(a.name) + " NIL " + QOrNil(local) + " " + QOrNil(domain) + ")";
	}
	out += ")";
	return out;
}

std::string HeaderVal(const mime::ParsedMessage &p, const std::string &name) {
	std::string want = util::Upper(name);
	for (auto &h : p.headers) {
		if (util::Upper(h.first) == want) {
			return h.second;
		}
	}
	return "";
}

std::string BuildEnvelope(const citadel::Message &msg, const std::string &rfc822) {
	auto p = mime::Parse(rfc822);
	std::string date = HeaderVal(p, "Date");
	std::string from = HeaderVal(p, "From");
	std::string sender = from;
	std::string reply = HeaderVal(p, "Reply-To");
	std::string to = HeaderVal(p, "To");
	std::string cc = HeaderVal(p, "Cc");
	std::string subj = msg.subject.empty() ? p.subject : msg.subject;
	std::string inreply = HeaderVal(p, "In-Reply-To");
	std::string msgid = msg.euid.empty() ? p.message_id : msg.euid;
	return "(" + QOrNil(date) + " " + QOrNil(subj) + " " + EnvAddrs(from) + " " + EnvAddrs(sender) + " " +
	       EnvAddrs(reply) + " " + EnvAddrs(to) + " " + EnvAddrs(cc) + " NIL " + QOrNil(inreply) + " " +
	       QOrNil(msgid) + ")";
}

// Expand a sequence set like "1,3:5,7:*" into 1-based positions (or, when
// is_uid, resolve UID numbers to positions). "*" is the last message.
std::vector<size_t> ParseSet(const std::string &set, const std::vector<int64_t> &uids, bool is_uid) {
	std::vector<size_t> out;
	size_t n = uids.size();
	auto emit_pos = [&](size_t pos1) {
		if (pos1 >= 1 && pos1 <= n) {
			out.push_back(pos1);
		}
	};
	auto uid_to_pos = [&](int64_t uid) -> size_t {
		for (size_t i = 0; i < n; i++) {
			if (uids[i] == uid) {
				return i + 1;
			}
		}
		return 0;
	};
	size_t start = 0;
	while (start <= set.size()) {
		size_t comma = set.find(',', start);
		std::string tok = set.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		if (!tok.empty()) {
			size_t colon = tok.find(':');
			if (colon == std::string::npos) {
				int64_t v = tok == "*" ? (is_uid ? (n ? uids[n - 1] : 0) : (int64_t)n) : std::atoll(tok.c_str());
				emit_pos(is_uid ? uid_to_pos(v) : (size_t)v);
			} else {
				std::string a = tok.substr(0, colon), b = tok.substr(colon + 1);
				int64_t lo = a == "*" ? (is_uid ? (n ? uids[n - 1] : 0) : (int64_t)n) : std::atoll(a.c_str());
				int64_t hi = b == "*" ? (is_uid ? (n ? uids[n - 1] : 0) : (int64_t)n) : std::atoll(b.c_str());
				if (is_uid) {
					if (lo > hi) {
						std::swap(lo, hi);
					}
					for (size_t i = 0; i < n; i++) {
						if (uids[i] >= lo && uids[i] <= hi) {
							out.push_back(i + 1);
						}
					}
				} else {
					if (lo > hi) {
						std::swap(lo, hi);
					}
					for (int64_t v = lo; v <= hi; v++) {
						emit_pos((size_t)v);
					}
				}
			}
		}
		if (comma == std::string::npos) {
			break;
		}
		start = comma + 1;
	}
	return out;
}

// Emit one FETCH response for the message at 1-based sequence position `pos`.
void FetchOne(Connection &con, Session &s, net::ClientStream &stream, size_t pos, const std::string &items_up,
              bool is_uid) {
	int64_t uid = s.uids[pos - 1];
	citadel::Message msg;
	if (!citadel::LoadMessage(con, uid, msg)) {
		return;
	}

	// Every byte IMAP serves is the RFC822 *view* of the message, not the
	// stored bytes. For a format 4 message the two are the same; for a native
	// (format 0) one — anything posted from the BBS, telnet, a Citadel client —
	// msg.raw is bare body text with no header block at all, so serving it
	// directly handed clients a message with no From, Subject or Date. POP3 and
	// NNTP have rendered this view for some time; this is IMAP catching up.
	std::string rfc822 = citadel::RenderRfc822(msg, citadel::GetConfig(con, "c_nodename", "quackcit"));

	bool want_flags = items_up.find("FLAGS") != std::string::npos;
	bool want_uid = is_uid || items_up.find("UID") != std::string::npos;
	bool want_size = items_up.find("RFC822.SIZE") != std::string::npos || items_up.find("FULL") != std::string::npos ||
	                 items_up.find("FAST") != std::string::npos || items_up.find("ALL") != std::string::npos;
	bool want_date = items_up.find("INTERNALDATE") != std::string::npos || items_up.find("FAST") != std::string::npos ||
	                 items_up.find("FULL") != std::string::npos || items_up.find("ALL") != std::string::npos;
	bool want_env = items_up.find("ENVELOPE") != std::string::npos || items_up.find("FULL") != std::string::npos ||
	                items_up.find("ALL") != std::string::npos;
	if (items_up.find("FAST") != std::string::npos || items_up.find("ALL") != std::string::npos ||
	    items_up.find("FULL") != std::string::npos) {
		want_flags = true;
	}

	// Body sections. Normalize "BODY.PEEK[...]" to "BODY[...]" for matching; the
	// PEEK only affects whether \Seen is set.
	bool peek = items_up.find(".PEEK") != std::string::npos;
	std::string sect = items_up;
	for (size_t p; (p = sect.find(".PEEK")) != std::string::npos;) {
		sect.erase(p, 5);
	}
	bool body_header = sect.find("BODY[HEADER]") != std::string::npos ||
	                   sect.find("RFC822.HEADER") != std::string::npos;
	bool body_text =
	    sect.find("BODY[TEXT]") != std::string::npos || sect.find("RFC822.TEXT") != std::string::npos;
	// Full message: "BODY[]" or a bare "RFC822" token (not RFC822.SIZE/.HEADER/.TEXT).
	bool body_full = sect.find("BODY[]") != std::string::npos;
	for (size_t p = 0; !body_full && (p = sect.find("RFC822", p)) != std::string::npos; p += 6) {
		char after = (p + 6 < sect.size()) ? sect[p + 6] : ' ';
		if (after != '.') {
			body_full = true;
		}
	}

	std::string parts;
	auto add = [&](const std::string &p) {
		if (!parts.empty()) {
			parts += " ";
		}
		parts += p;
	};

	if (want_flags) {
		add("FLAGS " + FlagsList(LoadFlags(con, uid, s.user)));
	}
	if (want_uid) {
		add("UID " + std::to_string(uid));
	}
	if (want_size) {
		add("RFC822.SIZE " + std::to_string(rfc822.size()));
	}
	if (want_date) {
		add("INTERNALDATE " + ImapQuote(InternalDate(msg.msgtime)));
	}
	if (want_env) {
		add("ENVELOPE " + BuildEnvelope(msg, rfc822));
	}

	// A body section is returned as a literal; assemble the leading part first,
	// then stream "{n}\r\n<bytes>".
	std::string section, payload;
	if (body_header) {
		std::string h, b;
		SplitHeaderBody(rfc822, h, b);
		section = sect.find("RFC822.HEADER") != std::string::npos ? "RFC822.HEADER" : "BODY[HEADER]";
		payload = h;
	} else if (body_text) {
		std::string h, b;
		SplitHeaderBody(rfc822, h, b);
		section = sect.find("RFC822.TEXT") != std::string::npos ? "RFC822.TEXT" : "BODY[TEXT]";
		payload = b;
	} else if (body_full) {
		section = sect.find("BODY[]") != std::string::npos ? "BODY[]" : "RFC822";
		payload = rfc822;
	}

	std::string head = "* " + std::to_string(pos) + " FETCH (" + parts;
	if (!section.empty()) {
		if (!parts.empty()) {
			head += " ";
		}
		head += section + " {" + std::to_string(payload.size()) + "}\r\n";
		stream.Write(head);
		stream.Write(payload);
		stream.Write(")\r\n");
		if (!peek) {
			AddFlag(con, uid, s.user, "\\Seen");
		}
	} else {
		stream.Write(head + ")\r\n");
	}
}

// ---- storage quota (RFC 9208) ---------------------------------------------
//
// STORAGE is counted in *kibibytes*, not bytes — RFC 2087 §3 and RFC 9208 §5.1
// both say "units of 1024 octets". Getting this wrong is the classic
// implementation bug: it reports a 10 MB quota as 10 GB and every client
// believes it.
//
// Usage rounds up and the limit rounds down, so the reported pair never claims
// a user has room they do not: one stored byte shows as 1, and a 1500-byte
// ceiling shows as 1 rather than as 2.
int64_t UsedKib(int64_t bytes) {
	return (bytes + 1023) / 1024;
}
int64_t LimitKib(int64_t bytes) {
	return bytes / 1024;
}

// "* QUOTA "" (STORAGE <used> <limit>)", or "" when this user has no ceiling.
//
// The quota root is the empty string, and there is exactly one: a quota here is
// citadel_rooms.mailbox_owner, and every one of a user's rooms has the same
// owner. "" is also what Dovecot's default rule uses, which makes it the
// best-tested value in the wild — a name like "User quota" is a Dovecot *rule*
// name and would send a client looking for a second root that does not exist.
std::string QuotaLine(Connection &con, const std::string &user) {
	auto info = quackmail::quota::Usage(con, user);
	if (!info.limited) {
		return std::string();
	}
	return "* QUOTA \"\" (STORAGE " + std::to_string(UsedKib(info.used_bytes)) + " " +
	       std::to_string(LimitKib(info.limit_bytes)) + ")";
}

std::string UnquoteMailbox(std::string v) {
	while (!v.empty() && v.back() == ' ') {
		v.pop_back();
	}
	while (!v.empty() && v.front() == ' ') {
		v.erase(v.begin());
	}
	if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
		v = v.substr(1, v.size() - 2);
	}
	return v;
}

void QuotaCommand(Connection &con, net::ClientStream &stream, const std::string &tag,
                  const std::string &cmd, const std::string &args, const std::string &user) {
	if (cmd == "GETQUOTAROOT") {
		std::string name = UnquoteMailbox(args);
		int64_t room = ResolveMailbox(con, user, name);
		if (room < 0) {
			stream.WriteLine(tag + " NO GETQUOTAROOT mailbox not found");
			return;
		}
		// A public room is owned by nobody, so it is in no quota root at all —
		// and neither is any mailbox of a user with no ceiling. The correct
		// answer in both cases is a QUOTAROOT line naming no roots, and no QUOTA
		// line to follow it.
		int64_t owner = 0;
		auto stmt = con.Prepare("SELECT mailbox_owner FROM citadel_rooms WHERE room_num = $1");
		if (!stmt->HasError()) {
			duckdb::vector<Value> params = {Value::BIGINT(room)};
			auto r = stmt->Execute(params, false);
			if (!r->HasError()) {
				auto &mat = r->Cast<MaterializedQueryResult>();
				if (mat.RowCount() > 0 && !mat.GetValue(0, 0).IsNull()) {
					owner = mat.GetValue(0, 0).GetValue<int64_t>();
				}
			}
		}
		std::string line = QuotaLine(con, user);
		bool mine = owner > 0 && owner == citadel::GetOrAssignUserNum(con, user);
		if (!mine || line.empty()) {
			stream.WriteLine("* QUOTAROOT " + ImapQuote(name));
		} else {
			stream.WriteLine("* QUOTAROOT " + ImapQuote(name) + " \"\"");
			stream.WriteLine(line);
		}
		stream.WriteLine(tag + " OK GETQUOTAROOT completed");
		return;
	}

	if (cmd == "GETQUOTA") {
		if (!UnquoteMailbox(args).empty()) {
			stream.WriteLine(tag + " NO No such quota root");
			return;
		}
		std::string line = QuotaLine(con, user);
		if (!line.empty()) {
			stream.WriteLine(line);
		}
		stream.WriteLine(tag + " OK GETQUOTA completed");
		return;
	}

	// SETQUOTA <root> (STORAGE <n>)
	//
	// RFC 9208 §4.1: a server SHOULD refuse this from a user who may not set
	// quotas. Aide is the same axlevel >= 6 threshold the web console uses.
	if (citadel::GetAxLevel(con, user) < 6) {
		stream.WriteLine(tag + " NO [NOPERM] Only an administrator may set a quota");
		return;
	}
	size_t lp = args.find('(');
	size_t rp = args.rfind(')');
	if (lp == std::string::npos || rp == std::string::npos || rp < lp) {
		stream.WriteLine(tag + " BAD SETQUOTA expects a resource list");
		return;
	}
	if (!UnquoteMailbox(args.substr(0, lp)).empty()) {
		stream.WriteLine(tag + " NO No such quota root");
		return;
	}
	auto items = ImapArgs(args.substr(lp + 1, rp - lp - 1));
	if (items.size() != 2 || util::Upper(items[0]) != "STORAGE") {
		// Silently ignoring a resource we do not implement would leave the client
		// believing it had set something.
		stream.WriteLine(tag + " NO Unsupported resource");
		return;
	}
	std::string err;
	if (!quackmail::quota::SetQuota(con, user, std::atoll(items[1].c_str()) * 1024, err)) {
		stream.WriteLine(tag + " NO " + err);
		return;
	}
	std::string line = QuotaLine(con, user);
	if (!line.empty()) {
		stream.WriteLine(line);
	}
	stream.WriteLine(tag + " OK SETQUOTA completed");
}

// The CAPABILITY token list. STARTTLS is advertised only before the TLS upgrade;
// mirrors a real Citadel server (which offers NAMESPACE, UIDPLUS, SASL, ID).
std::string CapabilityLine(bool tls_active, bool starttls_avail) {
	// QUOTA is the RFC 2087 spelling every deployed client recognises;
	// QUOTA=RES-STORAGE is what RFC 9208 §5 wants so a client learns which
	// resource types exist without probing for them.
	std::string caps = "IMAP4rev1 NAMESPACE ID UIDPLUS MOVE ACL IDLE QUOTA QUOTA=RES-STORAGE "
	                   "AUTH=PLAIN AUTH=LOGIN";
	if (!tls_active && starttls_avail) {
		caps += " STARTTLS";
	}
	return caps;
}

// IMAP mailbox wildcard match: '%' matches any run of non-delimiter chars, '*'
// matches anything (including the '/' delimiter). Case-insensitive on INBOX only,
// but we compare literally which is fine for our fixed name set.
bool ImapWildMatch(const std::string &pat, const std::string &name) {
	size_t pi = 0, ni = 0, star = std::string::npos, star_n = 0;
	while (ni < name.size()) {
		if (pi < pat.size() && (pat[pi] == name[ni] || pat[pi] == '%' || pat[pi] == '*')) {
			if (pat[pi] == '%' && name[ni] == '/') {
				// '%' does not cross the hierarchy delimiter.
				if (star == std::string::npos) {
					return false;
				}
				pi = star + 1;
				ni = ++star_n;
				continue;
			}
			if (pat[pi] == '*') {
				star = pi++;
				star_n = ni;
				continue;
			}
			pi++;
			ni++;
		} else if (star != std::string::npos) {
			pi = star + 1;
			ni = ++star_n;
		} else {
			return false;
		}
	}
	while (pi < pat.size() && (pat[pi] == '%' || pat[pi] == '*')) {
		pi++;
	}
	return pi == pat.size();
}

// One LIST/LSUB entry: attributes + IMAP mailbox name.
struct MailboxEntry {
	std::string attrs;
	std::string name;
};

// Build the mailbox tree the way Citadel presents it: the user's personal rooms
// under "INBOX"/"INBOX/<room>", and visible public rooms under their floor path
// "<Floor>" (a \NoSelect container) + "<Floor>/<Room>".
std::vector<MailboxEntry> BuildMailboxes(Connection &con, const std::string &user) {
	std::vector<MailboxEntry> out;
	int64_t usernum = citadel::GetOrAssignUserNum(con, user);
	bool is_aide = citadel::GetAxLevel(con, user) >= 6;

	// Personal rooms (mailbox_owner = usernum), ordered by display name so that
	// "Mail" (rendered INBOX) falls in its alphabetical slot, matching Citadel.
	auto pstmt = con.Prepare("SELECT display_name FROM citadel_rooms WHERE mailbox_owner = $1 "
	                         "ORDER BY display_name");
	if (!pstmt->HasError()) {
		duckdb::vector<Value> params = {Value::BIGINT(usernum)};
		auto r = pstmt->Execute(params, false);
		if (!r->HasError()) {
			auto &mat = r->Cast<MaterializedQueryResult>();
			for (idx_t i = 0; i < mat.RowCount(); i++) {
				std::string dn = mat.GetValue(0, i).ToString();
				out.push_back({"()", dn == "Mail" ? "INBOX" : "INBOX/" + dn});
			}
		}
	}

	// Visible public rooms grouped by floor. Private rooms are hidden from
	// non-aides (Citadel does the same: Aide/Global Address Book stay hidden).
	std::string psql = "SELECT f.name, r.display_name FROM citadel_rooms r "
	                   "JOIN citadel_floors f ON f.floor_num = r.floor_num "
	                   "WHERE r.mailbox_owner = 0";
	if (!is_aide) {
		psql += " AND (r.qr_flags & 4) = 0";
	}
	psql += " ORDER BY f.floor_num, r.display_name";
	std::vector<std::string> floors_seen;
	auto r = con.Query(psql);
	if (!r->HasError()) {
		for (idx_t i = 0; i < r->RowCount(); i++) {
			std::string floor = r->GetValue(0, i).ToString();
			std::string dn = r->GetValue(1, i).ToString();
			if (std::find(floors_seen.begin(), floors_seen.end(), floor) == floors_seen.end()) {
				floors_seen.push_back(floor);
				out.push_back({"(\\NoSelect \\HasChildren)", floor});
			}
			out.push_back({"()", floor + "/" + dn});
		}
	}
	return out;
}

// Tokenize an IMAP SEARCH criteria string (whitespace-split; quoted strings kept
// whole, parentheses become their own tokens).
std::vector<std::string> SearchTokens(const std::string &s) {
	std::vector<std::string> out;
	size_t i = 0;
	while (i < s.size()) {
		if (s[i] == ' ') {
			i++;
		} else if (s[i] == '(' || s[i] == ')') {
			out.push_back(std::string(1, s[i]));
			i++;
		} else if (s[i] == '"') {
			size_t j = i + 1;
			std::string v;
			while (j < s.size() && s[j] != '"') {
				v.push_back(s[j++]);
			}
			out.push_back(v);
			i = j + 1;
		} else {
			size_t j = i;
			while (j < s.size() && s[j] != ' ') {
				j++;
			}
			out.push_back(s.substr(i, j - i));
			i = j;
		}
	}
	return out;
}

// Evaluate one message against a SEARCH criteria token list. `idx1` is its 1-based
// position, `uid` its msgnum. Supports a practical RFC 3501 subset (flags, header
// field substrings, BODY/TEXT, SIZE, sequence/UID sets, NOT, ALL). Unknown keys
// are skipped conservatively (treated as matching) so clients still function.
bool SearchMatch(Connection &con, const std::string &user, size_t idx1, int64_t uid,
                 const std::vector<int64_t> &uids, const std::vector<std::string> &toks, size_t &pos,
                 const std::string &node) {
	auto has_flag = [&](const char *f) {
		auto fl = LoadFlags(con, uid, user);
		return std::find(fl.begin(), fl.end(), std::string(f)) != fl.end();
	};
	citadel::Message msg;
	bool loaded = false;
	auto load = [&]() -> citadel::Message & {
		if (!loaded) {
			citadel::LoadMessage(con, uid, msg);
			loaded = true;
		}
		return msg;
	};
	// The same RFC822 view FETCH serves, not the stored bytes: for a native
	// message msg.raw has no header block at all, so FROM/SUBJECT/HEADER could
	// never match and LARGER/SMALLER disagreed with the RFC822.SIZE FETCH
	// already reports for the same message.
	bool rendered = false;
	std::string rfc822;
	auto render = [&]() -> std::string & {
		if (!rendered) {
			rfc822 = citadel::RenderRfc822(load(), node);
			rendered = true;
		}
		return rfc822;
	};
	auto header_has = [&](const std::string &field, const std::string &needle) {
		auto p = mime::Parse(render());
		std::string want = util::Upper(field), nl = util::Upper(needle);
		for (auto &h : p.headers) {
			if (util::Upper(h.first) == want && util::Upper(h.second).find(nl) != std::string::npos) {
				return true;
			}
		}
		return false;
	};
	auto in_set = [&](const std::string &set, bool is_uid) {
		for (size_t p : ParseSet(set, uids, is_uid)) {
			if (p == idx1) {
				return true;
			}
		}
		return false;
	};

	bool ok = true; // AND of all criteria
	while (pos < toks.size() && toks[pos] != ")") {
		std::string k = util::Upper(toks[pos]);
		bool neg = false;
		if (k == "NOT") {
			neg = true;
			pos++;
			k = pos < toks.size() ? util::Upper(toks[pos]) : "";
		}
		bool m = true;
		if (k == "ALL" || k == "RECENT" || k == "NEW") {
			pos++;
		} else if (k == "ANSWERED") { m = has_flag("\\Answered"); pos++; }
		else if (k == "UNANSWERED") { m = !has_flag("\\Answered"); pos++; }
		else if (k == "SEEN") { m = has_flag("\\Seen"); pos++; }
		else if (k == "UNSEEN") { m = !has_flag("\\Seen"); pos++; }
		else if (k == "FLAGGED") { m = has_flag("\\Flagged"); pos++; }
		else if (k == "UNFLAGGED") { m = !has_flag("\\Flagged"); pos++; }
		else if (k == "DELETED") { m = has_flag("\\Deleted"); pos++; }
		else if (k == "UNDELETED") { m = !has_flag("\\Deleted"); pos++; }
		else if (k == "DRAFT") { m = has_flag("\\Draft"); pos++; }
		else if (k == "UNDRAFT") { m = !has_flag("\\Draft"); pos++; }
		else if (k == "OLD") { m = !has_flag("\\Recent"); pos++; }
		else if (k == "UID") { m = pos + 1 < toks.size() && in_set(toks[pos + 1], true); pos += 2; }
		else if (k == "FROM" || k == "TO" || k == "CC" || k == "BCC" || k == "SUBJECT") {
			m = pos + 1 < toks.size() && header_has(k, toks[pos + 1]);
			pos += 2;
		} else if (k == "HEADER") {
			m = pos + 2 < toks.size() && header_has(toks[pos + 1], toks[pos + 2]);
			pos += 3;
		} else if (k == "BODY" || k == "TEXT") {
			m = pos + 1 < toks.size() &&
			    util::Upper(render()).find(util::Upper(toks[pos + 1])) != std::string::npos;
			pos += 2;
		} else if (k == "LARGER") {
			m = pos + 1 < toks.size() && (int64_t)render().size() > std::atoll(toks[pos + 1].c_str());
			pos += 2;
		} else if (k == "SMALLER") {
			m = pos + 1 < toks.size() && (int64_t)render().size() < std::atoll(toks[pos + 1].c_str());
			pos += 2;
		} else if (!k.empty() && (isdigit((unsigned char)k[0]) || k == "*")) {
			m = in_set(toks[pos], false); // bare sequence set
			pos++;
		} else {
			pos++; // unknown key: skip (conservatively matches)
		}
		if (neg) {
			m = !m;
		}
		ok = ok && m;
	}
	return ok;
}

// ---- IDLE (RFC 2177) -----------------------------------------------------

// How often an idling session looks for changes, and how long it may idle
// before we make it come back.
//
// The wake-up is a **poll of the store**, not a push from whatever made the
// change. That is the shape this server is built for: extensions share no C++
// state and coordinate through tables, so the thing a second session wrote is
// exactly what this one reads back. `citadel::RoomChangeToken` is one cheap
// aggregate over two indexed columns, and reading it every couple of seconds is
// what turns polling *here* into push *to the client* — which is the half that
// matters, because the alternative is every phone on the site opening a fresh
// connection every few minutes to ask the same question.
constexpr int kIdlePollMs = 2000;
// RFC 2177 tells clients to re-issue IDLE at least every 29 minutes. A server
// that idles forever accumulates connections nothing will ever close, and each
// one costs a thread.
constexpr int64_t kIdleMaxSeconds = 29 * 60;

// Emit the untagged responses that bring the client's view of the selected
// mailbox up to date, and update the session's own sequence map with it.
//
// EXPUNGE goes first and in **descending** position order, because each one
// renumbers every message after it: a client applying ascending expunges
// removes the wrong messages. EXISTS goes last, and only when the count is not
// already what the client would have worked out from the expunges.
bool ReportChanges(Connection &con, Session &s, net::ClientStream &stream) {
	std::vector<int64_t> now = RoomUids(con, s.room);
	size_t removed = 0;
	for (size_t i = s.uids.size(); i > 0; i--) {
		if (std::find(now.begin(), now.end(), s.uids[i - 1]) == now.end()) {
			removed++;
			if (!stream.WriteLine("* " + std::to_string(i) + " EXPUNGE")) {
				return false;
			}
		}
	}
	size_t after_expunge = s.uids.size() - removed;
	s.uids = now;
	if (now.size() != after_expunge) {
		if (!stream.WriteLine("* " + std::to_string(now.size()) + " EXISTS")) {
			return false;
		}
		// Nothing tracks \Recent — it would need per-session state the store
		// does not keep — so it is reported honestly as 0 here, the same as
		// SELECT does, rather than guessed at.
		if (!stream.WriteLine("* 0 RECENT")) {
			return false;
		}
	}
	return true;
}

// Park until the client says DONE, pushing changes meanwhile. False means the
// connection is finished and the caller should stop reading from it.
bool HandleIdle(Connection &con, Session &s, net::ClientStream &stream, const std::string &tag) {
	stream.WriteLine("+ idling");
	int64_t token = s.selected ? quackmail::citadel::RoomChangeToken(con, s.room) : 0;
	int64_t deadline = (int64_t)std::time(nullptr) + kIdleMaxSeconds;

	for (;;) {
		if (stream.WaitReadable(kIdlePollMs)) {
			std::string line;
			if (!stream.ReadLine(line, 4096)) {
				return false; // the client went away mid-idle
			}
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
				line.pop_back();
			}
			// The reply carries the *IDLE* command's tag whichever way this
			// goes: the client is not listening for any other tag, so a BAD
			// under a different one would never be matched to anything.
			if (util::Upper(line) == "DONE") {
				stream.WriteLine(tag + " OK IDLE terminated");
			} else {
				stream.WriteLine(tag + " BAD expected DONE");
			}
			return true;
		}

		if ((int64_t)std::time(nullptr) >= deadline) {
			// There is no clean way to end IDLE from this side — the client is
			// waiting to send DONE and would match our tagged reply to a
			// command it has not finished. Closing is what other servers do,
			// and BYE says why.
			stream.WriteLine("* BYE Idle timeout, please re-issue IDLE");
			return false;
		}

		// IDLE outside a selected mailbox is legal and has nothing to report.
		if (!s.selected) {
			continue;
		}
		int64_t fresh = quackmail::citadel::RoomChangeToken(con, s.room);
		if (fresh == token) {
			continue;
		}
		token = fresh;
		if (!ReportChanges(con, s, stream)) {
			return false;
		}
	}
}

void HandleImap(DatabaseInstance &db, net::ClientStream &stream, ServerController &ctrl) {
	Connection con(db);
	store::EnsureSchema(con);

	Session s;
	bool tls_active = stream.IsTls();
	std::string pending_user;
	stream.WriteLine("* OK [CAPABILITY " + CapabilityLine(tls_active, ctrl.StartTlsEnabled()) +
	                 "] quackcit IMAP ready");

	std::string line;
	while (stream.ReadLine(line, 65536)) {
		// tag SP command [SP args]
		size_t sp1 = line.find(' ');
		std::string tag = sp1 == std::string::npos ? line : line.substr(0, sp1);
		std::string rest = sp1 == std::string::npos ? "" : line.substr(sp1 + 1);
		size_t sp2 = rest.find(' ');
		std::string cmd = util::Upper(sp2 == std::string::npos ? rest : rest.substr(0, sp2));
		std::string args = sp2 == std::string::npos ? "" : rest.substr(sp2 + 1);

		if (tag.empty()) {
			continue;
		}
		if (cmd == "CAPABILITY") {
			stream.WriteLine("* CAPABILITY " + CapabilityLine(tls_active, ctrl.StartTlsEnabled()));
			stream.WriteLine(tag + " OK CAPABILITY completed");
		} else if (cmd == "STARTTLS") {
			if (tls_active) {
				stream.WriteLine(tag + " NO Already using TLS");
			} else if (!ctrl.StartTlsEnabled()) {
				stream.WriteLine(tag + " NO STARTTLS not available");
			} else {
				stream.WriteLine(tag + " OK Begin TLS negotiation now");
				std::string terr;
				if (!stream.StartTls(ctrl.TlsCtx(), terr)) {
					return;
				}
				tls_active = true;
				// RFC 3501: discard any authentication state established before TLS.
				s = Session();
				pending_user.clear();
			}
		} else if (cmd == "AUTHENTICATE") {
			if (s.authed) {
				stream.WriteLine(tag + " NO Already authenticated");
			} else {
				std::string mech = util::Upper(args);
				std::string initial;
				size_t sp = mech.find(' ');
				if (sp != std::string::npos) {
					initial = args.substr(sp + 1);
					mech = mech.substr(0, sp);
				}
				auto challenge = [&](const std::string &c, std::string &resp) -> bool {
					if (!stream.WriteLine("+ " + c)) {
						return false;
					}
					return stream.ReadLine(resp, 8192);
				};
				std::string auth_user;
				auto r = sasl::ServerAuth(con, mech, initial, challenge, auth_user);
				if (r == sasl::Result::Ok) {
					s.authed = true;
					s.user = auth_user;
					citadel::EnsureUserRooms(con, auth_user);
					stream.WriteLine(tag + " OK [CAPABILITY " +
					                 CapabilityLine(tls_active, ctrl.StartTlsEnabled()) + "] AUTHENTICATE completed");
				} else if (r == sasl::Result::Unsupported) {
					stream.WriteLine(tag + " NO Unsupported authentication mechanism");
				} else {
					stream.WriteLine(tag + " NO Authentication failed");
				}
			}
		} else if (cmd == "NOOP") {
			// A client that cannot IDLE polls with NOOP, and RFC 3501 §6.1.2
			// names this as the whole point of the command: it is the chance to
			// be told what changed. Reporting nothing here is what makes such a
			// client miss new mail until it re-selects.
			if (s.selected) {
				ReportChanges(con, s, stream);
			}
			stream.WriteLine(tag + " OK NOOP completed");
		} else if (cmd == "IDLE") {
			if (!s.authed) {
				stream.WriteLine(tag + " NO not authenticated");
			} else if (!HandleIdle(con, s, stream, tag)) {
				return;
			}
		} else if (cmd == "LOGOUT") {
			stream.WriteLine("* BYE quackcit logging out");
			stream.WriteLine(tag + " OK LOGOUT completed");
			return;
		} else if (cmd == "LOGIN") {
			// LOGIN user pass  (strip optional quotes)
			std::string a = args;
			auto unq = [](std::string v) {
				if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
					return v.substr(1, v.size() - 2);
				}
				return v;
			};
			size_t sp = a.find(' ');
			std::string u = unq(sp == std::string::npos ? a : a.substr(0, sp));
			std::string pw = unq(sp == std::string::npos ? "" : a.substr(sp + 1));
			if (auth::Verify(con, u, pw)) {
				s.authed = true;
				s.user = u;
				citadel::EnsureUserRooms(con, u);
				stream.WriteLine(tag + " OK LOGIN completed");
			} else {
				stream.WriteLine(tag + " NO LOGIN failed");
			}
		} else if (!s.authed) {
			stream.WriteLine(tag + " NO Please LOGIN first");
		} else if (cmd == "NAMESPACE") {
			// Personal namespace "INBOX/", shared namespace "<Floor>/"; delimiter "/".
			stream.WriteLine("* NAMESPACE ((\"INBOX/\" \"/\")) NIL ((\"Main Floor/\" \"/\"))");
			stream.WriteLine(tag + " OK NAMESPACE completed");
		} else if (cmd == "LIST" || cmd == "LSUB") {
			// args = <reference> <pattern>; join them and match against the tree.
			auto unq = [](std::string v) {
				if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
					return v.substr(1, v.size() - 2);
				}
				return v;
			};
			std::string ref, pat;
			size_t sp = args.find(' ');
			if (sp == std::string::npos) {
				pat = unq(args);
			} else {
				ref = unq(args.substr(0, sp));
				pat = unq(args.substr(sp + 1));
			}
			std::string full = ref + pat;
			for (auto &mb : BuildMailboxes(con, s.user)) {
				if (full.empty() || full == "*" || ImapWildMatch(full, mb.name)) {
					stream.WriteLine("* " + cmd + " " + mb.attrs + " \"/\" " + ImapQuote(mb.name));
				}
			}
			stream.WriteLine(tag + " OK " + cmd + " completed");
		} else if (cmd == "STATUS") {
			// args = <mailbox> (<items>)
			std::string name = args;
			size_t paren = args.find('(');
			std::string items;
			if (paren != std::string::npos) {
				name = args.substr(0, paren);
				size_t rp = args.find(')', paren);
				items = util::Upper(args.substr(paren + 1, rp == std::string::npos ? std::string::npos : rp - paren - 1));
			}
			while (!name.empty() && (name.back() == ' ')) {
				name.pop_back();
			}
			if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
				name = name.substr(1, name.size() - 2);
			}
			int64_t room = ResolveMailbox(con, s.user, name);
			if (room < 0) {
				stream.WriteLine(tag + " NO STATUS mailbox not found");
			} else {
				auto uids = RoomUids(con, room);
				int64_t unseen = 0;
				for (int64_t u : uids) {
					auto fl = LoadFlags(con, u, s.user);
					if (std::find(fl.begin(), fl.end(), "\\Seen") == fl.end()) {
						unseen++;
					}
				}
				int64_t uidnext = uids.empty() ? 1 : uids.back() + 1;
				std::string resp;
				auto append = [&](const std::string &k, int64_t v) {
					if (items.find(k) != std::string::npos) {
						resp += (resp.empty() ? "" : " ") + k + " " + std::to_string(v);
					}
				};
				append("MESSAGES", (int64_t)uids.size());
				append("RECENT", 0);
				append("UIDNEXT", uidnext);
				append("UIDVALIDITY", room);
				append("UNSEEN", unseen);
				stream.WriteLine("* STATUS " + ImapQuote(name) + " (" + resp + ")");
				stream.WriteLine(tag + " OK STATUS completed");
			}
		} else if (cmd == "GETQUOTAROOT" || cmd == "GETQUOTA" || cmd == "SETQUOTA") {
			QuotaCommand(con, stream, tag, cmd, args, s.user);
		} else if (cmd == "SELECT" || cmd == "EXAMINE") {
			std::string name = args;
			if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
				name = name.substr(1, name.size() - 2);
			}
			int64_t room = ResolveMailbox(con, s.user, name);
			if (room < 0) {
				stream.WriteLine(tag + " NO mailbox not found");
			} else {
				s.selected = true;
				s.read_only = cmd == "EXAMINE";
				s.room = room;
				s.uids = RoomUids(con, room);
				int64_t uidnext = s.uids.empty() ? 1 : s.uids.back() + 1;
				stream.WriteLine("* " + std::to_string(s.uids.size()) + " EXISTS");
				stream.WriteLine("* 0 RECENT");
				stream.WriteLine("* FLAGS (\\Seen \\Answered \\Flagged \\Deleted \\Draft)");
				stream.WriteLine("* OK [PERMANENTFLAGS (\\Seen \\Answered \\Flagged \\Deleted \\Draft)] Limited");
				// UIDVALIDITY is stable per room (room_num never changes once assigned).
				stream.WriteLine("* OK [UIDVALIDITY " + std::to_string(room) + "] UIDs valid");
				stream.WriteLine("* OK [UIDNEXT " + std::to_string(uidnext) + "] Predicted next UID");
				stream.WriteLine(tag + " OK [" + std::string(s.read_only ? "READ-ONLY" : "READ-WRITE") +
				                 "] " + cmd + " completed");
			}
		} else if (cmd == "FETCH" || (cmd == "UID" && util::Upper(args).rfind("FETCH", 0) == 0)) {
			bool is_uid = cmd == "UID";
			std::string fargs = is_uid ? args.substr(args.find(' ') + 1) : args;
			// fargs = "<set> <items>"
			size_t sp = fargs.find(' ');
			std::string set = sp == std::string::npos ? fargs : fargs.substr(0, sp);
			std::string items = sp == std::string::npos ? "" : fargs.substr(sp + 1);
			std::string items_up = util::Upper(items);
			if (!s.selected) {
				stream.WriteLine(tag + " NO no mailbox selected");
			} else {
				for (size_t pos : ParseSet(set, s.uids, is_uid)) {
					FetchOne(con, s, stream, pos, items_up, is_uid);
				}
				stream.WriteLine(tag + " OK FETCH completed");
			}
		} else if (cmd == "STORE" || (cmd == "UID" && util::Upper(args).rfind("STORE", 0) == 0)) {
			bool is_uid = cmd == "UID";
			std::string sargs = is_uid ? args.substr(args.find(' ') + 1) : args;
			// sargs = "<set> <op>FLAGS (<flags>)"
			size_t sp = sargs.find(' ');
			std::string set = sp == std::string::npos ? sargs : sargs.substr(0, sp);
			std::string ops = sp == std::string::npos ? "" : sargs.substr(sp + 1);
			std::string ops_up = util::Upper(ops);
			bool remove = ops_up.find("-FLAGS") != std::string::npos;
			// Extract flags inside parentheses.
			std::vector<std::string> flags;
			size_t lp = ops.find('(');
			size_t rp = ops.find(')');
			if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
				std::string inner = ops.substr(lp + 1, rp - lp - 1);
				size_t st = 0;
				while (st < inner.size()) {
					size_t nx = inner.find(' ', st);
					std::string f = inner.substr(st, nx == std::string::npos ? std::string::npos : nx - st);
					if (!f.empty()) {
						flags.push_back(f);
					}
					if (nx == std::string::npos) {
						break;
					}
					st = nx + 1;
				}
			}
			if (!s.selected) {
				stream.WriteLine(tag + " NO no mailbox selected");
			} else {
				for (size_t pos : ParseSet(set, s.uids, is_uid)) {
					int64_t uid = s.uids[pos - 1];
					for (auto &f : flags) {
						if (remove) {
							auto stmt = con.Prepare("DELETE FROM citadel_msg_flags WHERE msgnum = $1 AND "
							                        "username = $2 AND flag = $3");
							if (!stmt->HasError()) {
								duckdb::vector<Value> params = {Value::BIGINT(uid), Value(s.user), Value(f)};
								stmt->Execute(params, false);
							}
						} else {
							AddFlag(con, uid, s.user, f);
						}
					}
					if (ops_up.find(".SILENT") == std::string::npos) {
						stream.WriteLine("* " + std::to_string(pos) + " FETCH (FLAGS " +
						                 FlagsList(LoadFlags(con, uid, s.user)) + ")");
					}
				}
				stream.WriteLine(tag + " OK STORE completed");
			}
		} else if (cmd == "SEARCH" || (cmd == "UID" && util::Upper(args).rfind("SEARCH", 0) == 0)) {
			bool is_uid = cmd == "UID";
			std::string crit = is_uid ? args.substr(args.find(' ') + 1) : args;
			if (!s.selected) {
				stream.WriteLine(tag + " NO no mailbox selected");
			} else {
				auto toks = SearchTokens(crit);
				std::string node = citadel::GetConfig(con, "c_nodename", "quackcit");
				std::string hits;
				for (size_t i = 0; i < s.uids.size(); i++) {
					size_t pos = 0;
					if (SearchMatch(con, s.user, i + 1, s.uids[i], s.uids, toks, pos, node)) {
						hits += (hits.empty() ? "" : " ") + std::to_string(is_uid ? s.uids[i] : (int64_t)(i + 1));
					}
				}
				stream.WriteLine("* SEARCH" + (hits.empty() ? "" : " " + hits));
				stream.WriteLine(tag + " OK SEARCH completed");
			}
		} else if (cmd == "COPY" || (cmd == "UID" && util::Upper(args).rfind("COPY", 0) == 0) ||
		           cmd == "MOVE" || (cmd == "UID" && util::Upper(args).rfind("MOVE", 0) == 0)) {
			bool is_uid = cmd == "UID";
			std::string keyword = cmd;   // COPY or MOVE
			std::string body = args;     // "<set> <dest>"
			if (is_uid) {
				size_t k = args.find(' ');
				keyword = util::Upper(args.substr(0, k));
				body = args.substr(k + 1);
			}
			bool is_move = keyword == "MOVE";
			size_t sp = body.find(' ');
			std::string set = sp == std::string::npos ? body : body.substr(0, sp);
			std::string dest = sp == std::string::npos ? "" : body.substr(sp + 1);
			if (dest.size() >= 2 && dest.front() == '"' && dest.back() == '"') {
				dest = dest.substr(1, dest.size() - 2);
			}
			int64_t droom = ResolveMailbox(con, s.user, dest);
			if (!s.selected) {
				stream.WriteLine(tag + " NO no mailbox selected");
			} else if (droom < 0) {
				stream.WriteLine(tag + " NO [TRYCREATE] destination mailbox not found");
			} else {
				// Through core rather than inline SQL. The unlink half of a MOVE
				// has to leave a tombstone or a JMAP/DAV client synchronizing the
				// same room never learns the message left, and re-deriving that
				// here is exactly how this front-end came to skip it.
				for (size_t pos : ParseSet(set, s.uids, is_uid)) {
					std::string move_err;
					quackmail::citadel::MoveMessage(con, s.room, droom, s.uids[pos - 1], !is_move,
					                                move_err);
				}
				if (is_move) {
					s.uids = RoomUids(con, s.room);
				}
				stream.WriteLine(tag + " OK " + (is_move ? "MOVE" : "COPY") + " completed");
			}
		} else if (cmd == "CREATE") {
			std::string name = args;
			if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
				name = name.substr(1, name.size() - 2);
			}
			std::string leaf = name;
			auto slash = name.rfind('/');
			if (slash != std::string::npos) {
				leaf = name.substr(slash + 1);
			}
			// New personal folder for this user (Citadel exposes them under INBOX/).
			int64_t rn = citadel::GetOrCreateUserRoom(con, s.user, leaf);
			stream.WriteLine(rn >= 0 ? tag + " OK CREATE completed" : tag + " NO CREATE failed");
		} else if (cmd == "DELETE") {
			std::string name = args;
			if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
				name = name.substr(1, name.size() - 2);
			}
			int64_t rn = ResolveMailbox(con, s.user, name);
			std::string err;
			if (rn < 0) {
				stream.WriteLine(tag + " NO mailbox not found");
			} else if (citadel::KillRoom(con, rn, err)) {
				stream.WriteLine(tag + " OK DELETE completed");
			} else {
				stream.WriteLine(tag + " NO " + err);
			}
		} else if (cmd == "MYRIGHTS" || cmd == "GETACL" || cmd == "LISTRIGHTS" || cmd == "SETACL" ||
		           cmd == "DELETEACL") {
			// RFC 4314. Rights are mostly derived from the room itself; the ACL
			// table holds the grants Citadel's flags cannot express — above all
			// "anyone p", which is what opens a room to e-mail.
			auto a = ImapArgs(args);
			citadel::Room room;
			int64_t rn = a.empty() ? -1 : ResolveMailbox(con, s.user, a[0]);
			if (a.empty()) {
				stream.WriteLine(tag + " BAD " + cmd + " needs a mailbox name");
			} else if (rn < 0 || !citadel::GetRoomByNum(con, rn, room)) {
				stream.WriteLine(tag + " NO [NONEXISTENT] mailbox not found");
			} else if (cmd == "MYRIGHTS") {
				stream.WriteLine("* MYRIGHTS " + ImapQuote(a[0]) + " " +
				                 citadel::EffectiveRights(con, s.user, room));
				stream.WriteLine(tag + " OK MYRIGHTS completed");
			} else if (citadel::EffectiveRights(con, s.user, room).find('a') == std::string::npos) {
				// Everything below reads or rewrites the ACL, which RFC 4314
				// reserves for holders of the "administer" right.
				stream.WriteLine(tag + " NO [NOPERM] you do not administer that mailbox");
			} else if (cmd == "GETACL") {
				std::string line = "* ACL " + ImapQuote(a[0]);
				for (auto &e : citadel::ListRights(con, room)) {
					line += " " + ImapQuote(e.first) + " " + e.second;
				}
				stream.WriteLine(line);
				stream.WriteLine(tag + " OK GETACL completed");
			} else if (cmd == "LISTRIGHTS") {
				if (a.size() < 2) {
					stream.WriteLine(tag + " BAD LISTRIGHTS needs a mailbox and an identifier");
				} else {
					// Nothing is granted unconditionally; every right is
					// individually grantable.
					std::string line = "* LISTRIGHTS " + ImapQuote(a[0]) + " " + ImapQuote(a[1]) + " \"\"";
					for (const char *p = citadel::kAclRights; *p; p++) {
						line += std::string(" ") + *p;
					}
					stream.WriteLine(line);
					stream.WriteLine(tag + " OK LISTRIGHTS completed");
				}
			} else if (cmd == "DELETEACL") {
				std::string err;
				if (a.size() < 2) {
					stream.WriteLine(tag + " BAD DELETEACL needs a mailbox and an identifier");
				} else if (citadel::SetRights(con, room, a[1], "", err)) {
					stream.WriteLine(tag + " OK DELETEACL completed");
				} else {
					stream.WriteLine(tag + " NO " + err);
				}
			} else { // SETACL
				if (a.size() < 3) {
					stream.WriteLine(tag + " BAD SETACL needs a mailbox, an identifier and rights");
				} else {
					// A leading '+' or '-' edits the existing grant rather than
					// replacing it.
					std::string mod = a[2];
					std::string wanted = mod;
					if (!mod.empty() && (mod[0] == '+' || mod[0] == '-')) {
						std::string cur;
						for (auto &e : citadel::ListRights(con, room)) {
							if (util::Upper(e.first) == util::Upper(a[1])) {
								cur = e.second;
							}
						}
						wanted.clear();
						for (const char *p = citadel::kAclRights; *p; p++) {
							bool have = cur.find(*p) != std::string::npos;
							bool named = mod.find(*p, 1) != std::string::npos;
							if (mod[0] == '+' ? (have || named) : (have && !named)) {
								wanted += *p;
							}
						}
					}
					std::string err;
					if (citadel::SetRights(con, room, a[1], wanted, err)) {
						stream.WriteLine(tag + " OK SETACL completed");
					} else {
						stream.WriteLine(tag + " NO " + err);
					}
				}
			}
		} else if (cmd == "RENAME") {
			auto unq = [](std::string v) {
				if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
					return v.substr(1, v.size() - 2);
				}
				return v;
			};
			size_t sp = args.find(' ');
			std::string from = unq(sp == std::string::npos ? args : args.substr(0, sp));
			std::string to = unq(sp == std::string::npos ? "" : args.substr(sp + 1));
			int64_t rn = ResolveMailbox(con, s.user, from);
			std::string leaf = to;
			auto slash = to.rfind('/');
			if (slash != std::string::npos) {
				leaf = to.substr(slash + 1);
			}
			if (rn < 0) {
				stream.WriteLine(tag + " NO mailbox not found");
			} else {
				auto st = con.Prepare("UPDATE citadel_rooms SET display_name = $1 WHERE room_num = $2");
				if (!st->HasError()) {
					duckdb::vector<Value> pr = {Value(leaf), Value::BIGINT(rn)};
					st->Execute(pr, false);
				}
				stream.WriteLine(tag + " OK RENAME completed");
			}
		} else if (cmd == "SUBSCRIBE" || cmd == "UNSUBSCRIBE") {
			// All rooms are effectively subscribed (LSUB mirrors LIST); accept.
			stream.WriteLine(tag + " OK " + cmd + " completed");
		} else if (cmd == "APPEND") {
			// APPEND <mailbox> [(<flags>)] [<date>] {<size>}
			std::string a = args;
			auto unq = [](std::string v) {
				if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
					return v.substr(1, v.size() - 2);
				}
				return v;
			};
			size_t sp = a.find(' ');
			std::string mbox = unq(sp == std::string::npos ? a : a.substr(0, sp));
			std::string tail = sp == std::string::npos ? "" : a.substr(sp + 1);
			// Parse optional (flags).
			std::vector<std::string> flags;
			size_t lp = tail.find('(');
			if (lp != std::string::npos && (lp == 0 || tail[lp - 1] == ' ')) {
				size_t rp = tail.find(')', lp);
				if (rp != std::string::npos) {
					std::string inner = tail.substr(lp + 1, rp - lp - 1);
					for (auto &f : SearchTokens(inner)) {
						flags.push_back(f);
					}
				}
			}
			// Literal size {n} at the end of the line.
			size_t brace = tail.rfind('{');
			int64_t n = 0;
			if (brace != std::string::npos) {
				n = std::atoll(tail.c_str() + brace + 1);
			}
			int64_t droom = ResolveMailbox(con, s.user, mbox);
			if (droom < 0) {
				stream.WriteLine(tag + " NO [TRYCREATE] mailbox not found");
			} else if (quackmail::quota::WouldExceed(con, s.user, n)) {
				// Refused before the continuation, so the literal never leaves the
				// client. RFC 3501 §4.3 lets a server reject a command carrying a
				// synchronizing literal outright; the alternative is reading up to
				// the whole message in order to throw it away. RFC 9208 §5.1
				// defines OVERQUOTA for exactly this.
				stream.WriteLine(tag + " NO [OVERQUOTA] Mailbox is over its storage quota");
			} else {
				stream.WriteLine("+ Ready for literal data");
				// Read exactly n bytes worth of message, reconstructing CRLFs.
				std::string raw;
				std::string ln;
				while ((int64_t)raw.size() < n && stream.ReadLine(ln, 65536)) {
					raw += ln;
					raw += "\r\n";
				}
				if ((int64_t)raw.size() > n) {
					raw.resize(n);
				}
				auto pm = mime::Parse(raw);
				citadel::Message m;
				m.author = s.user;
				m.author_usernum = citadel::GetOrAssignUserNum(con, s.user);
				m.msgtime = (int64_t)std::time(nullptr);
				m.format_type = 4; // RFC822/MIME
				m.subject = pm.subject;
				m.origin_room = mbox;
				m.raw = raw;
				std::string err;
				int64_t msgnum = citadel::InsertMessage(con, m, {droom}, err);
				if (msgnum < 0) {
					stream.WriteLine(tag + " NO APPEND failed: " + err);
				} else {
					for (auto &f : flags) {
						AddFlag(con, msgnum, s.user, f);
					}
					if (s.selected && s.room == droom) {
						s.uids = RoomUids(con, s.room);
					}
					stream.WriteLine(tag + " OK [APPENDUID " + std::to_string(droom) + " " +
					                 std::to_string(msgnum) + "] APPEND completed");
				}
			}
		} else if (cmd == "EXPUNGE") {
			if (!s.selected) {
				stream.WriteLine(tag + " NO no mailbox selected");
			} else {
				// Remove \Deleted messages' pointers from this room, high-to-low.
				for (size_t i = s.uids.size(); i-- > 0;) {
					auto flags = LoadFlags(con, s.uids[i], s.user);
					bool del = false;
					for (auto &f : flags) {
						if (f == "\\Deleted") {
							del = true;
						}
					}
					if (del) {
						// citadel::DeleteMessage, not a DELETE of our own: it is
						// the one unlink that records a tombstone, and without one
						// an expunge here is invisible to JMAP's Email/changes, to
						// DAV's sync-collection, and to another session's IDLE.
						std::string del_err;
						quackmail::citadel::DeleteMessage(con, s.room, s.uids[i], del_err);
						stream.WriteLine("* " + std::to_string(i + 1) + " EXPUNGE");
					}
				}
				s.uids = RoomUids(con, s.room);
				stream.WriteLine(tag + " OK EXPUNGE completed");
			}
		} else if (cmd == "CLOSE") {
			s.selected = false;
			stream.WriteLine(tag + " OK CLOSE completed");
		} else {
			stream.WriteLine(tag + " BAD command not supported");
		}
	}
}

// Thin ConnHandler that carries the global controller into the real handler
// (needed for STARTTLS: ctrl.StartTlsEnabled()/TlsCtx()).
void HandleImapConn(DatabaseInstance &db, net::ClientStream &stream) {
	HandleImap(db, stream, g_imap);
}
void HandleImapsConn(DatabaseInstance &db, net::ClientStream &stream) {
	HandleImap(db, stream, g_imaps);
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "qm_imap", 1143, g_imap, HandleImapConn);
	RegisterServerControls(loader, "qm_imaps", 1993, g_imaps, HandleImapsConn);
}

} // namespace

void QuackmailImapExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailImapExtension::Name() {
	return "quackmail_imap";
}
std::string QuackmailImapExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_IMAP
	return EXT_VERSION_QUACKMAIL_IMAP;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_imap, loader) {
	duckdb::LoadInternal(loader);
}
}
