#pragma once

#include <string>
#include <utility>
#include <vector>

namespace quackmail {
namespace mime {

struct ParsedMessage {
	std::string from;       // From: header address (raw)
	std::string subject;    // Subject: header (decoded as-is)
	std::string message_id; // Message-ID: header
	std::vector<std::pair<std::string, std::string>> headers;
};

// Parse an RFC-5322 message: split headers from body, unfold, and pull out a
// few well-known fields. Header names are returned as-is (case preserved), the
// convenience accessors are case-insensitive.
ParsedMessage Parse(const std::string &raw);

// Extract the bare address (<...>) from a header value like: Foo <a@b.com>.
std::string ExtractAddress(const std::string &header_value);

} // namespace mime
} // namespace quackmail
