#define DUCKDB_EXTENSION_MAIN

#include "quackmail_smtp_in_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/delivery.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/util.hpp"

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

// Inbound MX: accept mail only for local recipients and deliver it into their
// Citadel Mail rooms. This is the public-facing MTA, so it offers no AUTH and
// never relays; authenticated submission for outbound mail lives in smtp_out.
void HandleSmtp(DatabaseInstance &db, net::ClientStream &stream) {
	Connection con(db);
	store::EnsureSchema(con);

	bool tls_active = stream.IsTls();
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
				have_mail = false;
				mail_from.clear();
				rcpts.clear();
			}
		} else if (verb == "AUTH") {
			// The MX does not authenticate senders (use the submission ports).
			stream.WriteLine("503 5.7.0 AUTH not available; use the submission service");
		} else if (verb == "MAIL") {
			mail_from = ExtractPath(rest);
			rcpts.clear();
			have_mail = true;
			stream.WriteLine("250 OK");
		} else if (verb == "RCPT") {
			if (!have_mail) {
				stream.WriteLine("503 Need MAIL before RCPT");
			} else {
				std::string rcpt = ExtractPath(rest);
				if (citadel::IsLocalUser(con, rcpt)) {
					rcpts.push_back(rcpt);
					stream.WriteLine("250 OK");
				} else {
					// Distinguish an unknown local user from a relay attempt so the
					// client gets an accurate reason.
					auto at = rcpt.find('@');
					std::string fqdn = citadel::GetConfig(con, "c_fqdn", "");
					bool foreign = at != std::string::npos && !fqdn.empty() &&
					               util::Upper(rcpt.substr(at + 1)) != util::Upper(fqdn);
					stream.WriteLine(foreign ? "550 5.7.1 Relaying denied"
					                         : "550 5.1.1 No such user here");
				}
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
			if (!deliver::LocalDeliver(con, mail_from, rcpts, body, err)) {
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
