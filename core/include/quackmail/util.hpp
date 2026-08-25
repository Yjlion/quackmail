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

// base64url (RFC 4648 §5) over arbitrary bytes, unpadded. RandomBase64Url above
// is a *generator* and cannot encode anything you already have; JWS needs both
// halves of this, and so does anything else that has to put bytes in a URL.
std::string Base64UrlEncode(const std::string &in);
bool Base64UrlDecode(const std::string &in, std::string &out);

// Lowercase hex SHA-256 of the input.
std::string Sha256Hex(const std::string &in);

// The same digest as raw bytes. An RFC 7638 JWK thumbprint is base64url over
// the digest, not over its hex spelling, so hex is the wrong currency there.
std::string Sha256Raw(const std::string &in);

// Constant-time comparison, for secrets. False for differing lengths.
bool SecureEquals(const std::string &a, const std::string &b);

} // namespace util
} // namespace quackmail
