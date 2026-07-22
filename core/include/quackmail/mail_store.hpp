#pragma once

#include "duckdb.hpp"

namespace quackmail {
namespace store {

// Create the shared schema if it does not already exist: local user
// credentials, the Sieve script table, the outbound queue, and (via
// citadel::EnsureCitadelSchema) the Citadel room/floor/message model that
// serves as the message store. Idempotent, so every extension can call it on
// start without a load-order dependency.
void EnsureSchema(duckdb::Connection &con);

} // namespace store
} // namespace quackmail
