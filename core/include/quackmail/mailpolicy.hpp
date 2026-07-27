#pragma once

#include "duckdb.hpp"
#include "quackmail/dkim.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace quackmail {
namespace policy {

// Site policy for the SMTP front-ends: which domains we host, how addresses are
// rewritten, who may talk to us, how fast a user may send, and which key signs
// their outbound mail.
//
// Everything lives in DuckDB tables so the two SMTP extensions, the admin table
// functions and the CLI all see one consistent view without sharing C++ state.

// Create the policy tables. Called from store::EnsureSchema, so every extension
// gets them regardless of load order. Idempotent.
void EnsureSchema(duckdb::Connection &con);

// ---- domains ------------------------------------------------------------

struct Domain {
	std::string domain;
	std::string kind; // "local" (deliver here) | "relay" (accept and forward)
	bool enabled = true;
	std::string dkim_selector;
	std::string note;
};

// True when `domain` is one we accept mail for. The configured c_fqdn always
// counts, so a single-domain install needs no rows at all.
bool IsLocalDomain(duckdb::Connection &con, const std::string &domain);
std::vector<Domain> ListDomains(duckdb::Connection &con);
bool AddDomain(duckdb::Connection &con, const std::string &domain, const std::string &kind,
               std::string &err);
bool RemoveDomain(duckdb::Connection &con, const std::string &domain, std::string &err);

// ---- aliases ------------------------------------------------------------

// Expand `addr` through the alias table, returning the destination addresses.
// Handles exact aliases ("sales@x" -> "alice") and per-domain catch-alls
// ("@x" -> "alice"). An empty result means the address is not aliased; the
// caller then falls back to matching a local user directly. Expansion follows
// alias chains, with a depth cap so a cycle cannot hang the RCPT handler.
std::vector<std::string> ExpandAlias(duckdb::Connection &con, const std::string &addr);
bool AddAlias(duckdb::Connection &con, const std::string &alias, const std::string &destination,
              std::string &err);
bool RemoveAlias(duckdb::Connection &con, const std::string &alias, const std::string &destination,
                 std::string &err);

// ---- access control -----------------------------------------------------

enum class AclVerdict {
	None,  // no rule matched
	Allow, // explicitly allow-listed (skips later blocks and the DNSBL)
	Block, // explicitly blocked
};

// Match `value` against the rules for `scope` ("ip", "sender", "domain",
// "rcpt", "helo"). Allow beats block, so a narrow allow rule can carve an
// exception out of a broad block. Patterns are wildmat globs; the "ip" scope
// additionally understands CIDR notation ("192.0.2.0/24").
AclVerdict CheckAcl(duckdb::Connection &con, const std::string &scope, const std::string &value,
                    std::string &note);

// Does a client address match one pattern? A pattern containing '/' is treated
// as a CIDR block, anything else as a wildmat glob. Exposed because the same
// question comes up outside the ACL tables — the web front-end asks it of its
// trusted-proxy list.
bool IpMatches(const std::string &ip, const std::string &pattern);
// As above, over a comma-separated list. Empty list = no match.
bool IpMatchesAny(const std::string &ip, const std::string &patterns);
bool AddAcl(duckdb::Connection &con, const std::string &scope, const std::string &pattern,
            const std::string &action, const std::string &note, std::string &err);
bool RemoveAcl(duckdb::Connection &con, int64_t id, std::string &err);

// ---- DNSBL --------------------------------------------------------------

// Enabled DNSBL zones, in priority order. Empty by default: DNSBL checking is
// opt-in so a fresh install issues no third-party DNS queries.
std::vector<std::string> RblZones(duckdb::Connection &con);
bool AddRblZone(duckdb::Connection &con, const std::string &zone, std::string &err);
bool RemoveRblZone(duckdb::Connection &con, const std::string &zone, std::string &err);

// ---- DKIM keys ----------------------------------------------------------

struct DkimKey {
	std::string domain;
	std::string selector;
	std::string private_key; // PEM
	std::string public_key;  // base64 SPKI, for the DNS record
	std::string algo;
	std::string headers; // the h= list to sign
	bool enabled = true;
};

// The signing key for `domain`, falling back to the organizational domain so a
// key on example.com also signs mail from mail.example.com.
bool DkimKeyFor(duckdb::Connection &con, const std::string &domain, DkimKey &out);
std::vector<DkimKey> ListDkimKeys(duckdb::Connection &con);
// Generate and store a new RSA key pair. `dns_record` receives the TXT record
// body to publish at <selector>._domainkey.<domain>.
bool GenerateDkimKey(duckdb::Connection &con, const std::string &domain, const std::string &selector,
                     int bits, std::string &dns_record, std::string &err);
bool AddDkimKey(duckdb::Connection &con, const std::string &domain, const std::string &selector,
                const std::string &private_key_pem, std::string &err);
bool RemoveDkimKey(duckdb::Connection &con, const std::string &domain, const std::string &selector,
                   std::string &err);

// A dkim::KeyLookup backed by quackmail_dkim_keys. Locally known keys are used
// before DNS, which lets a loopback deployment (and the test suite) verify its
// own signatures with no resolver involved.
dkim::KeyLookup DkimKeyLookup(duckdb::Connection &con);

// ---- rate limiting ------------------------------------------------------

struct RateLimit {
	std::string username; // "" is the default policy applied to everyone else
	int64_t burst_max = 100;
	int64_t burst_secs = 300;
	int64_t daily_max = 500;
	bool enabled = true;
};

struct RateVerdict {
	bool allowed = true;
	std::string reason;       // filled when denied, for the SMTP reply text
	int64_t retry_after = 0;  // seconds until the quota frees up
	int64_t burst_used = 0;
	int64_t daily_used = 0;
	RateLimit limit;
};

// Check whether `username` may send `count` more messages right now. One unit is
// charged per envelope recipient, so a message to 50 addresses costs 50.
RateVerdict CheckRate(duckdb::Connection &con, const std::string &username, int64_t count);
// Record `count` deliveries against the user's quota.
void RecordSend(duckdb::Connection &con, const std::string &username, const std::string &rcpt,
                int64_t count);
bool SetRateLimit(duckdb::Connection &con, const std::string &username, int64_t burst_max,
                  int64_t burst_secs, int64_t daily_max, std::string &err);
std::vector<RateLimit> ListRateLimits(duckdb::Connection &con);
// Drop send-log rows older than the longest window, so the table stays bounded.
void PruneSendLog(duckdb::Connection &con);

// ---- enforcement toggles ------------------------------------------------

// Read from citadel_config, so the CLI can change them with qm_config_set.
// Defaults match the shipped posture: results are always stamped, but only the
// sender's own DMARC policy and an explicit DNSBL listing cause a rejection.
struct Enforcement {
	bool spf_reject = false;   // qm_spf_reject   — reject on SPF hard fail
	bool dkim_reject = false;  // qm_dkim_reject  — reject when a signature fails
	bool dmarc_enforce = true; // qm_dmarc_enforce — honour p=reject/quarantine
	bool rbl_reject = true;    // qm_rbl_reject   — reject a DNSBL-listed client
	std::string quarantine_room = "Junk"; // qm_quarantine_room
};

Enforcement GetEnforcement(duckdb::Connection &con);

// ---- audit --------------------------------------------------------------

struct InboundVerdict {
	std::string client_ip;
	std::string helo;
	std::string mail_from;
	std::string rcpt;
	std::string spf;
	std::string dkim;
	std::string dmarc;
	std::string rbl;
	std::string disposition; // "accept" | "quarantine" | "reject"
	std::string detail;
};

void LogInbound(duckdb::Connection &con, const InboundVerdict &v);

} // namespace policy
} // namespace quackmail
