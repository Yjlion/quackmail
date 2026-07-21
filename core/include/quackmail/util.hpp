#pragma once

#include <string>

namespace quackmail {
namespace util {

std::string Base64Encode(const std::string &in);
// Returns false if the input is not valid base64.
bool Base64Decode(const std::string &in, std::string &out);

// Uppercase ASCII copy (for case-insensitive command matching).
std::string Upper(const std::string &s);

// The local-part of an address (before '@'); returns the whole string if no '@'.
std::string LocalPart(const std::string &addr);

} // namespace util
} // namespace quackmail
