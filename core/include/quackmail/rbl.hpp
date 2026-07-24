#pragma once

#include <string>
#include <vector>

namespace quackmail {
namespace rbl {

struct Hit {
	bool listed = false;
	std::string zone;   // the DNSBL that listed the address
	std::string code;   // the 127.0.0.x answer, which encodes why
	std::string reason; // the zone's TXT explanation, when it publishes one
};

// Query each zone for `ip` (reversed-nibble name under the zone) and return the
// first listing. Zones are tried in order, so put the most trusted first.
// A resolver failure is treated as "not listed" — a DNSBL outage must never
// start rejecting mail.
bool Check(const std::string &ip, const std::vector<std::string> &zones, Hit &out);

} // namespace rbl
} // namespace quackmail
