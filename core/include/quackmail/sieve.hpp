#pragma once

#include "duckdb.hpp"
#include "quackmail/mime.hpp"

#include <string>
#include <vector>

namespace quackmail {
namespace sieve {

// RFC 5228 mail filtering, plus the `reject` (RFC 5429), `envelope` and `copy`
// (RFC 3894) extensions. A script produces a *set* of actions, not one: a
// message can be filed into several folders, or redirected and kept.

struct Action {
	enum Type {
		KEEP,     // deliver to the default folder (INBOX / the Mail room)
		FILEINTO, // deliver to a named folder
		REDIRECT, // forward to another address
		REJECT,   // refuse the message with a reason
		DISCARD,  // silently drop
	};
	Type type = KEEP;
	std::string folder;  // FILEINTO
	std::string address; // REDIRECT
	std::string reason;  // REJECT
	bool create = false; // fileinto :create

	Action() = default;
	explicit Action(Type t) : type(t) {}
};

// The envelope values the `envelope` test inspects. These come from the SMTP
// transaction, not from the message headers, which is the whole point of it.
struct Envelope {
	std::string mail_from;
	std::string rcpt_to;

	Envelope() = default;
	Envelope(std::string from, std::string to) : mail_from(std::move(from)), rcpt_to(std::move(to)) {}
};

struct Result {
	std::vector<Action> actions; // in the order the script produced them
	std::string error;           // set when the script could not be parsed

	// True when the message should be dropped with no delivery anywhere.
	bool IsDiscard() const {
		return actions.size() == 1 && actions[0].type == Action::DISCARD;
	}
	// The reject reason, or "" when the script did not reject.
	std::string RejectReason() const {
		for (auto &a : actions) {
			if (a.type == Action::REJECT) {
				return a.reason.empty() ? "message refused by the recipient's filter" : a.reason;
			}
		}
		return "";
	}
};

// Evaluate `script` against a message. `raw` is the full RFC822 text (the
// `size` and `body` tests need it); `env` supplies the SMTP envelope.
//
// RFC 5228 §2.10.2 implicit keep applies: a script that takes no delivering
// action still keeps the message. A script that fails to parse also keeps it —
// mail is never lost to a bad filter.
Result Evaluate(const std::string &script, const mime::ParsedMessage &msg, const std::string &raw,
                const Envelope &env);

// Parse-check a script without running it, for ManageSieve PUTSCRIPT and
// CHECKSCRIPT. Returns false with a human-readable `err` naming the line.
bool Check(const std::string &script, std::string &err);

// Load the active Sieve script for a user (empty string if none).
std::string LoadActiveScript(duckdb::Connection &con, const std::string &username);

// The extensions this engine implements, space-separated, for ManageSieve's
// "SIEVE" capability line. Only what actually works is advertised.
std::string Capabilities();

} // namespace sieve
} // namespace quackmail
