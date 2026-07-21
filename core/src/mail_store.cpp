#include "quackmail/mail_store.hpp"

#include <memory>

namespace quackmail {
namespace store {

using duckdb::Connection;
using duckdb::PreparedStatement;
using duckdb::Value;

void EnsureSchema(Connection &con) {
	// A sequence supplies message ids so inbound/outbound paths stay independent.
	con.Query("CREATE SEQUENCE IF NOT EXISTS quackmail_msg_seq START 1");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_messages (
			id          BIGINT PRIMARY KEY,
			received_at TIMESTAMP DEFAULT now(),
			mailbox     VARCHAR,
			from_addr   VARCHAR,
			subject     VARCHAR,
			message_id  VARCHAR,
			size_bytes  BIGINT,
			raw         BLOB
		)
	)");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_recipients (
			message_id BIGINT,
			rcpt       VARCHAR
		)
	)");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_headers (
			message_id BIGINT,
			name       VARCHAR,
			value      VARCHAR
		)
	)");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_mailboxes (
			username    VARCHAR,
			name        VARCHAR,
			uidvalidity BIGINT DEFAULT 1,
			flags       VARCHAR
		)
	)");

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_message_flags (
			message_id BIGINT,
			flag       VARCHAR
		)
	)");

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

	con.Query(R"(
		CREATE TABLE IF NOT EXISTS quackmail_sieve_scripts (
			username VARCHAR,
			name     VARCHAR,
			active   BOOLEAN DEFAULT false,
			script   VARCHAR
		)
	)");

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
}

int64_t InsertMessage(Connection &con, const StoredMessage &msg, std::string &err) {
	EnsureSchema(con);

	auto begin = con.Query("BEGIN TRANSACTION");
	if (begin->HasError()) {
		err = begin->GetError();
		return -1;
	}

	auto fail = [&](const std::string &e) -> int64_t {
		err = e;
		con.Query("ROLLBACK");
		return -1;
	};

	auto id_res = con.Query("SELECT nextval('quackmail_msg_seq')");
	if (id_res->HasError()) {
		return fail(id_res->GetError());
	}
	int64_t id = id_res->GetValue(0, 0).GetValue<int64_t>();

	{
		auto stmt = con.Prepare("INSERT INTO quackmail_messages "
		                        "(id, mailbox, from_addr, subject, message_id, size_bytes, raw) "
		                        "VALUES ($1, $2, $3, $4, $5, $6, $7)");
		if (stmt->HasError()) {
			return fail(stmt->GetError());
		}
		duckdb::vector<Value> params = {
		    Value::BIGINT(id),
		    Value(msg.mailbox),
		    Value(msg.from_addr),
		    Value(msg.subject),
		    Value(msg.message_id),
		    Value::BIGINT((int64_t)msg.raw.size()),
		    Value::BLOB(reinterpret_cast<const duckdb::data_t *>(msg.raw.data()), msg.raw.size())};
		auto r = stmt->Execute(params);
		if (r->HasError()) {
			return fail(r->GetError());
		}
	}

	{
		auto stmt = con.Prepare("INSERT INTO quackmail_recipients (message_id, rcpt) VALUES ($1, $2)");
		if (stmt->HasError()) {
			return fail(stmt->GetError());
		}
		for (auto &rcpt : msg.recipients) {
			duckdb::vector<Value> params = {Value::BIGINT(id), Value(rcpt)};
			auto r = stmt->Execute(params);
			if (r->HasError()) {
				return fail(r->GetError());
			}
		}
	}

	{
		auto stmt = con.Prepare("INSERT INTO quackmail_headers (message_id, name, value) VALUES ($1, $2, $3)");
		if (stmt->HasError()) {
			return fail(stmt->GetError());
		}
		for (auto &h : msg.headers) {
			duckdb::vector<Value> params = {Value::BIGINT(id), Value(h.first), Value(h.second)};
			auto r = stmt->Execute(params);
			if (r->HasError()) {
				return fail(r->GetError());
			}
		}
	}

	auto commit = con.Query("COMMIT");
	if (commit->HasError()) {
		return fail(commit->GetError());
	}
	return id;
}

} // namespace store
} // namespace quackmail
