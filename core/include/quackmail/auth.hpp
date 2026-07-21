#pragma once

#include "duckdb.hpp"

#include <string>

namespace quackmail {
namespace auth {

// Generate a random hex salt.
std::string GenSalt();

// Salted SHA-256 (hex) of a password. First-pass KDF; bcrypt/argon2 is a noted
// hardening follow-up.
std::string HashPassword(const std::string &password, const std::string &salt);

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
