#include "quackmail/dns.hpp"

#include <algorithm>
#include <cstring>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netdb.h>
#include <resolv.h>

namespace quackmail {
namespace dns {

namespace {

// One resolver query. `answer` receives the raw DNS response; `len` its size.
// h_errno distinguishes "no such name" and "no record of this type" from a real
// resolver failure, which SPF/DMARC must not conflate (see Status).
Status Query(const std::string &name, int type, std::vector<unsigned char> &answer, int &len) {
	answer.resize(NS_MAXMSG);
	len = res_query(name.c_str(), ns_c_in, type, answer.data(), (int)answer.size());
	if (len >= 0) {
		return Status::Ok;
	}
	switch (h_errno) {
	case HOST_NOT_FOUND:
		return Status::NxDomain;
	case NO_DATA:
		return Status::NoData;
	default:
		// TRY_AGAIN / NO_RECOVERY and anything else: no definite answer.
		return Status::TempFail;
	}
}

// Iterate the answer section, invoking `visit` for each record of `type` with
// its rdata pointer and length.
template <typename Fn>
Status ForEachRecord(const std::string &name, int type, const Fn &visit) {
	std::vector<unsigned char> answer;
	int len = 0;
	Status st = Query(name, type, answer, len);
	if (st != Status::Ok) {
		return st;
	}

	ns_msg handle;
	if (ns_initparse(answer.data(), len, &handle) < 0) {
		return Status::TempFail;
	}

	bool any = false;
	int count = ns_msg_count(handle, ns_s_an);
	for (int i = 0; i < count; i++) {
		ns_rr rr;
		if (ns_parserr(&handle, ns_s_an, i, &rr) < 0) {
			continue;
		}
		if (ns_rr_type(rr) != type) {
			continue; // CNAMEs in the chain, etc.
		}
		if (visit(handle, rr)) {
			any = true;
		}
	}
	return any ? Status::Ok : Status::NoData;
}

} // namespace

Status LookupMXRecords(const std::string &domain, std::vector<MxHost> &out) {
	out.clear();
	if (domain.empty()) {
		return Status::NxDomain;
	}
	Status st = ForEachRecord(domain, ns_t_mx, [&](const ns_msg &handle, const ns_rr &rr) {
		const unsigned char *rdata = ns_rr_rdata(rr);
		int pref = ns_get16(rdata);
		char exchange[NS_MAXDNAME];
		if (ns_name_uncompress(ns_msg_base(handle), ns_msg_end(handle), rdata + 2, exchange,
		                       sizeof(exchange)) < 0) {
			return false;
		}
		out.push_back(MxHost{pref, std::string(exchange)});
		return true;
	});
	std::sort(out.begin(), out.end(),
	          [](const MxHost &a, const MxHost &b) { return a.preference < b.preference; });
	return st;
}

bool LookupMX(const std::string &domain, std::vector<MxHost> &out) {
	if (domain.empty()) {
		out.clear();
		return false;
	}
	LookupMXRecords(domain, out);
	if (out.empty()) {
		// No MX record (or resolver failure): fall back to the domain's own
		// address as an implicit MX, per RFC 5321.
		out.push_back(MxHost{0, domain});
	}
	return true; // LookupMXRecords already sorted by preference
}

Status LookupTXT(const std::string &name, std::vector<std::string> &out) {
	out.clear();
	return ForEachRecord(name, ns_t_txt, [&](const ns_msg &, const ns_rr &rr) {
		// TXT rdata is a sequence of length-prefixed strings; a record longer
		// than 255 bytes (any real RSA DKIM key) arrives split, and the pieces
		// are concatenated with no separator.
		const unsigned char *p = ns_rr_rdata(rr);
		const unsigned char *end = p + ns_rr_rdlen(rr);
		std::string joined;
		while (p < end) {
			size_t seg = *p++;
			if (p + seg > end) {
				break;
			}
			joined.append(reinterpret_cast<const char *>(p), seg);
			p += seg;
		}
		if (joined.empty()) {
			return false;
		}
		out.push_back(std::move(joined));
		return true;
	});
}

Status LookupA(const std::string &name, std::vector<std::string> &out) {
	out.clear();
	return ForEachRecord(name, ns_t_a, [&](const ns_msg &, const ns_rr &rr) {
		if (ns_rr_rdlen(rr) != 4) {
			return false;
		}
		char buf[INET_ADDRSTRLEN];
		if (!inet_ntop(AF_INET, ns_rr_rdata(rr), buf, sizeof(buf))) {
			return false;
		}
		out.push_back(std::string(buf));
		return true;
	});
}

Status LookupAAAA(const std::string &name, std::vector<std::string> &out) {
	out.clear();
	return ForEachRecord(name, ns_t_aaaa, [&](const ns_msg &, const ns_rr &rr) {
		if (ns_rr_rdlen(rr) != 16) {
			return false;
		}
		char buf[INET6_ADDRSTRLEN];
		if (!inet_ntop(AF_INET6, ns_rr_rdata(rr), buf, sizeof(buf))) {
			return false;
		}
		out.push_back(std::string(buf));
		return true;
	});
}

Status LookupPTR(const std::string &ip, std::vector<std::string> &out) {
	out.clear();
	// IPv4 and IPv6 reverse names live under different suffixes, so pick the
	// zone from the address family rather than letting ReverseName guess.
	struct in6_addr probe;
	bool is_v6 = inet_pton(AF_INET6, ip.c_str(), &probe) == 1;
	std::string name;
	if (!ReverseName(ip, is_v6 ? "ip6.arpa" : "in-addr.arpa", name)) {
		return Status::NxDomain;
	}
	return ForEachRecord(name, ns_t_ptr, [&](const ns_msg &handle, const ns_rr &rr) {
		char target[NS_MAXDNAME];
		if (ns_name_uncompress(ns_msg_base(handle), ns_msg_end(handle), ns_rr_rdata(rr), target,
		                       sizeof(target)) < 0) {
			return false;
		}
		out.push_back(std::string(target));
		return true;
	});
}

bool ReverseName(const std::string &ip, const std::string &zone, std::string &out) {
	static const char kHex[] = "0123456789abcdef";

	struct in_addr v4;
	if (inet_pton(AF_INET, ip.c_str(), &v4) == 1) {
		// 192.0.2.1 -> 1.2.0.192.<zone>
		const unsigned char *b = reinterpret_cast<const unsigned char *>(&v4.s_addr);
		out.clear();
		for (int i = 3; i >= 0; i--) {
			out += std::to_string((int)b[i]);
			out += '.';
		}
		out += zone;
		return true;
	}

	struct in6_addr v6;
	if (inet_pton(AF_INET6, ip.c_str(), &v6) == 1) {
		// Each byte becomes two nibbles, lowest-order nibble first.
		out.clear();
		for (int i = 15; i >= 0; i--) {
			unsigned char byte = v6.s6_addr[i];
			out += kHex[byte & 0x0f];
			out += '.';
			out += kHex[(byte >> 4) & 0x0f];
			out += '.';
		}
		out += zone;
		return true;
	}

	return false;
}

} // namespace dns
} // namespace quackmail
