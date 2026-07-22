#define DUCKDB_EXTENSION_MAIN

#include "quackmail_smtp_in_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/citadel_store.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/sieve.hpp"
#include "quackmail/util.hpp"

#include <ctime>
#include <string>
#include <vector>

namespace duckdb {
namespace {

using namespace quackmail;

// One controller instance for this extension (internal linkage, so it does not
// collide with other extensions' controllers in the static test binary).
ServerController g_smtp_in;

constexpr size_t kMaxMessageBytes = 25 * 1024 * 1024;

// Parse "FROM:<addr>" / "TO:<addr>" out of a MAIL/RCPT argument.
std::string ExtractPath(const std::string &arg) {
	auto lt = arg.find('<');
	auto gt = arg.find('>');
	if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
		return arg.substr(lt + 1, gt - lt - 1);
	}
	// Fall back to the token after the ':'.
	auto colon = arg.find(':');
	std::string rest = colon == std::string::npos ? arg : arg.substr(colon + 1);
	// Trim spaces.
	size_t b = rest.find_first_not_of(" \t");
	size_t e = rest.find_last_not_of(" \t");
	return b == std::string::npos ? "" : rest.substr(b, e - b + 1);
}

// Split a command line into upper-cased verb + remainder.
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

// Handle "AUTH ..." after TLS. Returns true if authentication succeeded.
bool HandleAuth(Connection &con, net::ClientStream &stream, const std::string &rest, std::string &user_out) {
	std::string mechanism, initial;
	SplitCommand(rest, mechanism, initial);

	if (mechanism == "PLAIN") {
		std::string b64 = initial;
		if (b64.empty()) {
			stream.WriteLine("334 ");
			if (!stream.ReadLine(b64, 4096)) {
				return false;
			}
		}
		std::string decoded;
		if (!util::Base64Decode(b64, decoded)) {
			stream.WriteLine("535 Authentication failed");
			return false;
		}
		// authzid \0 authcid \0 passwd
		size_t p1 = decoded.find('\0');
		if (p1 == std::string::npos) {
			stream.WriteLine("535 Authentication failed");
			return false;
		}
		size_t p2 = decoded.find('\0', p1 + 1);
		if (p2 == std::string::npos) {
			stream.WriteLine("535 Authentication failed");
			return false;
		}
		std::string username = decoded.substr(p1 + 1, p2 - p1 - 1);
		std::string password = decoded.substr(p2 + 1);
		if (auth::Verify(con, username, password)) {
			user_out = username;
			stream.WriteLine("235 Authentication successful");
			return true;
		}
		stream.WriteLine("535 Authentication failed");
		return false;
	}

	if (mechanism == "LOGIN") {
		std::string b64, username, password;
		stream.WriteLine("334 " + util::Base64Encode("Username:"));
		if (!stream.ReadLine(b64, 4096) || !util::Base64Decode(b64, username)) {
			stream.WriteLine("535 Authentication failed");
			return false;
		}
		stream.WriteLine("334 " + util::Base64Encode("Password:"));
		if (!stream.ReadLine(b64, 4096) || !util::Base64Decode(b64, password)) {
			stream.WriteLine("535 Authentication failed");
			return false;
		}
		if (auth::Verify(con, username, password)) {
			user_out = username;
			stream.WriteLine("235 Authentication successful");
			return true;
		}
		stream.WriteLine("535 Authentication failed");
		return false;
	}

	stream.WriteLine("504 Unrecognized authentication mechanism");
	return false;
}

// Find a header value by case-insensitive name (empty if absent).
std::string HeaderValue(const mime::ParsedMessage &parsed, const std::string &name) {
	std::string want = util::Upper(name);
	for (auto &h : parsed.headers) {
		if (util::Upper(h.first) == want) {
			return h.second;
		}
	}
	return "";
}

// Parse, filter (Sieve), and deliver an accepted message into each recipient's
// Citadel Mail room (or a Sieve fileinto room). Returns false on error.
bool StoreMessage(Connection &con, const std::string &mail_from, const std::vector<std::string> &rcpts,
                  const std::string &body, std::string &err) {
	auto parsed = mime::Parse(body);

	// Resolve the delivery room for each local recipient (deduplicated).
	std::vector<int64_t> rooms;
	auto add_room = [&](int64_t room) {
		if (room < 0) {
			return;
		}
		for (int64_t r : rooms) {
			if (r == room) {
				return;
			}
		}
		rooms.push_back(room);
	};

	for (const std::string &rcpt : rcpts) {
		std::string user = util::LocalPart(rcpt);
		std::string folder = "Mail";
		std::string script = sieve::LoadActiveScript(con, user);
		if (!script.empty()) {
			auto action = sieve::Evaluate(script, parsed);
			if (action.type == sieve::Action::DISCARD) {
				continue; // silently dropped by the user's filter
			}
			if (action.type == sieve::Action::FILEINTO && !action.folder.empty()) {
				folder = action.folder;
			}
		}
		add_room(citadel::GetOrCreateUserRoom(con, user, folder));
	}

	if (rooms.empty()) {
		return true; // no local recipients / all filtered out; accept and drop
	}

	citadel::Message msg;
	// Author: the From: display name/address, falling back to the envelope sender.
	if (!parsed.from.empty()) {
		auto addrs = mime::ParseAddressList(parsed.from);
		if (!addrs.empty()) {
			msg.author = !addrs[0].name.empty() ? addrs[0].name : addrs[0].addr;
		}
	}
	if (msg.author.empty()) {
		msg.author = mail_from;
	}
	std::string joined;
	for (size_t i = 0; i < rcpts.size(); i++) {
		joined += (i ? ", " : "") + rcpts[i];
	}
	msg.recipient = joined;
	msg.subject = mime::DecodeEncodedWords(parsed.subject);
	msg.euid = parsed.message_id;
	msg.references = HeaderValue(parsed, "References");
	msg.format_type = 4; // RFC822/MIME
	msg.raw = body;
	int64_t epoch = 0;
	msg.msgtime = mime::ParseDate(HeaderValue(parsed, "Date"), epoch) ? epoch : (int64_t)std::time(nullptr);

	return citadel::InsertMessage(con, msg, rooms, err) >= 0;
}

void HandleSmtp(DatabaseInstance &db, net::ClientStream &stream) {
	Connection con(db);
	store::EnsureSchema(con);

	bool tls_active = stream.IsTls();
	bool authed = false;
	std::string auth_user;
	std::string mail_from;
	std::vector<std::string> rcpts;
	bool have_mail = false;

	stream.WriteLine("220 quackmail ESMTP ready");

	std::string line;
	while (stream.ReadLine(line, 8192)) {
		std::string verb, rest;
		SplitCommand(line, verb, rest);

		if (verb == "EHLO") {
			stream.WriteLine("250-quackmail greets " + rest);
			if (!tls_active && g_smtp_in.StartTlsEnabled()) {
				stream.WriteLine("250-STARTTLS");
			}
			if (tls_active) {
				stream.WriteLine("250-AUTH PLAIN LOGIN");
			}
			stream.WriteLine("250 SIZE " + std::to_string(kMaxMessageBytes));
		} else if (verb == "HELO") {
			stream.WriteLine("250 quackmail");
		} else if (verb == "STARTTLS") {
			if (tls_active) {
				stream.WriteLine("503 Already running TLS");
			} else if (!g_smtp_in.StartTlsEnabled()) {
				stream.WriteLine("502 STARTTLS not available");
			} else {
				stream.WriteLine("220 Ready to start TLS");
				std::string terr;
				if (!stream.StartTls(g_smtp_in.TlsCtx(), terr)) {
					return; // handshake failed; drop connection
				}
				tls_active = true;
				authed = false;
				have_mail = false;
				mail_from.clear();
				rcpts.clear();
			}
		} else if (verb == "AUTH") {
			if (!tls_active) {
				stream.WriteLine("538 Encryption required for requested authentication mechanism");
			} else if (authed) {
				stream.WriteLine("503 Already authenticated");
			} else {
				authed = HandleAuth(con, stream, rest, auth_user);
			}
		} else if (verb == "MAIL") {
			mail_from = ExtractPath(rest);
			rcpts.clear();
			have_mail = true;
			stream.WriteLine("250 OK");
		} else if (verb == "RCPT") {
			if (!have_mail) {
				stream.WriteLine("503 Need MAIL before RCPT");
			} else {
				rcpts.push_back(ExtractPath(rest));
				stream.WriteLine("250 OK");
			}
		} else if (verb == "DATA") {
			if (!have_mail || rcpts.empty()) {
				stream.WriteLine("503 Need MAIL and RCPT before DATA");
				continue;
			}
			stream.WriteLine("354 End data with <CR><LF>.<CR><LF>");
			std::string body;
			if (!stream.ReadDotStuffed(body, kMaxMessageBytes)) {
				stream.WriteLine("552 Message too large or read error");
				return;
			}
			std::string err;
			if (!StoreMessage(con, mail_from, rcpts, body, err)) {
				stream.WriteLine("451 Local storage error");
			} else {
				stream.WriteLine("250 OK: message accepted");
			}
			have_mail = false;
			mail_from.clear();
			rcpts.clear();
		} else if (verb == "RSET") {
			have_mail = false;
			mail_from.clear();
			rcpts.clear();
			stream.WriteLine("250 OK");
		} else if (verb == "NOOP") {
			stream.WriteLine("250 OK");
		} else if (verb == "VRFY") {
			stream.WriteLine("252 Cannot VRFY user");
		} else if (verb == "QUIT") {
			stream.WriteLine("221 quackmail closing connection");
			return;
		} else {
			stream.WriteLine("500 Unknown command");
		}
	}
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "qm_smtp_in", 2525, g_smtp_in, HandleSmtp);
}

} // namespace

void QuackmailSmtpInExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailSmtpInExtension::Name() {
	return "quackmail_smtp_in";
}
std::string QuackmailSmtpInExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_SMTP_IN
	return EXT_VERSION_QUACKMAIL_SMTP_IN;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_smtp_in, loader) {
	duckdb::LoadInternal(loader);
}
}
