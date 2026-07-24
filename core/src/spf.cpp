#include "quackmail/spf.hpp"

#include "quackmail/dns.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace quackmail {
namespace spf {

namespace {

// RFC 7208 §4.6.4: at most 10 terms that cause a DNS lookup may be evaluated,
// and "void" lookups (NXDOMAIN/no-data) are separately capped at 2.
constexpr int kMaxLookups = 10;
constexpr int kMaxVoidLookups = 2;
// §4.6.4 also caps mx and ptr expansion at 10 records each.
constexpr size_t kMaxMxRecords = 10;
constexpr size_t kMaxPtrRecords = 10;
// Guard against an include/redirect chain that grows deep without repeating.
constexpr int kMaxDepth = 10;

std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

bool IEquals(const std::string &a, const std::string &b) {
	return a.size() == b.size() && Lower(a) == Lower(b);
}

// True when `s` starts with `prefix`, case-insensitively.
bool StartsWithI(const std::string &s, const std::string &prefix) {
	return s.size() >= prefix.size() && IEquals(s.substr(0, prefix.size()), prefix);
}

std::string DomainOf(const std::string &addr) {
	auto at = addr.rfind('@');
	return at == std::string::npos ? "" : addr.substr(at + 1);
}

std::string LocalOf(const std::string &addr) {
	auto at = addr.rfind('@');
	return at == std::string::npos ? addr : addr.substr(0, at);
}

// ---------------------------------------------------------------------------
// Address handling
// ---------------------------------------------------------------------------

struct Ip {
	bool valid = false;
	bool v6 = false;
	unsigned char bytes[16] = {0};
};

Ip ParseIp(const std::string &s) {
	Ip ip;
	struct in_addr v4;
	if (inet_pton(AF_INET, s.c_str(), &v4) == 1) {
		ip.valid = true;
		ip.v6 = false;
		std::memcpy(ip.bytes, &v4.s_addr, 4);
		return ip;
	}
	struct in6_addr v6;
	if (inet_pton(AF_INET6, s.c_str(), &v6) == 1) {
		ip.valid = true;
		ip.v6 = true;
		std::memcpy(ip.bytes, v6.s6_addr, 16);
		return ip;
	}
	return ip;
}

// Compare the first `prefix_bits` bits of two same-family addresses.
bool PrefixMatch(const Ip &a, const Ip &b, int prefix_bits) {
	if (!a.valid || !b.valid || a.v6 != b.v6) {
		return false;
	}
	int total_bits = a.v6 ? 128 : 32;
	if (prefix_bits < 0 || prefix_bits > total_bits) {
		prefix_bits = total_bits;
	}
	int whole = prefix_bits / 8;
	int rest = prefix_bits % 8;
	if (whole > 0 && std::memcmp(a.bytes, b.bytes, (size_t)whole) != 0) {
		return false;
	}
	if (rest == 0) {
		return true;
	}
	unsigned char mask = (unsigned char)(0xff << (8 - rest));
	return (a.bytes[whole] & mask) == (b.bytes[whole] & mask);
}

// Dotted-nibble form of an IPv6 address, and dotted-quad of an IPv4 — the "%{i}"
// macro expansion (RFC 7208 §7.3).
std::string MacroIp(const Ip &ip) {
	static const char kHex[] = "0123456789abcdef";
	if (!ip.valid) {
		return "";
	}
	std::string out;
	if (!ip.v6) {
		for (int i = 0; i < 4; i++) {
			if (i) {
				out += '.';
			}
			out += std::to_string((int)ip.bytes[i]);
		}
		return out;
	}
	for (int i = 0; i < 16; i++) {
		if (i) {
			out += '.';
		}
		out += kHex[(ip.bytes[i] >> 4) & 0x0f];
		out += '.';
		out += kHex[ip.bytes[i] & 0x0f];
	}
	return out;
}

// ---------------------------------------------------------------------------
// Macro expansion (RFC 7208 §7)
// ---------------------------------------------------------------------------

struct MacroCtx {
	Ip ip;
	std::string sender;   // full "local@domain" of the checked identity
	std::string domain;   // <domain> currently being evaluated
	std::string helo;
	std::string client_ip_text;
};

// Split on any of `delims` (default "."), then apply the reverse/count
// transformers described in §7.1.
std::string Transform(const std::string &value, int digits, bool reverse, const std::string &delims) {
	std::string d = delims.empty() ? "." : delims;
	std::vector<std::string> parts;
	std::string cur;
	for (char c : value) {
		if (d.find(c) != std::string::npos) {
			parts.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	parts.push_back(cur);

	if (reverse) {
		std::reverse(parts.begin(), parts.end());
	}
	if (digits > 0 && (size_t)digits < parts.size()) {
		parts.erase(parts.begin(), parts.end() - digits);
	}
	std::string out;
	for (size_t i = 0; i < parts.size(); i++) {
		if (i) {
			out += '.';
		}
		out += parts[i];
	}
	return out;
}

// URL-escape for the "%{s}"-style macros that appear inside exists: domains.
std::string UrlEscape(const std::string &in) {
	static const char kHex[] = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : in) {
		bool unreserved = std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
		if (unreserved) {
			out += (char)c;
		} else {
			out += '%';
			out += kHex[(c >> 4) & 0x0f];
			out += kHex[c & 0x0f];
		}
	}
	return out;
}

std::string ExpandMacros(const std::string &in, const MacroCtx &ctx) {
	std::string out;
	for (size_t i = 0; i < in.size(); i++) {
		if (in[i] != '%') {
			out += in[i];
			continue;
		}
		if (i + 1 >= in.size()) {
			out += '%';
			break;
		}
		char next = in[++i];
		if (next == '%') {
			out += '%';
			continue;
		}
		if (next == '_') {
			out += ' ';
			continue;
		}
		if (next == '-') {
			out += "%20";
			continue;
		}
		if (next != '{') {
			// Not a macro after all; emit it literally.
			out += '%';
			out += next;
			continue;
		}
		auto close = in.find('}', i);
		if (close == std::string::npos) {
			out += "%{";
			continue;
		}
		std::string body = in.substr(i + 1, close - i - 1);
		i = close;
		if (body.empty()) {
			continue;
		}

		char letter = (char)std::tolower((unsigned char)body[0]);
		bool url_escape = std::isupper((unsigned char)body[0]) != 0;
		std::string rest = body.substr(1);

		int digits = 0;
		size_t p = 0;
		while (p < rest.size() && std::isdigit((unsigned char)rest[p])) {
			digits = digits * 10 + (rest[p] - '0');
			p++;
		}
		bool reverse = false;
		if (p < rest.size() && (rest[p] == 'r' || rest[p] == 'R')) {
			reverse = true;
			p++;
		}
		std::string delims = rest.substr(p);

		std::string value;
		switch (letter) {
		case 's':
			value = ctx.sender;
			break;
		case 'l':
			value = LocalOf(ctx.sender);
			break;
		case 'o':
			value = DomainOf(ctx.sender);
			break;
		case 'd':
			value = ctx.domain;
			break;
		case 'i':
			value = MacroIp(ctx.ip);
			break;
		case 'h':
			value = ctx.helo;
			break;
		case 'v':
			value = ctx.ip.v6 ? "ip6" : "in-addr";
			break;
		case 'c':
			value = ctx.client_ip_text;
			break;
		case 'r':
			value = "quackcit";
			break;
		default:
			value = "";
			break;
		}

		value = Transform(value, digits, reverse, delims);
		out += url_escape ? UrlEscape(value) : value;
	}
	return out;
}

// ---------------------------------------------------------------------------
// Record evaluation
// ---------------------------------------------------------------------------

struct Limits {
	int lookups = 0;
	int void_lookups = 0;
	bool exceeded = false;

	// Charge one DNS-consuming term. Returns false once the budget is spent.
	bool Charge() {
		if (++lookups > kMaxLookups) {
			exceeded = true;
			return false;
		}
		return true;
	}
	// Record a lookup that produced no answer; too many is a PermError.
	bool Void() {
		if (++void_lookups > kMaxVoidLookups) {
			exceeded = true;
			return false;
		}
		return true;
	}
};

// Pull the single "v=spf1" record for a domain. `status` distinguishes
// none/temperror/permerror for the caller.
Result FetchRecord(const std::string &domain, std::string &record) {
	record.clear();
	std::vector<std::string> txts;
	auto st = dns::LookupTXT(domain, txts);
	if (st == dns::Status::TempFail) {
		return Result::TempError;
	}
	if (st != dns::Status::Ok) {
		return Result::None;
	}
	int found = 0;
	for (auto &t : txts) {
		// The version token is matched case-insensitively but must be followed
		// by a space or end-of-record, so "v=spf10" is not an SPF record.
		if (!StartsWithI(t, "v=spf1")) {
			continue;
		}
		if (t.size() > 6 && t[6] != ' ' && t[6] != '\t') {
			continue;
		}
		found++;
		record = t;
	}
	if (found == 0) {
		return Result::None;
	}
	if (found > 1) {
		record.clear();
		return Result::PermError; // §4.5: more than one record is unusable
	}
	return Result::Pass; // "record found" — the caller evaluates it
}

Result QualifierToResult(char q) {
	switch (q) {
	case '-':
		return Result::Fail;
	case '~':
		return Result::SoftFail;
	case '?':
		return Result::Neutral;
	default:
		return Result::Pass;
	}
}

// Split "name/cidr4//cidr6" into its parts. Missing values stay -1.
void SplitCidr(const std::string &in, std::string &name, int &cidr4, int &cidr6) {
	name = in;
	cidr4 = -1;
	cidr6 = -1;
	auto dbl = name.find("//");
	if (dbl != std::string::npos) {
		cidr6 = std::atoi(name.c_str() + dbl + 2);
		name = name.substr(0, dbl);
	}
	auto slash = name.find('/');
	if (slash != std::string::npos) {
		cidr4 = std::atoi(name.c_str() + slash + 1);
		name = name.substr(0, slash);
	}
}

Result Evaluate(const std::string &domain, const MacroCtx &ctx, Limits &limits, int depth,
                std::string &matched_record, std::string &explanation);

// Resolve `name`'s A/AAAA records (family chosen by the client address) and test
// them against the client IP with the given prefix length.
bool MatchHost(const std::string &name, const MacroCtx &ctx, int cidr, Limits &limits, bool &temp_error) {
	std::vector<std::string> addrs;
	auto st = ctx.ip.v6 ? dns::LookupAAAA(name, addrs) : dns::LookupA(name, addrs);
	if (st == dns::Status::TempFail) {
		temp_error = true;
		return false;
	}
	if (st != dns::Status::Ok) {
		limits.Void();
		return false;
	}
	for (auto &a : addrs) {
		if (PrefixMatch(ctx.ip, ParseIp(a), cidr)) {
			return true;
		}
	}
	return false;
}

Result Evaluate(const std::string &domain, const MacroCtx &ctx_in, Limits &limits, int depth,
                std::string &matched_record, std::string &explanation) {
	if (depth > kMaxDepth) {
		explanation = "include/redirect nesting too deep";
		return Result::PermError;
	}
	if (domain.empty()) {
		explanation = "empty domain";
		return Result::None;
	}

	std::string record;
	Result fetch = FetchRecord(domain, record);
	if (fetch == Result::TempError) {
		explanation = "DNS temporary failure for " + domain;
		return Result::TempError;
	}
	if (fetch == Result::PermError) {
		explanation = "multiple SPF records for " + domain;
		return Result::PermError;
	}
	if (fetch == Result::None) {
		explanation = "no SPF record for " + domain;
		return Result::None;
	}
	if (matched_record.empty()) {
		matched_record = record;
	}

	MacroCtx ctx = ctx_in;
	ctx.domain = domain;

	// Tokenize on whitespace, skipping the leading "v=spf1".
	std::vector<std::string> terms;
	{
		std::string cur;
		for (char c : record) {
			if (c == ' ' || c == '\t') {
				if (!cur.empty()) {
					terms.push_back(cur);
					cur.clear();
				}
			} else {
				cur += c;
			}
		}
		if (!cur.empty()) {
			terms.push_back(cur);
		}
	}

	std::string redirect;
	for (size_t ti = 1; ti < terms.size(); ti++) {
		const std::string &raw = terms[ti];

		// Modifiers are "name=value" with no qualifier.
		if (StartsWithI(raw, "redirect=")) {
			redirect = ExpandMacros(raw.substr(9), ctx);
			continue;
		}
		if (StartsWithI(raw, "exp=")) {
			continue; // parsed and ignored; we generate our own explanation
		}

		char qualifier = '+';
		std::string term = raw;
		if (!term.empty() && (term[0] == '+' || term[0] == '-' || term[0] == '~' || term[0] == '?')) {
			qualifier = term[0];
			term = term.substr(1);
		}
		if (term.empty()) {
			continue;
		}

		// Split "mech:value" / "mech/cidr".
		std::string mech = term;
		std::string arg;
		auto colon = term.find(':');
		auto slash = term.find('/');
		if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) {
			mech = term.substr(0, colon);
			arg = term.substr(colon + 1);
		} else if (slash != std::string::npos) {
			mech = term.substr(0, slash);
			arg = term.substr(slash); // keep the leading '/' for SplitCidr
		}
		mech = Lower(mech);

		if (mech == "all") {
			explanation = "matched 'all' in " + domain + "'s SPF record";
			return QualifierToResult(qualifier);
		}

		if (mech == "ip4" || mech == "ip6") {
			std::string name;
			int c4 = -1, c6 = -1;
			SplitCidr(arg, name, c4, c6);
			Ip target = ParseIp(name);
			if (!target.valid) {
				explanation = "malformed " + mech + " term in " + domain;
				return Result::PermError;
			}
			// ip4/ip6 carry a single prefix length for their own family, which
			// SplitCidr always returns in cidr4.
			(void)c6;
			if (PrefixMatch(ctx.ip, target, c4)) {
				explanation = "client matched " + raw + " in " + domain;
				return QualifierToResult(qualifier);
			}
			continue;
		}

		if (mech == "a" || mech == "mx") {
			if (!limits.Charge()) {
				explanation = "too many DNS lookups evaluating " + domain;
				return Result::PermError;
			}
			std::string name;
			int c4 = -1, c6 = -1;
			// "a" / "a/24" / "a:host" / "a:host/24"
			SplitCidr(arg.empty() ? "" : (arg[0] == '/' ? domain + arg : arg), name, c4, c6);
			if (name.empty()) {
				name = domain;
			}
			name = ExpandMacros(name, ctx);
			int cidr = ctx.ip.v6 ? c6 : c4;

			bool temp = false;
			bool hit = false;
			if (mech == "a") {
				hit = MatchHost(name, ctx, cidr, limits, temp);
			} else {
				// The strict lookup: a domain that publishes no MX must not
				// match `mx` via LookupMX's implicit-MX fallback.
				std::vector<dns::MxHost> mx;
				auto mst = dns::LookupMXRecords(name, mx);
				if (mst == dns::Status::TempFail) {
					temp = true;
				} else if (mst != dns::Status::Ok) {
					limits.Void();
				} else {
					size_t n = std::min(mx.size(), kMaxMxRecords);
					for (size_t i = 0; i < n && !hit; i++) {
						hit = MatchHost(mx[i].host, ctx, cidr, limits, temp);
					}
				}
			}
			if (temp) {
				explanation = "DNS temporary failure resolving " + name;
				return Result::TempError;
			}
			if (limits.exceeded) {
				explanation = "too many void DNS lookups evaluating " + domain;
				return Result::PermError;
			}
			if (hit) {
				explanation = "client matched " + raw + " in " + domain;
				return QualifierToResult(qualifier);
			}
			continue;
		}

		if (mech == "exists") {
			if (!limits.Charge()) {
				explanation = "too many DNS lookups evaluating " + domain;
				return Result::PermError;
			}
			std::string name = ExpandMacros(arg, ctx);
			std::vector<std::string> addrs;
			auto st = dns::LookupA(name, addrs);
			if (st == dns::Status::TempFail) {
				explanation = "DNS temporary failure for exists:" + name;
				return Result::TempError;
			}
			if (st == dns::Status::Ok && !addrs.empty()) {
				explanation = "client matched " + raw + " in " + domain;
				return QualifierToResult(qualifier);
			}
			limits.Void();
			if (limits.exceeded) {
				explanation = "too many void DNS lookups evaluating " + domain;
				return Result::PermError;
			}
			continue;
		}

		if (mech == "include") {
			if (!limits.Charge()) {
				explanation = "too many DNS lookups evaluating " + domain;
				return Result::PermError;
			}
			std::string target = ExpandMacros(arg, ctx);
			std::string sub_record, sub_expl;
			Result r = Evaluate(target, ctx, limits, depth + 1, sub_record, sub_expl);
			// §5.2: include only matches on Pass; None/PermError from the
			// included record are a PermError here.
			if (r == Result::Pass) {
				explanation = "client matched include:" + target;
				return QualifierToResult(qualifier);
			}
			if (r == Result::TempError) {
				explanation = sub_expl;
				return Result::TempError;
			}
			if (r == Result::None || r == Result::PermError) {
				explanation = "include:" + target + " is unusable (" + sub_expl + ")";
				return Result::PermError;
			}
			continue; // Fail / SoftFail / Neutral: no match, keep going
		}

		if (mech == "ptr") {
			if (!limits.Charge()) {
				explanation = "too many DNS lookups evaluating " + domain;
				return Result::PermError;
			}
			std::string target = arg.empty() ? domain : ExpandMacros(arg, ctx);
			std::vector<std::string> names;
			auto st = dns::LookupPTR(ctx.client_ip_text, names);
			if (st == dns::Status::TempFail) {
				explanation = "DNS temporary failure resolving PTR";
				return Result::TempError;
			}
			bool hit = false;
			size_t n = std::min(names.size(), kMaxPtrRecords);
			for (size_t i = 0; i < n && !hit; i++) {
				std::string candidate = names[i];
				if (!candidate.empty() && candidate.back() == '.') {
					candidate.pop_back();
				}
				// The PTR name must both resolve back to the client address and
				// be (a subdomain of) the target — the validated-hostname rule.
				bool temp = false;
				if (!MatchHost(candidate, ctx, -1, limits, temp)) {
					continue;
				}
				std::string lc = Lower(candidate), lt = Lower(target);
				if (lc == lt || (lc.size() > lt.size() && lc.compare(lc.size() - lt.size() - 1,
				                                                     lt.size() + 1, "." + lt) == 0)) {
					hit = true;
				}
			}
			if (hit) {
				explanation = "client matched ptr in " + domain;
				return QualifierToResult(qualifier);
			}
			continue;
		}

		// An unrecognised mechanism makes the whole record unusable (§4.6.1).
		if (raw.find('=') == std::string::npos) {
			explanation = "unknown mechanism '" + mech + "' in " + domain;
			return Result::PermError;
		}
		// Unknown modifiers are ignored by design.
	}

	if (!redirect.empty()) {
		if (!limits.Charge()) {
			explanation = "too many DNS lookups evaluating " + domain;
			return Result::PermError;
		}
		std::string sub_record, sub_expl;
		Result r = Evaluate(redirect, ctx, limits, depth + 1, sub_record, sub_expl);
		// §6.1: a redirect to a domain with no record is a PermError.
		if (r == Result::None) {
			explanation = "redirect=" + redirect + " has no SPF record";
			return Result::PermError;
		}
		explanation = sub_expl;
		if (!sub_record.empty()) {
			matched_record = sub_record;
		}
		return r;
	}

	// A record with no matching mechanism and no redirect defaults to neutral.
	explanation = "no mechanism matched in " + domain + "'s SPF record";
	return Result::Neutral;
}

} // namespace

Result Check(const std::string &client_ip, const std::string &helo, const std::string &mail_from,
             Eval &out) {
	out = Eval();

	MacroCtx ctx;
	ctx.ip = ParseIp(client_ip);
	ctx.client_ip_text = client_ip;
	ctx.helo = helo;

	if (!ctx.ip.valid) {
		out.result = Result::None;
		out.explanation = "no usable client IP address";
		return out.result;
	}

	// §2.4: an empty reverse-path is checked as postmaster@<helo>.
	std::string identity = mail_from;
	if (identity.empty() || identity == "<>") {
		identity = "postmaster@" + helo;
	}
	if (identity.find('@') == std::string::npos) {
		identity = "postmaster@" + identity;
	}
	std::string domain = DomainOf(identity);
	if (domain.empty()) {
		out.result = Result::None;
		out.explanation = "no domain in the sender identity";
		return out.result;
	}

	ctx.sender = identity;
	ctx.domain = domain;
	out.domain = domain;

	Limits limits;
	out.result = Evaluate(domain, ctx, limits, 0, out.record, out.explanation);
	return out.result;
}

std::string ResultName(Result r) {
	switch (r) {
	case Result::None:
		return "none";
	case Result::Neutral:
		return "neutral";
	case Result::Pass:
		return "pass";
	case Result::Fail:
		return "fail";
	case Result::SoftFail:
		return "softfail";
	case Result::TempError:
		return "temperror";
	case Result::PermError:
		return "permerror";
	}
	return "none";
}

std::string ReceivedSpf(const Eval &eval, const std::string &client_ip, const std::string &helo,
                        const std::string &mail_from, const std::string &receiver) {
	std::string out = ResultName(eval.result);
	if (!eval.explanation.empty()) {
		out += " (" + receiver + ": " + eval.explanation + ")";
	}
	out += " client-ip=" + client_ip + ";";
	if (!helo.empty()) {
		out += " helo=" + helo + ";";
	}
	out += " envelope-from=<" + mail_from + ">;";
	return out;
}

} // namespace spf
} // namespace quackmail
