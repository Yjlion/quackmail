#pragma once

#include "duckdb.hpp"

#include <string>

namespace quackmail {
namespace auth {

// Password storage.
//
// **scrypt** (RFC 7914), through OpenSSL's EVP_PBE_scrypt — no new dependency,
// and available since OpenSSL 1.1.0, unlike Argon2 which needs 3.2. It is
// memory-hard, which is the property that matters: a password KDF is only ever
// as good as how badly it scales on the attacker's hardware, and PBKDF2 and
// bcrypt both fit comfortably in a GPU's registers where scrypt does not.
//
// This replaced a single round of salted SHA-256, which a leaked
// `quackmail_users` table gave up at billions of guesses per second.
//
// `algo` is **self-describing** — "scrypt$16384$8$1" — so the work factor can
// be raised later without invalidating rows already stored at the old one. The
// legacy value "sha256" is still accepted on the way in, and a row carrying it
// is rewritten the next time its owner successfully signs in; see Verify.

struct Stored {
	std::string algo; // "scrypt$<N>$<r>$<p>", or the legacy "sha256"
	std::string salt; // hex
	std::string hash; // hex

	Stored();
};

// Hash `password` at the current default work factor, with a fresh salt.
Stored HashPassword(const std::string &password);

// Check a password against a stored triple, in constant time. Understands both
// the current scheme and the legacy one, so an existing database keeps working.
bool CheckPassword(const std::string &password, const Stored &stored);

// True when `algo` is not what HashPassword would produce today — either the
// legacy scheme or a lower work factor than the current default.
bool NeedsRehash(const std::string &algo);

// Create (or replace) a local user with a hashed password. Returns false + err
// on failure.
bool AddUser(duckdb::Connection &con, const std::string &username, const std::string &password,
             std::string &err);

// Remove a local user. Returns false + err on failure.
bool RemoveUser(duckdb::Connection &con, const std::string &username, std::string &err);

// Verify a username/password against quackmail_users using a constant-time
// compare. Returns true only for an enabled user with a matching hash.
bool Verify(duckdb::Connection &con, const std::string &username, const std::string &password);

} // namespace auth
} // namespace quackmail
