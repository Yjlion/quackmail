#include "quackmail/auth.hpp"
#include "quackmail/mail_store.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

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

std::string HashPassword(const std::string &password, const std::string &salt) {
	std::string material = salt + ":" + password;
	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char *>(material.data()), material.size(), digest);
	return ToHex(digest, SHA256_DIGEST_LENGTH);
}

bool AddUser(Connection &con, const std::string &username, const std::string &password, std::string &err) {
	store::EnsureSchema(con);
	std::string salt = GenSalt();
	std::string hash = HashPassword(password, salt);
	auto stmt = con.Prepare("INSERT OR REPLACE INTO quackmail_users "
	                        "(username, password_hash, salt, algo, enabled, created_at) "
	                        "VALUES ($1, $2, $3, 'sha256', true, now())");
	if (stmt->HasError()) {
		err = stmt->GetError();
		return false;
	}
	duckdb::vector<Value> params = {Value(username), Value(hash), Value(salt)};
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
	auto stmt = con.Prepare("SELECT password_hash, salt FROM quackmail_users "
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
	if (hash_v.IsNull() || salt_v.IsNull()) {
		return false;
	}
	std::string stored_hash = hash_v.ToString();
	std::string salt = salt_v.ToString();
	std::string computed = HashPassword(password, salt);
	if (computed.size() != stored_hash.size()) {
		return false;
	}
	return CRYPTO_memcmp(computed.data(), stored_hash.data(), computed.size()) == 0;
}

} // namespace auth
} // namespace quackmail
