#include "quackmail/dmarc.hpp"

#include "quackmail/dns.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace quackmail {
namespace dmarc {

namespace {

std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string Trim(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return "";
	}
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

std::vector<std::string> Labels(const std::string &domain) {
	std::vector<std::string> out;
	std::string cur;
	for (char c : domain) {
		if (c == '.') {
			if (!cur.empty()) {
				out.push_back(cur);
			}
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty()) {
		out.push_back(cur);
	}
	return out;
}

std::string Join(const std::vector<std::string> &labels, size_t from) {
	std::string out;
	for (size_t i = from; i < labels.size(); i++) {
		if (!out.empty()) {
			out += '.';
		}
		out += labels[i];
	}
	return out;
}

// Multi-label public suffixes common enough to be worth special-casing without
// shipping a full Public Suffix List. See the note in the header.
bool IsTwoLabelSuffix(const std::string &a, const std::string &b) {
	static const char *kSecondLevel[] = {"co",  "com", "net", "org", "gov", "edu",
	                                     "ac",  "mil", "sch", "ltd", "plc", "me",
	                                     "or",  "ne",  "gr",  "in",  "nic", nullptr};
	static const char *kCountry[] = {"uk", "au", "nz", "za", "jp", "kr", "br", "cn", "il",
	                                 "tr", "mx", "ar", "id", "th", "sg", "hk", "my", "ph",
	                                 "pk", "ng", "ke", "at", "pl", nullptr};
	bool second = false, country = false;
	for (int i = 0; kSecondLevel[i]; i++) {
		if (a == kSecondLevel[i]) {
			second = true;
			break;
		}
	}
	for (int i = 0; kCountry[i]; i++) {
		if (b == kCountry[i]) {
			country = true;
			break;
		}
	}
	return second && country;
}

// Alignment: strict requires an exact domain match, relaxed accepts a shared
// organizational domain (RFC 7489 §3.1).
bool Aligned(const std::string &a, const std::string &b, bool strict) {
	if (a.empty() || b.empty()) {
		return false;
	}
	std::string la = Lower(a), lb = Lower(b);
	if (la == lb) {
		return true;
	}
	if (strict) {
		return false;
	}
	return OrganizationalDomain(la) == OrganizationalDomain(lb);
}

// Deterministic 0..99 sample for pct=, seeded from the Message-ID so a message
// is always given the same disposition however many times it is evaluated.
int SampleBucket(const std::string &seed) {
	// FNV-1a, which is plenty for a 100-bucket split.
	unsigned long long h = 1469598103934665603ULL;
	for (unsigned char c : seed) {
		h ^= c;
		h *= 1099511628211ULL;
	}
	return (int)(h % 100ULL);
}

Policy ParsePolicy(const std::string &v) {
	std::string p = Lower(Trim(v));
	if (p == "reject") {
		return Policy::Reject;
	}
	if (p == "quarantine") {
		return Policy::Quarantine;
	}
	return Policy::None;
}

// Fetch _dmarc.<domain>, then _dmarc.<organizational domain> (§6.6.3).
Result FetchRecord(const std::string &from_domain, std::string &record, std::string &policy_domain) {
	record.clear();
	policy_domain.clear();

	std::vector<std::string> candidates;
	candidates.push_back(from_domain);
	std::string org = OrganizationalDomain(from_domain);
	if (org != from_domain && !org.empty()) {
		candidates.push_back(org);
	}

	bool temp = false;
	for (auto &d : candidates) {
		std::vector<std::string> txts;
		auto st = dns::LookupTXT("_dmarc." + d, txts);
		if (st == dns::Status::TempFail) {
			temp = true;
			continue;
		}
		for (auto &t : txts) {
			std::string lowered = Lower(Trim(t));
			// The version tag must be first and is the only reliable marker.
			if (lowered.rfind("v=dmarc1", 0) != 0) {
				continue;
			}
			record = t;
			policy_domain = d;
			return Result::Pass; // "record found"; the caller evaluates it
		}
	}
	return temp ? Result::TempError : Result::None;
}

} // namespace

std::string OrganizationalDomain(const std::string &domain) {
	auto labels = Labels(Lower(domain));
	if (labels.size() <= 2) {
		return Join(labels, 0);
	}
	// "example.co.uk" keeps three labels; "mail.example.com" keeps two.
	const std::string &tld = labels[labels.size() - 1];
	const std::string &sld = labels[labels.size() - 2];
	if (IsTwoLabelSuffix(sld, tld) && labels.size() >= 3) {
		return Join(labels, labels.size() - 3);
	}
	return Join(labels, labels.size() - 2);
}

Eval Evaluate(const std::string &from_domain, const std::string &spf_domain, spf::Result spf_result,
              const std::vector<dkim::VerifyResult> &dkim_results, const std::string &msg_id) {
	Eval eval;
	eval.from_domain = from_domain;

	if (from_domain.empty()) {
		eval.result = Result::None;
		eval.info = "no From: domain to evaluate";
		return eval;
	}

	Result fetch = FetchRecord(from_domain, eval.record, eval.policy_domain);
	if (fetch == Result::TempError) {
		eval.result = Result::TempError;
		eval.info = "DMARC record lookup failed temporarily";
		return eval;
	}
	if (fetch == Result::None) {
		eval.result = Result::None;
		eval.info = "no DMARC record for " + from_domain;
		return eval;
	}

	// Parse the record's tags.
	std::string p_tag, sp_tag, adkim = "r", aspf = "r";
	int pct = 100;
	{
		size_t pos = 0;
		while (pos < eval.record.size()) {
			auto semi = eval.record.find(';', pos);
			std::string chunk =
			    eval.record.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
			pos = semi == std::string::npos ? eval.record.size() : semi + 1;
			auto eq = chunk.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			std::string name = Lower(Trim(chunk.substr(0, eq)));
			std::string value = Trim(chunk.substr(eq + 1));
			if (name == "p") {
				p_tag = value;
			} else if (name == "sp") {
				sp_tag = value;
			} else if (name == "adkim") {
				adkim = Lower(value);
			} else if (name == "aspf") {
				aspf = Lower(value);
			} else if (name == "pct") {
				pct = std::atoi(value.c_str());
			}
		}
	}
	if (p_tag.empty()) {
		eval.result = Result::PermError;
		eval.info = "DMARC record has no p= tag";
		return eval;
	}

	// A subdomain of the policy domain uses sp= when it is present.
	bool is_subdomain = !eval.policy_domain.empty() && Lower(from_domain) != Lower(eval.policy_domain);
	Policy published = ParsePolicy(is_subdomain && !sp_tag.empty() ? sp_tag : p_tag);

	// --- alignment ---
	if (spf_result == spf::Result::Pass) {
		eval.spf_aligned = Aligned(spf_domain, from_domain, aspf == "s");
	}
	for (auto &d : dkim_results) {
		if (d.result == dkim::Result::Pass && Aligned(d.domain, from_domain, adkim == "s")) {
			eval.dkim_aligned = true;
			break;
		}
	}

	if (eval.spf_aligned || eval.dkim_aligned) {
		eval.result = Result::Pass;
		eval.policy = Policy::None;
		eval.info = eval.dkim_aligned ? "aligned DKIM signature" : "aligned SPF pass";
		return eval;
	}

	eval.result = Result::Fail;
	eval.info = "neither SPF nor DKIM authenticated an aligned identity";

	// pct< 100 asks that only a sample get the full policy; the remainder is
	// treated one step down (reject -> quarantine, quarantine -> none).
	if (pct >= 100 || SampleBucket(msg_id.empty() ? from_domain : msg_id) < pct) {
		eval.policy = published;
	} else if (published == Policy::Reject) {
		eval.policy = Policy::Quarantine;
		eval.info += "; outside pct= sample, softened to quarantine";
	} else {
		eval.policy = Policy::None;
		if (published != Policy::None) {
			eval.info += "; outside pct= sample";
		}
	}
	return eval;
}

std::string ResultName(Result r) {
	switch (r) {
	case Result::None:
		return "none";
	case Result::Pass:
		return "pass";
	case Result::Fail:
		return "fail";
	case Result::TempError:
		return "temperror";
	case Result::PermError:
		return "permerror";
	}
	return "none";
}

std::string PolicyName(Policy p) {
	switch (p) {
	case Policy::None:
		return "none";
	case Policy::Quarantine:
		return "quarantine";
	case Policy::Reject:
		return "reject";
	}
	return "none";
}

} // namespace dmarc
} // namespace quackmail
