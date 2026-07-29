#pragma once

#include "duckdb.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace quackmail {
namespace fetch {

// Pulling messages *in* from somewhere else: a POP3 or IMAP mailbox on another
// server, or an RSS/Atom feed. Whatever is new is posted into a Citadel room —
// so a newsgroup, a webmail account and a blog all end up readable through the
// same BBS, mail client or newsreader as everything else.
//
// The transports live in core/mail_client.cpp (POP3/IMAP) and
// core/http_client.cpp + core/feed.cpp (RSS). This file is the model: what to
// poll, where to put it, and what has already been seen.

void EnsureSchema(duckdb::Connection &con);

enum class Kind {
	Pop3,
	Imap,
	Rss,
};

enum class Tls {
	None,
	StartTls,
	Implicit,
};

struct Feed {
	int64_t id = 0;
	std::string name; // unique; also names the synthesized From: address
	Kind kind = Kind::Rss;
	bool enabled = true;

	std::string url; // RSS
	std::string host;
	int64_t port = 0;
	Tls tls = Tls::StartTls;
	std::string username;
	std::string password;
	std::string mailbox = "INBOX"; // IMAP

	// Exactly one of these. A room takes the message as-is; a user routes it
	// through deliver::LocalDeliver, so their Sieve script runs.
	int64_t target_room = -1;
	std::string target_user;

	std::string author_override; // display name to use instead of the item's
	std::string subject_prefix;

	int64_t interval_secs = 900;
	bool leave_on_server = true;
	int64_t max_per_run = 50;
	int64_t max_bytes = 5 * 1024 * 1024;

	// Resume state.
	int64_t last_run_at = 0;
	std::string last_status; // "ok" | "error" | "unchanged" | ""
	std::string last_error;
	int64_t uidvalidity = 0;
	int64_t last_uid = 0;
	std::string etag;
	std::string last_modified;
	int64_t messages_pulled = 0;

	Feed() = default;
};

std::vector<Feed> ListFeeds(duckdb::Connection &con, bool enabled_only = false);
bool GetFeed(duckdb::Connection &con, const std::string &name, Feed &out);
// Create or update by name. Validates the kind's required fields and that the
// target resolves.
bool SetFeed(duckdb::Connection &con, const Feed &feed, std::string &err);
bool RemoveFeed(duckdb::Connection &con, const std::string &name, std::string &err);
// Set one field by name, for the admin CLI and the web form. Keys mirror the
// struct: kind, enabled, url, host, port, tls, username, password, mailbox,
// room, user, author, subject_prefix, interval, leave_on_server, max_per_run.
bool SetField(duckdb::Connection &con, const std::string &name, const std::string &key,
              const std::string &value, std::string &err);

struct RunResult {
	std::string feed;
	int64_t fetched = 0; // items downloaded
	int64_t stored = 0;  // messages posted into a room / delivered
	int64_t skipped = 0; // already seen
	std::string status;
	std::string error;
};

// Poll one feed once, whether or not its interval has elapsed. Failures are
// recorded on the row rather than thrown: one unreachable server must not stop
// the others.
bool RunFeed(duckdb::Connection &con, const Feed &feed, RunResult &out);
// Every enabled feed whose interval has elapsed. This is the worker's tick.
std::vector<RunResult> RunDue(duckdb::Connection &con, bool force = false);

// Connect and authenticate without touching the mailbox — the admin console's
// "test" button.
bool TestFeed(duckdb::Connection &con, const std::string &name, std::string &info, std::string &err);

// Names, for the SQL surface.
std::string KindName(Kind k);
Kind ParseKind(const std::string &s);
std::string TlsName(Tls t);
Tls ParseTls(const std::string &s);

} // namespace fetch
} // namespace quackmail
