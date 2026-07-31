#include "quackmail/submission.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/delivery.hpp"
#include "quackmail/dkim.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mailpolicy.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/util.hpp"

namespace quackmail {
namespace submission {

namespace {

std::string DomainOf(const std::string &addr) {
	size_t at = addr.rfind('@');
	return at == std::string::npos ? std::string() : util::Lower(addr.substr(at + 1));
}

} // namespace

Result::Result() {
}

std::string Sign(duckdb::Connection &con, const std::string &mail_from, const std::string &body) {
	// The From: header domain is what a receiver aligns against for DMARC, so it
	// is preferred over the envelope sender.
	auto parsed = mime::Parse(body);
	std::string domain;
	if (!parsed.from.empty()) {
		for (const auto &a : mime::ParseAddressList(parsed.from)) {
			if (!a.addr.empty()) {
				domain = DomainOf(a.addr);
				break;
			}
		}
	}
	if (domain.empty()) {
		domain = DomainOf(mail_from);
	}
	if (domain.empty()) {
		return body;
	}

	policy::DkimKey key;
	if (!policy::DkimKeyFor(con, domain, key) || key.private_key.empty()) {
		return body;
	}
	std::string signed_body, err;
	if (!dkim::Sign(body, key.domain.empty() ? domain : key.domain, key.selector, key.private_key,
	                key.headers, signed_body, err)) {
		return body; // signing failure must not block the mail
	}
	return signed_body;
}

std::string ReceivedHeader(duckdb::Connection &con, const std::string &via, const std::string &auth_user,
                           bool tls) {
	return "Received: from " + (via.empty() ? std::string("unknown") : via) +
	       " (submission, authenticated as " + auth_user + ")\r\n\tby " +
	       citadel::GetConfig(con, "c_fqdn", "quackmail.test") + " (QuackCit) with " +
	       (tls ? "ESMTPSA" : "ESMTPA") + ";\r\n\t" + util::RfcDate() + "\r\n";
}

bool Send(duckdb::Connection &con, const std::string &mail_from, const std::vector<std::string> &rcpts,
          const std::string &received, const std::string &body, Result &out) {
	// Stamp, then sign. Signing last puts the DKIM-Signature at the very top,
	// and means the locally delivered copy and every queued copy carry the same
	// signature rather than two that verify differently.
	std::string full = Sign(con, mail_from, received + body);

	std::vector<std::string> local;
	for (const auto &r : rcpts) {
		if (citadel::IsLocalUser(con, r)) {
			local.push_back(r);
			out.delivered.push_back(r);
		} else {
			store::EnqueueOutbound(con, mail_from, r, full);
			out.queued.push_back(r);
		}
	}
	if (local.empty()) {
		out.ok = true;
		return true;
	}
	std::string err;
	out.ok = deliver::LocalDeliver(con, mail_from, local, full, err);
	out.err = err;
	return out.ok;
}

} // namespace submission
} // namespace quackmail
