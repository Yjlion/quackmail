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

// Resolve the MX hosts for a domain, sorted by ascending preference. When the
// domain has no MX records, falls back to the domain itself as an implicit MX
// (RFC 5321 §5.1). Returns false only on a hard resolver failure with no usable
// fallback (e.g. an empty domain).
bool LookupMX(const std::string &domain, std::vector<MxHost> &out);

} // namespace dns
} // namespace quackmail
