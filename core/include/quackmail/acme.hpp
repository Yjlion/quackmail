#pragma once

#include "duckdb.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace quackmail {
namespace acme {

// An ACME client (RFC 8555), http-01 only.
//
// The point is that a QuackCit install can obtain and renew its own
// certificate. Until now `deploy/quackcit.sh` could only generate a self-signed
// one, and the documentation's advice was to get a real one with some other
// tool — which is a whole second piece of software to install, configure and
// remember to renew.
//
// **http-01 only, deliberately.** dns-01 would need to publish a TXT record, and
// `dns.hpp` resolves rather than updates; offering it would mean "write this
// record by hand within the next few minutes", which is not automation.
// tls-alpn-01 needs a challenge certificate and an ALPN callback on every TLS
// listener — more OpenSSL surface than http-01 for the same result, given that
// `qm_http` is already there.
//
// **RS256 only.** `dkim::GenerateKey` is RSA and there is no ECDSA anywhere in
// this tree, so ES256 would be entirely new key handling for no interoperability
// gain: every ACME server accepts RS256.
//
// External Account Binding is out of scope. It is one extra JWS nested inside
// the newAccount payload and slots in without changing any of the shapes here.

// ---- the pure half: JOSE, JWK, CSR ---------------------------------------
//
// All of these are functions of their arguments, which is what lets
// test/sql/acme.test assert them against the known-answer vectors in RFC 7638
// §3.1 and RFC 8555 §8.1 with no network and no server.

bool GenerateAccountKey(int bits, std::string &pem, std::string &err);

// The public half as a JWK, in RFC 7638 §3 canonical form: the required members
// only, lexicographically ordered, no whitespace. That exact spelling is what
// the thumbprint is taken over, so it is not a formatting preference.
bool JwkPublic(const std::string &key_pem, std::string &jwk_json, std::string &err);

// base64url(SHA-256(canonical JWK)) — RFC 7638.
bool JwkThumbprint(const std::string &key_pem, std::string &thumbprint, std::string &err);

// RFC 8555 §8.1: the key authorization a challenge is answered with.
std::string KeyAuthorization(const std::string &token, const std::string &thumbprint);

// A flattened-JSON JWS (RFC 7515 §7.2.2), which is what ACME speaks.
// `protected_json` is the protected header; `payload` is sent as-is, and an
// empty one is POST-as-GET (RFC 8555 §6.3) rather than an empty object.
bool JwsSign(const std::string &key_pem, const std::string &protected_json,
             const std::string &payload, std::string &flattened, std::string &err);

// A PKCS#10 CSR for `dns_names`, base64url of the DER — the encoding
// `finalize` wants. Generates the certificate key if `key_pem` is empty and
// returns it there, so a fresh key per issuance is the default path.
bool MakeCsr(const std::vector<std::string> &dns_names, std::string &key_pem,
             std::string &csr_b64url, std::string &err);

// notAfter of the first certificate in a PEM chain, as unix seconds.
bool CertNotAfter(const std::string &cert_pem, int64_t &not_after, std::string &err);

// ---- configuration and state ---------------------------------------------

struct Config {
	bool enabled = false;
	std::string directory_url;
	std::string contact; // an e-mail address, or ""
	std::string ca_bundle;
	std::string cert_dir;
	int renew_days = 30;
	bool tos_agreed = false;
	int key_bits = 2048;
	int timeout_ms = 30000;
};

// Read from citadel_config. The directory defaults to Let's Encrypt **staging**
// and `enabled` to false: pointing a fresh install at production means the first
// person with a misconfigured DNS record burns a real rate limit, and those are
// counted per account per hostname per hour.
Config LoadConfig(duckdb::Connection &con);

// Create the acme tables. Called from every entry point *including the HTTP
// challenge handler*, because store::EnsureSchema runs from a table function's
// init and the responder must work on a database where the worker has never
// ticked.
void EnsureSchema(duckdb::Connection &con);

struct Result {
	std::string name;
	std::string status;
	std::string note;
	int64_t not_after = 0;
};

// One pass of the state machine: prune expired challenges, ensure an account,
// pick up certificates inside the renewal window and orders whose next attempt
// has arrived, and advance each order.
//
// `only_name` limits the pass to one certificate; `force` ignores both the
// renewal window and the backoff.
bool RunOnce(duckdb::Connection &con, const std::string &only_name, bool force,
             std::vector<Result> &out);

// Queue an order. The worker is the consumer: an order takes tens of seconds,
// which is not a web request's or a CLI call's to wait for.
bool Order(duckdb::Connection &con, const std::string &name, const std::string &domains,
           std::string &err);
bool Forget(duckdb::Connection &con, const std::string &name, std::string &err);
bool Revoke(duckdb::Connection &con, const std::string &name, int reason, std::string &err);

// Ask every listener in this database to reload its certificate, by finding the
// `<prefix>_tls_reload` functions in the catalog. The catalog is already an
// authoritative registry of what is loaded, so there is no second list to keep
// in step. **Zero matches is success**, not an error: a process that loaded only
// the spool extension has no listener to reload.
std::vector<std::pair<std::string, std::string>> ReloadListeners(duckdb::Connection &con);

} // namespace acme
} // namespace quackmail
