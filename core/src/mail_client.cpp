#include "quackmail/mail_client.hpp"

#include "quackmail/net.hpp"
#include "quackmail/tls.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace quackmail {
namespace mailclient {

using net::ClientStream;

namespace {

// A connected, possibly TLS-wrapped session. Both protocols need the same
// connect / implicit-TLS / in-band-upgrade dance, so it lives here once.
struct Session {
	std::unique_ptr<ClientStream> stream;
	tls::ClientContext ctx;
	std::string err;

	bool Open(const Account &acct, int default_port) {
		int port = acct.port > 0 ? acct.port : default_port;
		int fd = net::Connect(acct.host, port, acct.timeout_ms, err);
		if (fd < 0) {
			return false;
		}
		stream = std::unique_ptr<ClientStream>(new ClientStream(fd));
		// A remote mailbox that accepts the connection and then goes quiet must
		// not hold the fetch worker's thread; every read is bounded.
		stream->SetTimeouts(acct.timeout_ms, acct.timeout_ms);
		if (acct.tls == TlsMode::Implicit) {
			if (!ctx.Init(err) || !stream->ConnectTls(ctx.Get(), err)) {
				return false;
			}
		}
		return true;
	}

	// In-band upgrade, after the protocol's own go-ahead.
	bool Upgrade() {
		return ctx.Init(err) && stream->ConnectTls(ctx.Get(), err);
	}
};

// Strip the trailing CRLF a line still carries, and unstuff a leading dot.
std::string Unstuff(const std::string &line) {
	if (!line.empty() && line[0] == '.') {
		return line.substr(1);
	}
	return line;
}

// ---- POP3 ---------------------------------------------------------------

bool Pop3Ok(const std::string &line) {
	return line.size() >= 3 && line.compare(0, 3, "+OK") == 0;
}

// Send one command and read its single-line reply.
bool Pop3Cmd(ClientStream &s, const std::string &cmd, std::string &reply) {
	return s.WriteLine(cmd) && s.ReadLine(reply, 8192);
}

// Read a dot-terminated multi-line response into `lines`.
bool Pop3Multi(ClientStream &s, std::vector<std::string> &lines, size_t max_bytes) {
	size_t total = 0;
	std::string line;
	while (s.ReadLine(line, 65536)) {
		if (line == ".") {
			return true;
		}
		total += line.size() + 2;
		if (max_bytes > 0 && total > max_bytes) {
			// Keep draining to the terminator rather than abandoning the
			// connection mid-response: the next command's reply would otherwise
			// read this message's remaining lines.
			while (s.ReadLine(line, 65536) && line != ".") {
			}
			return false;
		}
		lines.push_back(Unstuff(line));
	}
	return false;
}

std::string JoinLines(const std::vector<std::string> &lines) {
	std::string out;
	for (const auto &l : lines) {
		out += l;
		out += "\r\n";
	}
	return out;
}

} // namespace

Result FetchPop3(const Account &acct, int max_messages, size_t max_bytes, bool del, const WantFn &want,
                 const std::function<bool(const Fetched &)> &on_message) {
	Result res;
	Session sess;
	if (!sess.Open(acct, acct.tls == TlsMode::Implicit ? 995 : 110)) {
		res.error = sess.err;
		return res;
	}
	ClientStream &s = *sess.stream;

	std::string line;
	if (!s.ReadLine(line, 8192) || !Pop3Ok(line)) {
		res.error = "bad greeting: " + line;
		return res;
	}

	if (acct.tls == TlsMode::StartTls) {
		if (!Pop3Cmd(s, "STLS", line) || !Pop3Ok(line)) {
			res.error = "STLS refused: " + line;
			return res;
		}
		if (!sess.Upgrade()) {
			res.error = "TLS handshake failed: " + sess.err;
			return res;
		}
	}

	if (!Pop3Cmd(s, "USER " + acct.username, line) || !Pop3Ok(line)) {
		res.error = "USER refused: " + line;
		return res;
	}
	if (!Pop3Cmd(s, "PASS " + acct.password, line) || !Pop3Ok(line)) {
		// Never echo the reply here: some servers quote the command back.
		res.error = "authentication failed";
		return res;
	}

	// UIDL gives a stable per-message identifier, which is the only thing that
	// makes a leave-on-server poll idempotent. Without it every poll would
	// re-download the whole mailbox, so a server that refuses it is an error
	// rather than something to work around.
	std::vector<std::string> uidl_lines;
	if (!Pop3Cmd(s, "UIDL", line) || !Pop3Ok(line) || !Pop3Multi(s, uidl_lines, 0)) {
		res.error = "UIDL not supported by this server";
		Pop3Cmd(s, "QUIT", line);
		return res;
	}

	std::vector<std::pair<int, std::string>> messages; // (msg number, uid)
	for (const auto &l : uidl_lines) {
		auto sp = l.find(' ');
		if (sp == std::string::npos) {
			continue;
		}
		int num = std::atoi(l.substr(0, sp).c_str());
		std::string uid = l.substr(sp + 1);
		if (num > 0 && !uid.empty()) {
			messages.emplace_back(num, uid);
		}
	}
	res.seen = (int64_t)messages.size();

	std::vector<int> to_delete;
	for (const auto &m : messages) {
		if (max_messages > 0 && res.fetched >= max_messages) {
			res.info = "stopped at the per-run limit";
			break;
		}
		if (want && !want(m.second)) {
			continue;
		}
		std::vector<std::string> body;
		if (!Pop3Cmd(s, "RETR " + std::to_string(m.first), line) || !Pop3Ok(line)) {
			continue; // one unreadable message must not abandon the rest
		}
		if (!Pop3Multi(s, body, max_bytes)) {
			continue; // oversized or truncated; left on the server untouched
		}
		Fetched f;
		f.uid = m.second;
		f.raw = JoinLines(body);
		if (!on_message(f)) {
			continue; // storage refused it: do not delete it upstream either
		}
		res.fetched++;
		if (del) {
			to_delete.push_back(m.first);
		}
	}

	// Deletions are issued last and only take effect at QUIT, so anything that
	// goes wrong above leaves the remote mailbox exactly as it was.
	for (int num : to_delete) {
		if (Pop3Cmd(s, "DELE " + std::to_string(num), line) && Pop3Ok(line)) {
			res.deleted++;
		}
	}
	Pop3Cmd(s, "QUIT", line);

	res.ok = true;
	return res;
}

Result TestPop3(const Account &acct) {
	Result res = FetchPop3(acct, 0, 0, false, [](const std::string &) { return false; },
	                       [](const Fetched &) { return true; });
	if (res.ok) {
		res.info = std::to_string(res.seen) + " message(s) in the mailbox";
	}
	return res;
}

// ---- IMAP ---------------------------------------------------------------

namespace {

struct ImapClient {
	ClientStream &s;
	int tag = 0;

