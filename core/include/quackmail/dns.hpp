#pragma once

#include <string>
#include <utility>
#include <vector>

namespace quackmail {
namespace dns {

struct MxHost {
	int preference = 0;
	std::string host;

	MxHost() = default;
	MxHost(int preference_, std::string host_) : preference(preference_), host(std::move(host_)) {}
};

// Outcome of a lookup, kept separate from "did we get records" because SPF and
// DMARC must distinguish "no such record" (a definite answer) from "the resolver
// could not tell us" (a temporary error that must not be treated as a policy
// failure).
enum class Status {
	Ok,       // at least one record of the requested type
	NoData,   // the name resolves but has no record of this type (definite)
	NxDomain, // the name does not exist (definite)
	TempFail, // resolver/network failure — retryable, never a policy answer
};

// Resolve the MX hosts for a domain, sorted by ascending preference. When the
// domain has no MX records, falls back to the domain itself as an implicit MX
// (RFC 5321 §5.1). Returns false only on a hard resolver failure with no usable
// fallback (e.g. an empty domain).
bool LookupMX(const std::string &domain, std::vector<MxHost> &out);

// The MX records as published, with no implicit-MX fallback. SPF's `mx`
// mechanism must not match a domain that publishes no MX at all, so it needs
// the unsynthesised answer.
Status LookupMXRecords(const std::string &domain, std::vector<MxHost> &out);

// TXT records for a name. Each record's length-prefixed character-strings are
// concatenated into one entry, which is what SPF (RFC 7208 §3.3), DKIM key
// records and DMARC records all expect — long keys routinely span segments.
Status LookupTXT(const std::string &name, std::vector<std::string> &out);

// A / AAAA records as presentation-format addresses ("192.0.2.1", "2001:db8::1").
Status LookupA(const std::string &name, std::vector<std::string> &out);
Status LookupAAAA(const std::string &name, std::vector<std::string> &out);

// Reverse lookup: the PTR names for a presentation-format IPv4 or IPv6 address.
Status LookupPTR(const std::string &ip, std::vector<std::string> &out);

// Build the reversed lookup name for an address under `zone` — "2.0.0.127.zone"
// for IPv4, nibble-reversed for IPv6. Used for DNSBL queries and PTR names.
// Returns false if `ip` is not a valid address.
bool ReverseName(const std::string &ip, const std::string &zone, std::string &out);

} // namespace dns
} // namespace quackmail
