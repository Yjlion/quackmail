#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace quackmail {
namespace util {

std::string Base64Encode(const std::string &in);
// Returns false if the input is not valid base64.
bool Base64Decode(const std::string &in, std::string &out);

// Uppercase ASCII copy (for case-insensitive command matching).
std::string Upper(const std::string &s);

// Lowercase ASCII copy.
std::string Lower(const std::string &s);

// The local-part of an address (before '@'); returns the whole string if no '@'.
std::string LocalPart(const std::string &addr);

// The RFC 5322 date a message header carries ("Mon, 28 Jul 2026 12:00:00 +0000"),
// always in UTC. `epoch` of 0 means now.
std::string RfcDate(int64_t epoch = 0);

// ---- randomness and digests ---------------------------------------------
// Cryptographically strong bytes from OpenSSL. Returns false if the RNG failed;
// callers that mint secrets must treat that as fatal rather than continuing
// with a predictable value.
bool RandomBytes(size_t n, std::string &out);
// Hex / base64url-encoded random. Both return "" if the RNG failed.
std::string RandomHex(size_t bytes);
std::string RandomBase64Url(size_t bytes);

// Lowercase hex SHA-256 of the input.
std::string Sha256Hex(const std::string &in);

// Constant-time comparison, for secrets. False for differing lengths.
bool SecureEquals(const std::string &a, const std::string &b);

} // namespace util
} // namespace quackmail