	explicit ImapClient(ClientStream &stream) : s(stream) {
	}

	std::string NextTag() {
		char buf[16];
		std::snprintf(buf, sizeof buf, "a%03d", ++tag);
		return buf;
	}

	// Run one command, collecting untagged responses until the tagged one.
	// Returns true on OK. Literals ({n}) in the untagged data are read as
	// exactly n bytes and appended to the line they appeared on, which is what
	// makes a FETCH body come back in one piece.
	bool Command(const std::string &cmd, std::vector<std::string> &untagged, std::string &reply) {
		std::string t = NextTag();
		if (!s.WriteLine(t + " " + cmd)) {
			reply = "write failed";
			return false;
		}
		std::string line;
		while (s.ReadLine(line, 65536)) {
			// A literal is announced by "{n}" at the very end of the line.
			while (!line.empty() && line.back() == '}') {
				auto brace = line.rfind('{');
				if (brace == std::string::npos) {
					break;
				}
				std::string count = line.substr(brace + 1, line.size() - brace - 2);
				if (count.empty() || !std::all_of(count.begin(), count.end(),
				                                  [](char c) { return c >= '0' && c <= '9'; })) {
					break;
				}
				size_t n = (size_t)std::strtoull(count.c_str(), nullptr, 10);
				std::string blob;
				if (!s.ReadN(blob, n)) {
					reply = "short literal";
					return false;
				}
				line = line.substr(0, brace) + blob;
				// The rest of the response line follows the literal.
				std::string rest;
				if (!s.ReadLine(rest, 65536)) {
					break;
				}
				line += rest;
			}
			if (line.compare(0, t.size(), t) == 0 && line.size() > t.size() && line[t.size()] == ' ') {
				reply = line.substr(t.size() + 1);
				return reply.compare(0, 2, "OK") == 0;
			}
			untagged.push_back(line);
		}
		reply = "connection closed";
		return false;
	}

