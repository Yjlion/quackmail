#pragma once

#include "duckdb.hpp"
#include "quackmail/mime.hpp"

#include <string>
#include <vector>

namespace quackmail {
namespace sieve {

// RFC 5228 mail filtering, plus the `reject` (RFC 5429), `envelope` and `copy`
// (RFC 3894), `mailbox` (RFC 5490 — only `fileinto :create`), `imap4flags`
// (RFC 5232), `variables` (RFC 5229) and `vacation` (RFC 5230) extensions. A
// script produces a *set* of actions, not one: a message can be filed into
// several folders, or redirected and kept.

// vacation's tagged arguments (RFC 5230). Only meaningful on a VACATION action.
//
// The evaluator decides *whether an auto-reply is warranted by the message* and
// nothing else — every rule in §4.5/§4.6 that is a pure function of the message
// is applied before the action is emitted, so those rules are testable without
// a database. Whether one was already sent to this correspondent recently is
// the caller's problem, because only the caller has a connection.
struct VacationSpec {
	std::string subject;                // :subject; empty means "Auto: <original>"
	std::string from;                   // :from; empty means the recipient's own address
	std::string handle;                 // :handle; empty means "derive it from the text"
	std::vector<std::string> addresses; // :addresses — further addresses that are "me"
	int days = 7;                       // :days — the per-correspondent silence window
	bool mime = false;                  // the reason is a MIME entity rather than plain text

	VacationSpec() = default;
};

struct Action {
	enum Type {
		KEEP,     // deliver to the default folder (INBOX / the Mail room)
		FILEINTO, // deliver to a named folder
		REDIRECT, // forward to another address
		REJECT,   // refuse the message with a reason
		DISCARD,  // silently drop
		VACATION, // auto-reply to the sender
	};
	Type type = KEEP;
	std::string folder;  // FILEINTO
	std::string address; // REDIRECT
	std::string reason;  // REJECT, VACATION
	bool create = false; // fileinto :create

	// imap4flags: the flags to set on the stored copy. A *snapshot* of the
	// internal flag set as it stood when this action ran, not a reference to it
	// — `addflag` after a `fileinto` must not reach back and change what was
	// already filed, which is the whole subtlety of RFC 5232 §5.
	std::vector<std::string> flags;

	VacationSpec vacation; // VACATION

	Action() = default;
	explicit Action(Type t) : type(t) {}
};

// The envelope values the `envelope` test inspects. These come from the SMTP
// transaction, not from the message headers, which is the whole point of it.
struct Envelope {
	std::string mail_from;
	std::string rcpt_to;
	// The subaddress separator `:user`/`:detail` split on (RFC 5233), which the
	// site configures as qm_subaddress_sep. Empty disables the split.
	std::string separator = "+";

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

// ---- the rule view -------------------------------------------------------
//
// A rule builder needs to show a script as a list of rules and write one back.
// The hard part is not the UI, it is deciding what the source of truth is.
//
// **It is the script text.** `quackmail_sieve_scripts` is also written by
// ManageSieve and by the admin console, so a client can replace a script at any
// moment. Keeping structured rules in a table beside it would mean two sources
// of truth: regenerating from the table would destroy the out-of-band edit, and
// trusting the table instead would make the builder describe something the
// server does not do. Marker comments fail the same way and evaporate the first
// time a third-party client rewrites the script.
//
// So the rules are *derived* from the text every time, by walking the same parse
// tree Evaluate uses. When a script says something the rule view cannot express,
// Decompose says so and the caller must fall back to the raw editor rather than
// silently rewriting it.

// One condition inside a rule.
struct RuleTest {
	// "from" | "to" | "cc" | "subject" | "header:X" | "size" | "body"
	std::string field;
	// "is" | "contains" | "matches" for text; "over" | "under" for size
	std::string op = "contains";
	std::string value;
	bool negate = false;

	RuleTest() = default;
};

// One `if <tests> { <actions> }` in the script.
struct Rule {
	// From a `# rule: <name>` comment above the `if`. A name is the one thing a
	// parse tree cannot carry, and losing it is cosmetic — an unnamed rule shows
	// as "Rule 3" rather than breaking.
	std::string name;
	bool all = true; // allof (every test) vs anyof (any test)
	// Empty means *unconditional*: the actions run for every message and the
	// script has them at the top level with no `if` around them. An
	// out-of-office reply is the shape that needs this — "when should I
	// auto-reply?" has the answer "always" far more often than it has a
	// condition — and a rule list that could not say "always" could not
	// describe one.
	std::vector<RuleTest> tests;
	std::vector<Action> actions;
	bool stop = false; // a trailing `stop;` inside the block

	Rule() = default;
};

// Break a script into rules. False when it uses something the rule view cannot
// represent — a nested `if`, an `else`, a test this does not model — with `why`
// set to a sentence a UI can show. The caller must then offer only the text
// editor: displaying a partial decomposition would describe filtering that is
// not what runs.
bool Decompose(const std::string &script, std::vector<Rule> &out, std::string &why);

// The inverse. The output always parses — Compose is the only writer, so a rule
// set that came from the UI cannot produce a script `Check` rejects.
std::string Compose(const std::vector<Rule> &rules);

} // namespace sieve
} // namespace quackmail
