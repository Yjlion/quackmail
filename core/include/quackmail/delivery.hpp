#pragma once

#include "duckdb.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace quackmail {
namespace deliver {

// Knobs the SMTP front-ends set per message.
struct Options {
	// When set, every recipient's copy is filed into this folder and the Sieve
	// engine is skipped. Used for a DMARC quarantine, where the site's judgement
	// must override the user's filter rather than compete with it.
	std::string folder_override;

	// Rooms the message must also be pointed into, regardless of recipients —
	// how a public room address (room_<name>@) is delivered. They are merged
	// into the same stored message, so mail to a room and a user is stored once.
	std::vector<int64_t> extra_rooms;

	// Per-recipient subaddress detail: "bob@x" -> "receipts" for bob+receipts@x
	// (RFC 5233). Files into that existing personal folder, and is offered to
	// the user's Sieve script as `envelope :detail`.
	std::vector<std::pair<std::string, std::string>> subaddress;

	// The separator the detail was split on, for `envelope :detail`.
	std::string subaddress_sep = "+";

	// Create a subaddressed folder that does not exist yet. Off by default: any
	// sender can pick the folder name, so this would let one mint rooms in a
	// user's account. Without it an unknown folder falls back to the inbox.
	bool subaddress_create = false;

	Options() = default;
};

struct Outcome {
	bool ok = true;
	std::string err;
	int64_t msgnum = -1;
	// Recipients whose Sieve script issued `reject`, with the reason. LMTP
	// reports these per recipient; SMTP folds them into one reply.
	std::vector<std::pair<std::string, std::string>> rejected;
	// Recipients whose filter discarded the message (delivered nowhere, but not
	// an error — the user asked for exactly this).
	std::vector<std::string> discarded;
	// Recipients refused because their mailbox is over its storage quota. Kept
	// apart from `rejected` on purpose: a Sieve reject is the user's own
	// permanent decision and earns a 5.x.x, while a full mailbox is a condition
	// that clears and earns a 4.x.x. Folding the two would turn a deferral the
	// sender would have retried into a bounce they cannot.
	std::vector<std::string> over_quota;

	Outcome() = default;
};

// Parse an RFC822 message, run each local recipient's Sieve filter, and deliver
// it into the resulting Citadel rooms as one shared format_type=4 message
// pointed into every destination room. `rcpts` are the envelope recipients;
// each should already be a validated local user.
//
// Sieve `redirect` actions are enqueued onto the outbound relay queue; `reject`
// and `discard` suppress delivery for that recipient and are reported in
// `out`. Returns false only on a storage error.
bool LocalDeliver(duckdb::Connection &con, const std::string &mail_from,
                  const std::vector<std::string> &rcpts, const std::string &body,
                  const Options &opts, Outcome &out);

// Convenience wrapper for callers that only care whether storage worked.
bool LocalDeliver(duckdb::Connection &con, const std::string &mail_from,
                  const std::vector<std::string> &rcpts, const std::string &body, std::string &err);

} // namespace deliver
} // namespace quackmail
