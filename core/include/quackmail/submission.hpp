#pragma once

#include "duckdb.hpp"

#include <string>
#include <vector>

namespace quackmail {
namespace submission {

// Sending an authenticated user's mail.
//
// Extracted so the SMTP submission listener and JMAP's EmailSubmission/set are
// one implementation rather than two. They differ only in how the message
// arrives: over a socket in dot-stuffed DATA, or as a JSON object naming a
// stored draft. Everything after that — stamp, sign, split local from remote,
// deliver one and queue the other — is the same, and was worth exactly one copy.

struct Result {
	bool ok = true;
	std::string err;
	// Recipients handed to the local store, and recipients put on the relay
	// queue. Reported separately because JMAP's EmailSubmission has a per
	// recipient delivery status and SMTP does not.
	std::vector<std::string> delivered;
	std::vector<std::string> queued;

	Result();
};

// DKIM-sign a message on its way out, choosing the key by the From domain and
// falling back to the envelope sender. Signing failure returns the body
// unchanged: an unsigned message is worse than a signed one and far better than
// no message at all.
std::string Sign(duckdb::Connection &con, const std::string &mail_from, const std::string &body);

// Stamp, sign and send. `received` is prepended before signing, so the
// DKIM-Signature ends up above it and the locally filed copy and every queued
// copy carry the same signature.
//
// Does *not* check the rate limit or charge it: the caller knows how many units
// its protocol counts and when in the exchange to refuse. It does nothing about
// authentication either — `auth_user` is assumed already verified.
bool Send(duckdb::Connection &con, const std::string &mail_from,
          const std::vector<std::string> &rcpts, const std::string &received,
          const std::string &body, Result &out);

// The Received header a front-end stamps on. Kept here so the two agree about
// its shape, which is what makes a message's path readable in a header dump
// whichever door it came in by.
std::string ReceivedHeader(duckdb::Connection &con, const std::string &via, const std::string &auth_user,
                           bool tls);

} // namespace submission
} // namespace quackmail
