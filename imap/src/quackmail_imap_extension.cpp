#define DUCKDB_EXTENSION_MAIN

#include "quackmail_imap_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/citadel_store.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/sasl.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace {

using namespace quackmail;

ServerController g_imap;

// IMAP is deliberately a minimal but real subset: LOGIN, CAPABILITY, LIST,
// SELECT/EXAMINE, FETCH (FLAGS/UID/RFC822.SIZE/INTERNALDATE/ENVELOPE and
// BODY[]/[HEADER]/[TEXT]), STORE flags, EXPUNGE, CLOSE, LOGOUT. BODYSTRUCTURE,
// IDLE, CONDSTORE and friends are a later iteration.
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

std::string BuildEnvelope(const citadel::Message &msg) {
	auto p = mime::Parse(msg.raw);
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
		add("RFC822.SIZE " + std::to_string(msg.raw.size()));
	}
	if (want_date) {
		add("INTERNALDATE " + ImapQuote(InternalDate(msg.msgtime)));
	}
	if (want_env) {
		add("ENVELOPE " + BuildEnvelope(msg));
	}

	// A body section is returned as a literal; assemble the leading part first,
	// then stream "{n}\r\n<bytes>".
	std::string section, payload;
	if (body_header) {
		std::string h, b;
		SplitHeaderBody(msg.raw, h, b);
		section = sect.find("RFC822.HEADER") != std::string::npos ? "RFC822.HEADER" : "BODY[HEADER]";
		payload = h;
	} else if (body_text) {
		std::string h, b;
		SplitHeaderBody(msg.raw, h, b);
		section = sect.find("RFC822.TEXT") != std::string::npos ? "RFC822.TEXT" : "BODY[TEXT]";
		payload = b;
	} else if (body_full) {
		section = sect.find("BODY[]") != std::string::npos ? "BODY[]" : "RFC822";
		payload = msg.raw;
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

// The CAPABILITY token list. STARTTLS is advertised only before the TLS upgrade;
// mirrors a real Citadel server (which offers NAMESPACE, UIDPLUS, SASL, ID).
std::string CapabilityLine(bool tls_active, bool starttls_avail) {
	std::string caps = "IMAP4rev1 NAMESPACE ID UIDPLUS AUTH=PLAIN AUTH=LOGIN";
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
			stream.WriteLine(tag + " OK NOOP completed");
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
						auto stmt = con.Prepare("DELETE FROM citadel_room_msgs WHERE room_num = $1 AND msgnum = $2");
						if (!stmt->HasError()) {
							duckdb::vector<Value> params = {Value::BIGINT(s.room), Value::BIGINT(s.uids[i])};
							stmt->Execute(params, false);
						}
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

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "qm_imap", 1143, g_imap, HandleImapConn);
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
