#include "quackmail/rbl.hpp"

#include "quackmail/dns.hpp"

namespace quackmail {
namespace rbl {

bool Check(const std::string &ip, const std::vector<std::string> &zones, Hit &out) {
	out = Hit();
	if (ip.empty() || zones.empty()) {
		return false;
	}

	for (const auto &zone : zones) {
		if (zone.empty()) {
			continue;
		}
		std::string name;
		if (!dns::ReverseName(ip, zone, name)) {
			continue; // not an address we can query for
		}

		std::vector<std::string> answers;
		auto st = dns::LookupA(name, answers);
		// NoData/NxDomain means "not listed"; TempFail means the DNSBL could not
		// be reached, which must not be read as a listing.
		if (st != dns::Status::Ok || answers.empty()) {
			continue;
		}

		out.listed = true;
		out.zone = zone;
		out.code = answers[0];

		// Most zones publish the human-readable reason as TXT at the same name.
		std::vector<std::string> txts;
		if (dns::LookupTXT(name, txts) == dns::Status::Ok && !txts.empty()) {
			out.reason = txts[0];
		}
		return true;
	}
	return false;
}

} // namespace rbl
} // namespace quackmail