	// An IMAP string argument: quoted, with the two characters that need it
	// escaped. Mailbox names come from configuration, but configuration is
	// still input.
	static std::string Quote(const std::string &in) {
		std::string out = "\"";
		for (char c : in) {
			if (c == '"' || c == '\\') {
				out += '\\';
			}
			out += c;
		}
		return out + "\"";
	}
};

// Pull the first "NAME <number>" out of an untagged OK response code.
int64_t ResponseCodeNum(const std::vector<std::string> &lines, const std::string &name) {
	for (const auto &l : lines) {
		auto at = l.find("[" + name + " ");
		if (at == std::string::npos) {
			continue;
		}
		return std::strtoll(l.c_str() + at + name.size() + 2, nullptr, 10);
	}
	return 0;
}

} // namespace

Result FetchImap(const Account &acct, int64_t uidvalidity, int64_t since_uid, int max_messages,
                 size_t max_bytes, bool del, const WantFn &want,
                 const std::function<bool(const Fetched &)> &on_message) {
	Result res;
	Session sess;
	if (!sess.Open(acct, acct.tls == TlsMode::Implicit ? 993 : 143)) {
		res.error = sess.err;
		return res;
	}
	ClientStream &s = *sess.stream;
	ImapClient c(s);

	std::string line, reply;
	if (!s.ReadLine(line, 8192) || line.find("OK") == std::string::npos) {
		res.error = "bad greeting: " + line;
		return res;
	}

	std::vector<std::string> untagged;
	if (acct.tls == TlsMode::StartTls) {
		if (!c.Command("STARTTLS", untagged, reply)) {
			res.error = "STARTTLS refused: " + reply;
			return res;
		}
		if (!sess.Upgrade()) {
			res.error = "TLS handshake failed: " + sess.err;
			return res;
		}
	}

	untagged.clear();
	if (!c.Command("LOGIN " + ImapClient::Quote(acct.username) + " " + ImapClient::Quote(acct.password),
	               untagged, reply)) {
		res.error = "authentication failed";
		return res;
	}

	untagged.clear();
	std::string mailbox = acct.mailbox.empty() ? "INBOX" : acct.mailbox;
	if (!c.Command("SELECT " + ImapClient::Quote(mailbox), untagged, reply)) {
		res.error = "cannot select '" + mailbox + "': " + reply;
		c.Command("LOGOUT", untagged, reply);
		return res;
	}
	res.uidvalidity = ResponseCodeNum(untagged, "UIDVALIDITY");

	// UIDVALIDITY changing means the server has renumbered the mailbox and every
	// uid we stored now means something else. Starting over is the only correct
	// response — RFC 3501 is explicit that old uids must not be reused.
	int64_t from_uid = since_uid;
	if (uidvalidity != 0 && res.uidvalidity != 0 && res.uidvalidity != uidvalidity) {
		from_uid = 0;
		res.info = "UIDVALIDITY changed; starting from the beginning of the mailbox";
	}

	untagged.clear();
	std::string range = std::to_string(from_uid + 1) + ":*";
	if (!c.Command("UID SEARCH UID " + range, untagged, reply)) {
		res.error = "UID SEARCH failed: " + reply;
		c.Command("LOGOUT", untagged, reply);
		return res;
	}
	std::vector<int64_t> uids;
	for (const auto &l : untagged) {
		if (l.compare(0, 9, "* SEARCH ") != 0) {
			continue;
		}
		const char *p = l.c_str() + 9;
		while (*p) {
			char *end = nullptr;
			long long v = std::strtoll(p, &end, 10);
			if (end == p) {
				break;
			}
			// "UID n:*" matches the highest uid even when it is below n, so a
			// mailbox with nothing new still returns one result. Filtering here
			// rather than trusting the range is what stops the newest message
			// being re-fetched on every poll.
			if (v > from_uid) {
				uids.push_back((int64_t)v);
			}
			p = end;
			while (*p == ' ') {
				p++;
			}
		}
	}
	std::sort(uids.begin(), uids.end());
	res.seen = (int64_t)uids.size();
	res.highest_uid = from_uid;

	std::vector<int64_t> to_delete;
	for (int64_t uid : uids) {
		if (max_messages > 0 && res.fetched >= max_messages) {
			res.info = "stopped at the per-run limit";
			break;
		}
		std::string key = std::to_string(res.uidvalidity) + "." + std::to_string(uid);
		if (want && !want(key)) {
			res.highest_uid = std::max(res.highest_uid, uid);
			continue;
		}
		untagged.clear();
		// BODY.PEEK, not BODY: fetching must not mark the message read in
		// somebody else's mailbox.
		if (!c.Command("UID FETCH " + std::to_string(uid) + " (BODY.PEEK[])", untagged, reply)) {
			continue;
		}
		std::string raw;
		for (const auto &l : untagged) {
			auto at = l.find("BODY[]");
			if (at == std::string::npos) {
				continue;
			}
			// Command() has already spliced the literal in place of "{n}", so
			// what follows the marker is the message.
			auto start = l.find(' ', at);
			if (start != std::string::npos) {
				raw = l.substr(start + 1);
			}
		}
		if (raw.empty() || (max_bytes > 0 && raw.size() > max_bytes)) {
			res.highest_uid = std::max(res.highest_uid, uid);
			continue;
		}
		Fetched f;
		f.uid = key;
		f.raw = raw;
		if (!on_message(f)) {
			continue; // storage refused it; leave the watermark where it is
		}
		res.fetched++;
		res.highest_uid = std::max(res.highest_uid, uid);
		if (del) {
			to_delete.push_back(uid);
		}
	}

	for (int64_t uid : to_delete) {
		untagged.clear();
		if (c.Command("UID STORE " + std::to_string(uid) + " +FLAGS (\\Deleted)", untagged, reply)) {
			res.deleted++;
		}
	}
	if (res.deleted > 0) {
		untagged.clear();
		c.Command("EXPUNGE", untagged, reply);
	}
	untagged.clear();
	c.Command("LOGOUT", untagged, reply);

	res.ok = true;
	return res;
}

Result TestImap(const Account &acct) {
	Result res = FetchImap(acct, 0, 0, 0, 0, false, [](const std::string &) { return false; },
	                       [](const Fetched &) { return true; });
	if (res.ok) {
		res.info = std::to_string(res.seen) + " message(s) in " +
		           (acct.mailbox.empty() ? "INBOX" : acct.mailbox) +
		           ", UIDVALIDITY " + std::to_string(res.uidvalidity);
	}
	return res;
}

} // namespace mailclient
} // namespace quackmail
