#pragma once

#include "duckdb.hpp"

#include <string>
#include <utility>
#include <vector>

namespace quackmail {
namespace store {

// Create the shared QuackMail schema if it does not already exist. Idempotent,
// so every extension can call it on start without a load-order dependency.
void EnsureSchema(duckdb::Connection &con);

// A parsed, ready-to-store inbound message.
struct StoredMessage {
	std::string mailbox;     // destination mailbox / folder ("INBOX" default)
	std::string from_addr;   // envelope MAIL FROM
	std::string subject;     // parsed Subject header
	std::string message_id;  // parsed Message-ID header
	std::string raw;         // full RFC-5322 bytes
	std::vector<std::string> recipients; // envelope RCPT TO list
	std::vector<std::pair<std::string, std::string>> headers;
};

// Insert a message plus its recipients and headers in one transaction.
// Returns the new message id, or -1 on error (with err set).
int64_t InsertMessage(duckdb::Connection &con, const StoredMessage &msg, std::string &err);

} // namespace store
} // namespace quackmail
