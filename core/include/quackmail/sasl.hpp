#pragma once

#include "duckdb.hpp"

#include <functional>
#include <string>

namespace quackmail {
namespace sasl {

enum class Result { Ok, Fail, Unsupported };

// Drive a server-side SASL exchange for `mechanism` (upper-cased "PLAIN" or
// "LOGIN"). `initial` is the optional initial response (base64, may be empty).
//
// The caller supplies `challenge`, which issues a base64 prompt to the client
// using that protocol's own continuation framing (SMTP "334 ...", IMAP "+ ...")
// and reads the client's base64 response line into `response`, returning false
// on an I/O error. This keeps the mechanism logic here and the wire framing in
// the caller.
//
// On Result::Ok, `user` holds the verified username (checked via auth::Verify).
// No final status line is written here — the caller emits its own success or
// failure reply.
Result ServerAuth(duckdb::Connection &con, const std::string &mechanism, const std::string &initial,
                  const std::function<bool(const std::string &challenge_b64, std::string &response)> &challenge,
                  std::string &user);

} // namespace sasl
} // namespace quackmail
