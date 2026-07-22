#include "quackmail/mail_store.hpp"

#include "quackmail/citadel_store.hpp"

#include <memory>

namespace quackmail {
namespace store {

using duckdb::Connection;

void EnsureSchema(Connection &con) {
	// A sequence supplies ids for the outbound queue.
	con.Query("CREATE SEQUENCE IF NOT EXISTS quackmail_msg_seq START 1");

	// Outbound relay/submission queue (drained by smtp_out).
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_outbound (
			id              BIGINT DEFAULT nextval('quackmail_msg_seq'),
			from_addr       VARCHAR,
			rcpt            VARCHAR,
			raw             BLOB,
			status          VARCHAR DEFAULT 'queued',
			attempts        INTEGER DEFAULT 0,
			last_error      VARCHAR,
			next_attempt_at TIMESTAMP DEFAULT now(),
			created_at      TIMESTAMP DEFAULT now()
		)
	)");

	// Per-user Sieve scripts (consulted by smtp_in delivery).
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_sieve_scripts (
			username VARCHAR,
			name     VARCHAR,
			active   BOOLEAN DEFAULT false,
			script   VARCHAR
		)
	)");

	// Local user credentials (verified by SASL AUTH and the Citadel USER/PASS).
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_users (
			username      VARCHAR PRIMARY KEY,
			password_hash VARCHAR,
			salt          VARCHAR,
			algo          VARCHAR DEFAULT 'sha256',
			enabled       BOOLEAN DEFAULT true,
			created_at    TIMESTAMP DEFAULT now()
		)
	)");

	// The Citadel room/floor/message model is the message store. Create it here
	// so every extension gets the full schema on load, regardless of load order.
	citadel::EnsureCitadelSchema(con);
}

} // namespace store
} // namespace quackmail
