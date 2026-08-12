#include "quackmail/auth.hpp"
#include "quackmail/mail_store.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cstdlib>
#include <cstdint>

namespace quackmail {
namespace auth {

using duckdb::Connection;
using duckdb::MaterializedQueryResult;
using duckdb::Value;

static std::string ToHex(const unsigned char *data, size_t len) {
	static const char *hex = "0123456789abcdef";
	std::string out;
	out.reserve(len * 2);
	for (size_t i = 0; i < len; i++) {
		out += hex[data[i] >> 4];
		out += hex[data[i] & 0xF];
	}
	return out;
}

std::string GenSalt() {
	unsigned char buf[16];
	if (RAND_bytes(buf, sizeof(buf)) != 1) {
		// Fall back to a fixed-length zero salt only if the RNG fails; callers
		// still get a usable (if weaker) value.
		for (auto &b : buf) {
			b = 0;
		}
	}
	return ToHex(buf, sizeof(buf));
}

// The current work factor. N is the CPU/memory cost and the only one worth
// tuning: memory used is roughly 128 * N * r bytes, so 16384 * 8 is 16 MiB, and
// a verify measures ~80 ms on a modest 2026 machine. That is the whole point —
// it is the same 16 MiB per guess for an attacker, which is what a GPU cannot
// parallelize its way out of.
//
// 80 ms is also the cost of every IMAP/POP3/SMTP AUTH, which for a protocol
// that reconnects often is not nothing. It is bounded by the login throttle
// the web front-end already applies and by the connection cap; if it ever
// becomes the bottleneck, the answer is a session or token cache, not a weaker
// KDF.
//
// Raising these is safe: `algo` records what each row was hashed at, and a row
// at an older factor is rewritten on its owner's next sign-in.
constexpr uint64_t kScryptN = 16384;
constexpr uint64_t kScryptR = 8;
constexpr uint64_t kScryptP = 1;
constexpr size_t kScryptLen = 32;

Stored::Stored() {
}

std::string CurrentAlgo() {
	return "scrypt$" + std::to_string(kScryptN) + "$" + std::to_string(kScryptR) + "$" +
	       std::to_string(kScryptP);
}

// Parse "scrypt$N$r$p". False for anything else, including the legacy "sha256"
// — a caller that cannot read the algo must not guess at one.
bool ParseScrypt(const std::string &algo, uint64_t &n, uint64_t &r, uint64_t &p) {
	if (algo.rfind("scrypt$", 0) != 0) {
		return false;
	}
	uint64_t *out[3] = {&n, &r, &p};
	size_t pos = 7;
	for (int i = 0; i < 3; i++) {
		size_t next = algo.find('$', pos);
		std::string tok = algo.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
		if (tok.empty() || tok.find_first_not_of("0123456789") != std::string::npos) {
			return false;
		}
		*out[i] = std::strtoull(tok.c_str(), nullptr, 10);
		if (*out[i] == 0) {
			return false;
		}
		if (i < 2 && next == std::string::npos) {
			return false;
		}
		pos = next == std::string::npos ? algo.size() : next + 1;
	}
	// A refusal rather than a clamp. These come out of the database, and a row
	// naming an absurd N would otherwise let one login allocate the machine.
	return n <= (1u << 22) && r <= 64 && p <= 16;
}

std::string ScryptHex(const std::string &password, const std::string &salt, uint64_t n, uint64_t r,
                      uint64_t p) {
	unsigned char out[kScryptLen];
	// The memory limit has to be given explicitly or OpenSSL refuses anything
	// over 32 MiB by default, which is below where the work factor wants to go.
	uint64_t maxmem = 128 * n * r * 2 + (1u << 20);
	if (EVP_PBE_scrypt(password.data(), password.size(),
	                   reinterpret_cast<const unsigned char *>(salt.data()), salt.size(), n, r, p,
	                   maxmem, out, sizeof(out)) != 1) {
		return std::string();
	}
	return ToHex(out, sizeof(out));
}

// The scheme this replaced: one round of salted SHA-256. Kept only so an
// existing database still lets its users in — never used to store anything new.
std::string LegacySha256(const std::string &password, const std::string &salt) {
	std::string material = salt + ":" + password;
	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char *>(material.data()), material.size(), digest);
	return ToHex(digest, SHA256_DIGEST_LENGTH);
}

Stored HashPassword(const std::string &password) {
	Stored out;
	out.algo = CurrentAlgo();
	out.salt = GenSalt();
	out.hash = ScryptHex(password, out.salt, kScryptN, kScryptR, kScryptP);
	return out;
}

bool CheckPassword(const std::string &password, const Stored &stored) {
	std::string computed;
	uint64_t n = 0, r = 0, p = 0;
	if (ParseScrypt(stored.algo, n, r, p)) {
		computed = ScryptHex(password, stored.salt, n, r, p);
	} else if (stored.algo.empty() || stored.algo == "sha256") {
		computed = LegacySha256(password, stored.salt);
	} else {
		return false; // an algo we do not implement is not a password we can check
	}
	if (computed.empty() || computed.size() != stored.hash.size()) {
		return false;
	}
	return CRYPTO_memcmp(computed.data(), stored.hash.data(), computed.size()) == 0;
}

bool NeedsRehash(const std::string &algo) {
	uint64_t n = 0, r = 0, p = 0;
	if (!ParseScrypt(algo, n, r, p)) {
		return true; // legacy, or something we could not read
	}
	return n < kScryptN || r < kScryptR || p < kScryptP;
}

bool AddUser(Connection &con, const std::string &username, const std::string &password, std::string &err) {
	store::EnsureSchema(con);
	Stored s = HashPassword(password);
	if (s.hash.empty()) {
		err = "the password could not be hashed";
		return false;
	}
	auto stmt = con.Prepare("INSERT OR REPLACE INTO quackmail_users "
	                        "(username, password_hash, salt, algo, enabled, created_at) "
	                        "VALUES ($1, $2, $3, $4, true, now())");
	if (stmt->HasError()) {
		err = stmt->GetError();
		return false;
	}
	duckdb::vector<Value> params = {Value(username), Value(s.hash), Value(s.salt), Value(s.algo)};
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		err = r->GetError();
		return false;
	}
	return true;
}

