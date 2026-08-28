#pragma once

#include "quackmail/citadel_store.hpp"

#include "duckdb.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace quackmail {
namespace quota {

// Per-user storage quotas.
//
// Distinct from policy::CheckRate, which limits how fast a user may *send*.
// This limits how many bytes a user may *keep*, and it is asked by every door
// onto the store rather than only by the two SMTP front-ends — which is why it
// lives here rather than beside the rate limiter in mailpolicy.hpp, whose remit
// is site policy for SMTP. Putting it there would also make citadel_store.cpp
// include mailpolicy.hpp, which is the wrong way round.
//
// The unit is the RFC822 wire size — exactly what IMAP reports as RFC822.SIZE —
// so a user adding up the sizes in a FETCH gets the number GETQUOTA gave them.
// That is not the length of the stored BLOB: a native format-0 message has no
// header block on disk and RenderRfc822 synthesizes one.

void EnsureSchema(duckdb::Connection &con);

struct Limit {
	std::string username;    // "" is the site default, applied to everyone else
	int64_t limit_bytes = 0; // 0 = unlimited
	bool enabled = true;
	int64_t changed_at = 0;
};

struct Info {
	std::string username;
	int64_t used_bytes = 0;
	int64_t limit_bytes = 0; // 0 = unlimited
	bool limited = false;    // limit_bytes > 0 && enabled
	bool over = false;       // limited && used >= limit
};

// The limit that applies to `username`: their own row, else the '' default.
// Never fails; an unknown user gets the default.
Limit LimitFor(duckdb::Connection &con, const std::string &username);

// Bytes this user is currently keeping, and the limit that applies.
//
// Usage is summed over DISTINCT msgnum reached through citadel_room_msgs from
// the rooms this user owns, never over citadel_messages directly. Both halves
// are load-bearing: LocalDeliver stores one message row and points it into every
// destination room, so summing per pointer would charge a user twice for a
// message in both Mail and Sent Items; and citadel_messages rows are never
// deleted — Unlink drops the pointer and writes a tombstone — so summing the
// message table would charge everyone forever for mail they threw away.
//
// When the limit is 0 the sum is skipped entirely and used_bytes stays 0. The
// overwhelmingly common configuration is unlimited, and it should cost one
// primary-key probe rather than an aggregate on every RCPT.
Info Usage(duckdb::Connection &con, const std::string &username);

// As above, but always computes usage even when unlimited. For the "you are
// using X" displays, which want the number whether or not there is a ceiling.
Info UsageAlways(duckdb::Connection &con, const std::string &username);

// Would storing `extra_bytes` more put this user over? False when unlimited.
// The predicate for a door that knows the size: IMAP APPEND has the literal
// length, LocalDeliver has the body.
bool WouldExceed(duckdb::Connection &con, const std::string &username, int64_t extra_bytes);

// Is this user already at or over the ceiling? False when unlimited. The
// predicate for a door that does not yet know the size — SMTP RCPT, which is
// asked before DATA and which this server never gets an ESMTP SIZE from.
bool IsOver(duckdb::Connection &con, const std::string &username);

// The wire size one message contributes. Exposed because InsertMessage has to
// compute it before the row exists, and because the backfill needs the same
// arithmetic for rows that predate the size_bytes column.
int64_t MessageSize(const citadel::Message &msg, const std::string &node);

// 0 clears the limit (unlimited). username "" sets the site default.
bool SetQuota(duckdb::Connection &con, const std::string &username, int64_t limit_bytes,
              std::string &err);
std::vector<Limit> ListQuotas(duckdb::Connection &con);

// The largest changed_at across this user's row and the default row. JMAP's
// Quota state is (account state, this): the first moves when stored bytes move,
// the second when an admin moves the ceiling, and a state that missed either
// would make Quota/changes lie.
//
// changed_at is a monotonic generation seeded from the clock rather than the
// clock itself — see SetQuota. Two edits in the same second have to be
// distinguishable, or a client polling between them is told nothing happened.
int64_t LimitGeneration(duckdb::Connection &con, const std::string &username);

// Fill in citadel_messages.size_bytes for rows that predate the column and
// cannot be backfilled in SQL (format 0/1, whose wire form is synthesized).
// Idempotent and bounded. Returns rows updated.
int64_t BackfillSizes(duckdb::Connection &con);

} // namespace quota
} // namespace quackmail
