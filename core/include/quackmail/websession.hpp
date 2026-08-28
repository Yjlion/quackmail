#pragma once

#include "duckdb.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace quackmail {
namespace web {

// Browser sessions for the HTTP front-end.
//
// The Citadel protocols are stateful connections, so until now "session" meant
// citadel_sessions — a presence row keyed by a predictable sequence number.
// HTTP is stateless, so it needs a real bearer secret, and that secret needs
// the properties a presence row does not have: unguessable, hashed at rest,
// expiring, revocable, and pinned to the transport it was minted over.
//
// It lives in core rather than in the http module so the admin CLI can list and
// revoke sessions whether or not the http extension is loaded — the same
// "tables are the bus" rule the rest of the server follows.

struct Session {
	std::string token_hash; // sha256 hex; the raw token is never stored
	std::string username;
	std::string csrf; // the raw CSRF token to put in this session's forms
	bool tls = false; // the session was established over TLS
	int64_t axlevel = 0;
	// This browser's row in citadel_sessions, so the web appears in RWHO beside
	// telnet and XMPP. 0 when presence could not be registered.
	int64_t presence_id = 0;
	// Whether that row is due a refresh. Set on the same once-a-minute schedule
	// the idle window is rewritten on, so a page load that also fetches a
	// stylesheet and four icons costs one UPDATE rather than six.
	bool presence_stale = false;
};

// How often a browser is expected to refresh its presence row. Declared to
// RegisterSession, which is what lets ReapSessions hold it to the promise: a
// closed tab has no close event to unregister on.
constexpr int64_t kWebHeartbeatSeconds = 60;

struct SessionRow {
	std::string token_hash;
	std::string username;
	std::string peer_ip;
	std::string user_agent;
	bool tls = false;
	int64_t created_at = 0; // unix seconds
	int64_t last_seen = 0;
	int64_t expires_at = 0;
};

// Create the session tables. Called from store::EnsureSchema.
void EnsureSchema(duckdb::Connection &con);

// Mint a session for an already-authenticated user. `out_token` is the raw
// secret to put in the cookie; it is never stored. Returns false (with err) if
// the RNG failed — a caller must treat that as fatal rather than proceeding
// with a predictable token.
bool CreateSession(duckdb::Connection &con, const std::string &username, bool tls,
                   const std::string &peer_ip, const std::string &user_agent, std::string &out_token,
                   std::string &err);

// Resolve a raw cookie value to a live session, enforcing expiry, revocation
// and transport pinning, and refreshing the idle window. Returns false when
// there is no usable session — the caller should then treat the request as
// anonymous and clear the cookie.
bool LookupSession(duckdb::Connection &con, const std::string &token, bool tls, Session &out);

// The CSRF token for a session, to be rendered into a hidden `_csrf` field on
// every form. It is derived from a server-side secret and the session's token
// hash rather than stored, so it is reproducible on every request while never
// appearing in a cookie — which is what makes this a synchronizer token rather
// than the weaker double-submit pattern.
std::string CsrfToken(duckdb::Connection &con, const std::string &token_hash);
// Constant-time comparison of a submitted token against the expected one.
bool CheckCsrf(duckdb::Connection &con, const std::string &token_hash, const std::string &submitted);

// The same idea for a visitor who has no session yet — the login form. Bound to
// the client address and a coarse time bucket instead of a session row, so a
// third party cannot mint one (they do not have the server secret) and login
// CSRF is closed without storing state for every anonymous visitor.
std::string AnonCsrfToken(duckdb::Connection &con, const std::string &peer_ip);
bool CheckAnonCsrf(duckdb::Connection &con, const std::string &peer_ip, const std::string &submitted);

void RevokeSession(duckdb::Connection &con, const std::string &token);
void RevokeByHash(duckdb::Connection &con, const std::string &token_hash, const std::string &username);
// Every session for a user. A password change must call this: auth::AddUser is
// an INSERT OR REPLACE, so without it the old password's sessions survive it.
void RevokeAllForUser(duckdb::Connection &con, const std::string &username);

// Drop expired and revoked rows. Cheap; call opportunistically rather than
// running a background thread (the same posture as policy::PruneSendLog).
void PruneSessions(duckdb::Connection &con);

// username "" lists every session (the admin view).
std::vector<SessionRow> ListSessions(duckdb::Connection &con, const std::string &username);

// ---- login throttling ---------------------------------------------------
// A world-reachable browser login form over single-round salted SHA-256 makes
// online guessing the live threat, so failures are counted per client address.

// False when this address has failed too often recently; retry_after is set to
// the number of seconds until it may try again.
bool LoginAllowed(duckdb::Connection &con, const std::string &peer_ip, int64_t &retry_after);
void RecordLoginFailure(duckdb::Connection &con, const std::string &peer_ip, const std::string &username);
void ClearLoginFailures(duckdb::Connection &con, const std::string &peer_ip);

} // namespace web
} // namespace quackmail
