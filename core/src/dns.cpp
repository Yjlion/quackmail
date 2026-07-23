#include "quackmail/dns.hpp"

#include <algorithm>

#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>

namespace quackmail {
namespace dns {

bool LookupMX(const std::string &domain, std::vector<MxHost> &out) {
	out.clear();
	if (domain.empty()) {
		return false;
	}

	unsigned char answer[NS_PACKETSZ];
	int len = res_query(domain.c_str(), ns_c_in, ns_t_mx, answer, sizeof(answer));
	if (len < 0) {
		// No MX record (or resolver failure): fall back to the domain's own
		// address as an implicit MX, per RFC 5321.
		out.push_back(MxHost{0, domain});
		return true;
	}

	ns_msg handle;
	if (ns_initparse(answer, len, &handle) < 0) {
		out.push_back(MxHost{0, domain});
		return true;
	}

	int count = ns_msg_count(handle, ns_s_an);
	for (int i = 0; i < count; i++) {
		ns_rr rr;
		if (ns_parserr(&handle, ns_s_an, i, &rr) < 0) {
			continue;
		}
		if (ns_rr_type(rr) != ns_t_mx) {
			continue;
		}
		const unsigned char *rdata = ns_rr_rdata(rr);
		int pref = ns_get16(rdata);
		char exchange[NS_MAXDNAME];
		if (ns_name_uncompress(ns_msg_base(handle), ns_msg_end(handle), rdata + 2, exchange,
		                       sizeof(exchange)) < 0) {
			continue;
		}
		out.push_back(MxHost{pref, std::string(exchange)});
	}

	if (out.empty()) {
		out.push_back(MxHost{0, domain});
	}
	std::sort(out.begin(), out.end(),
	          [](const MxHost &a, const MxHost &b) { return a.preference < b.preference; });
	return true;
}

} // namespace dns
} // namespace quackmail
