#include "quackmail/sasl.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/util.hpp"

namespace quackmail {
namespace sasl {

using duckdb::Connection;

Result ServerAuth(Connection &con, const std::string &mechanism, const std::string &initial,
                  const std::function<bool(const std::string &, std::string &)> &challenge,
                  std::string &user) {
	if (mechanism == "PLAIN") {
		std::string b64 = initial;
		if (b64.empty()) {
			// Empty challenge: SMTP sends "334 ", IMAP sends "+ ".
			if (!challenge("", b64)) {
				return Result::Fail;
			}
		}
		std::string decoded;
		if (!util::Base64Decode(b64, decoded)) {
			return Result::Fail;
		}
		// authzid \0 authcid \0 passwd
		size_t p1 = decoded.find('\0');
		if (p1 == std::string::npos) {
			return Result::Fail;
		}
		size_t p2 = decoded.find('\0', p1 + 1);
		if (p2 == std::string::npos) {
			return Result::Fail;
		}
		std::string username = decoded.substr(p1 + 1, p2 - p1 - 1);
		std::string password = decoded.substr(p2 + 1);
		if (auth::Verify(con, username, password)) {
			user = username;
			return Result::Ok;
		}
		return Result::Fail;
	}

	if (mechanism == "LOGIN") {
		std::string b64, username, password;
		if (!challenge(util::Base64Encode("Username:"), b64) || !util::Base64Decode(b64, username)) {
			return Result::Fail;
		}
		if (!challenge(util::Base64Encode("Password:"), b64) || !util::Base64Decode(b64, password)) {
			return Result::Fail;
		}
		if (auth::Verify(con, username, password)) {
			user = username;
			return Result::Ok;
		}
		return Result::Fail;
	}

	return Result::Unsupported;
}

} // namespace sasl
} // namespace quackmail
