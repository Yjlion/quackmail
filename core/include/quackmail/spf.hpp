#pragma once

#include <string>

namespace quackmail {
namespace spf {

// RFC 7208 §2.6 check results.
enum class Result {
	None,      // no SPF record published (not an assertion either way)
	Neutral,   // "?" — explicitly no assertion
	Pass,      // the client is authorised for this identity
	Fail,      // "-" — explicitly not authorised
	SoftFail,  // "~" — probably not authorised, but do not reject on it alone
	TempError, // DNS failed; the check must be retried, never treated as Fail
	PermError, // the record is unusable (syntax, loop, too many lookups)
};

struct Eval {
	Result result = Result::None;
	std::string domain;      // the identity actually checked (MAIL FROM, or HELO)
	std::string explanation; // human-readable reason, for Received-SPF's comment
	std::string record;      // the SPF record that produced the result, if any
};

// Evaluate SPF for a connection. `client_ip` is presentation-format IPv4 or
// IPv6; `helo` is the HELO/EHLO argument; `mail_from` is the envelope sender.
//
// Per §2.4 a null reverse-path (MAIL FROM:<>) is checked as postmaster@<helo>,
// so bounces are still attributable. Returns the same value as `out.result`.
Result Check(const std::string &client_ip, const std::string &helo, const std::string &mail_from, Eval &out);

// The RFC 7208 §2.6 result keyword ("pass", "fail", "softfail", ...), which is
// also the token that goes into Received-SPF and Authentication-Results.
std::string ResultName(Result r);

// Render a complete Received-SPF header value (RFC 7208 §9.1) for `eval`.
std::string ReceivedSpf(const Eval &eval, const std::string &client_ip, const std::string &helo,
                        const std::string &mail_from, const std::string &receiver);

} // namespace spf
} // namespace quackmail
