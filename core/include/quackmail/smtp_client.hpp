#pragma once

#include <string>

namespace quackmail {
namespace smtp {

enum class SendStatus {
	Sent,      // accepted by the peer (2xx after end-of-DATA)
	Transient, // 4xx or connection error — retry later
	Permanent, // 5xx — give up
};

struct ClientOpts {
	bool use_starttls = true; // attempt STARTTLS when the peer advertises it
	std::string auth_user;    // if set, AUTH PLAIN before MAIL (submission/smarthost)
	std::string auth_pass;
	std::string helo_name = "quackmail";
};

struct SendResult {
	SendStatus status = SendStatus::Transient;
	std::string info; // last server reply / error detail (for the queue's last_error)
};

// Deliver one message (single recipient) to host:port over SMTP: read greeting,
// EHLO, optionally STARTTLS + AUTH, then MAIL/RCPT/DATA. The outcome is
// classified for the relay drainer's retry logic. This performs a blocking
// connect and is meant to run on the drainer's background thread.
SendResult Deliver(const std::string &host, int port, const std::string &mail_from, const std::string &rcpt,
                   const std::string &raw, const ClientOpts &opts);

} // namespace smtp
} // namespace quackmail
