#pragma once

#include "duckdb.hpp"
#include "quackmail/mime.hpp"

#include <string>

namespace quackmail {
namespace sieve {

struct Action {
	enum Type { KEEP, DISCARD, FILEINTO };
	Type type = KEEP;
	std::string folder; // for FILEINTO
};

// Evaluate a (subset of) RFC-5228 Sieve script against a parsed message.
// Supported: keep; discard; fileinto "Folder";
// and a single guard: if header :contains "Name" "substr" { <action> }
// Anything unrecognised is ignored; the default is KEEP into INBOX.
Action Evaluate(const std::string &script, const mime::ParsedMessage &msg);

// Load the active Sieve script for a user (empty string if none).
std::string LoadActiveScript(duckdb::Connection &con, const std::string &username);

} // namespace sieve
} // namespace quackmail
