#pragma once

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace quackmail {
namespace deliver {

// Parse an RFC822 message, run each local recipient's Sieve filter, and deliver
// it into that user's Citadel Mail room (or a Sieve `fileinto` room) as one
// shared format_type=4 message pointed into every destination room. `rcpts` are
// the envelope recipients; each should already be a validated local user
// (unknown or Sieve-discarded recipients simply contribute no room). Returns
// false + err only on a storage error; an all-filtered message is a no-op.
bool LocalDeliver(duckdb::Connection &con, const std::string &mail_from,
                  const std::vector<std::string> &rcpts, const std::string &body, std::string &err);

} // namespace deliver
} // namespace quackmail
