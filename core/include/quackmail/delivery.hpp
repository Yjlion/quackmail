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
