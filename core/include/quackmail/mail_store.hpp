#pragma once

#include "duckdb.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace quackmail {
namespace store {

// Create the shared schema if it does not already exist: local user
// credentials, the Sieve script table, the outbound queue, and (via
// citadel::EnsureCitadelSchema) the Citadel room/floor/message model that
// serves as the message store. Idempotent, so every extension can call it on
// start without a load-order dependency.
void EnsureSchema(duckdb::Connection &con);

// ---- outbound relay queue (quackmail_outbound) --------------------------
// One remote recipient awaiting relay. `raw` holds the full RFC822 message.
struct OutboundItem {
	int64_t id = 0;
	std::string from_addr;
	std::string rcpt;
	std::string raw;
	int attempts = 0;
};

// Enqueue one message for one remote recipient (status 'queued').
void EnqueueOutbound(duckdb::Connection &con, const std::string &from_addr, const std::string &rcpt,
                     const std::string &raw);

// Atomically claim up to `limit` due items (status 'queued' and next_attempt_at
// <= now()), flipping them to 'sending' so a single drainer never double-sends.
std::vector<OutboundItem> ClaimOutboundDue(duckdb::Connection &con, int limit);

// Terminal / retry transitions for a claimed item.
void MarkSent(duckdb::Connection &con, int64_t id);
void MarkFailed(duckdb::Connection &con, int64_t id, const std::string &err);
// Requeue with an incremented attempt count and a backoff (seconds from now).
void MarkRetry(duckdb::Connection &con, int64_t id, int attempts, int backoff_secs, const std::string &err);

} // namespace store
} // namespace quackmail