bool RemoveUser(Connection &con, const std::string &username, std::string &err) {
	store::EnsureSchema(con);
	auto stmt = con.Prepare("DELETE FROM quackmail_users WHERE username = $1");
	if (stmt->HasError()) {
		err = stmt->GetError();
		return false;
	}
	duckdb::vector<Value> params = {Value(username)};
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		err = r->GetError();
		return false;
	}
	return true;
}

bool Verify(Connection &con, const std::string &username, const std::string &password) {
	auto stmt = con.Prepare("SELECT password_hash, salt, algo FROM quackmail_users "
	                        "WHERE username = $1 AND enabled = true");
	if (stmt->HasError()) {
		return false;
	}
	duckdb::vector<Value> params = {Value(username)};
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() != 1) {
		return false;
	}
	Value hash_v = mat.GetValue(0, 0);
	Value salt_v = mat.GetValue(1, 0);
	Value algo_v = mat.GetValue(2, 0);
	if (hash_v.IsNull() || salt_v.IsNull()) {
		return false;
	}
	Stored stored;
	stored.hash = hash_v.ToString();
	stored.salt = salt_v.ToString();
	stored.algo = algo_v.IsNull() ? std::string("sha256") : algo_v.ToString();

	if (!CheckPassword(password, stored)) {
		return false;
	}

	// Upgrade in place, now that the password is known to be right and is in
	// hand. Without this an existing install never benefits: every account
	// created before the change keeps its one-round SHA-256 forever, and the
	// improvement only ever reaches people who happen to set a new password.
	//
	// Failure here is deliberately ignored. The sign-in already succeeded, and
	// refusing it because a housekeeping UPDATE did not land would turn a
	// hardening measure into an outage.
	if (NeedsRehash(stored.algo)) {
		Stored fresh = HashPassword(password);
		if (!fresh.hash.empty()) {
			auto up = con.Prepare("UPDATE quackmail_users SET password_hash = $2, salt = $3, "
			                      "algo = $4 WHERE username = $1");
			if (!up->HasError()) {
				duckdb::vector<Value> p = {Value(username), Value(fresh.hash), Value(fresh.salt),
				                           Value(fresh.algo)};
				up->Execute(p, false);
			}
		}
	}
	return true;
}

} // namespace auth
} // namespace quackmail
