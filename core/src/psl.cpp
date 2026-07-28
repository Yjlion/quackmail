#include "quackmail/psl.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <vector>

namespace quackmail {
namespace psl {

namespace {

// What a rule says about the labels it matches.
enum class Kind {
	Normal,    // "uk", "co.uk"
	Wildcard,  // "*.ck" — any one label in that position
	Exception, // "!www.ck" — carve a registrable name back out of a wildcard
};

// The rule set, parsed once on first use. Keyed by the rule text with any
// leading "*." or "!" stripped, so a lookup is one hash of the candidate suffix.
using RuleMap = std::unordered_map<std::string, Kind>;

std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

const RuleMap &Rules() {
	static const RuleMap *map = [] {
		auto *m = new RuleMap();
		m->reserve(kPslRuleCount * 2);
		for (size_t c = 0; c < kPslChunkCount; c++) {
			const char *p = kPslChunks[c];
			std::string line;
			for (;; p++) {
				if (*p != '\n' && *p != '\0') {
					line += *p;
					continue;
				}
				if (!line.empty()) {
					Kind kind = Kind::Normal;
					size_t start = 0;
					if (line[0] == '!') {
						kind = Kind::Exception;
						start = 1;
					} else if (line.size() > 2 && line[0] == '*' && line[1] == '.') {
						kind = Kind::Wildcard;
						start = 2;
					}
					// An exception rule and a wildcard rule can share a key
					// ("*.ck" and "!www.ck" do not, but "*.foo"/"!foo" could);
					// the exception is the more specific answer, so it wins.
					auto key = Lower(line.substr(start));
					auto it = m->find(key);
					if (it == m->end() || kind == Kind::Exception) {
						(*m)[key] = kind;
					}
					line.clear();
				}
				if (*p == '\0') {
					break;
				}
			}
		}
		return m;
	}();
	return *map;
}

std::vector<std::string> Labels(const std::string &domain) {
	std::vector<std::string> out;
	std::string cur;
	for (char c : domain) {
		if (c == '.') {
			out.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	out.push_back(cur);
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

// How many trailing labels of `labels` form the public suffix.
size_t SuffixLabels(const std::vector<std::string> &labels) {
	const RuleMap &rules = Rules();
	size_t best = 0;
	// Walk every trailing sequence, longest match wins (PSL algorithm step 4).
	for (size_t i = labels.size(); i-- > 0;) {
		auto it = rules.find(Join(labels, i));
		if (it == rules.end()) {
			continue;
		}
		switch (it->second) {
		case Kind::Exception:
			// "!www.ck" makes www.ck registrable, so the suffix is one label
			// shorter than the rule. An exception outranks every other match,
			// which is why this returns rather than folding into `best`.
			return labels.size() - i - 1;
		case Kind::Wildcard:
			// "*.ck" is stored under "ck" and matches one label further left,
			// but only if there is a label there to match.
			best = std::max(best, labels.size() - i + (i > 0 ? 1 : 0));
			break;
		case Kind::Normal:
			best = std::max(best, labels.size() - i);
			break;
		}
	}
	// "If no rules match, the prevailing rule is '*'": the rightmost label.
	return best == 0 ? 1 : best;
}

} // namespace

std::string PublicSuffix(const std::string &domain) {
	std::string d = Lower(domain);
	while (!d.empty() && d.back() == '.') {
		d.pop_back(); // a fully-qualified name may carry a root dot
	}
	if (d.empty()) {
		return "";
	}
	auto labels = Labels(d);
	size_t n = std::min(SuffixLabels(labels), labels.size());
	return Join(labels, labels.size() - n);
}

std::string RegistrableDomain(const std::string &domain) {
	std::string d = Lower(domain);
	while (!d.empty() && d.back() == '.') {
		d.pop_back();
	}
	if (d.empty()) {
		return "";
	}
	auto labels = Labels(d);
	size_t n = std::min(SuffixLabels(labels), labels.size());
	if (labels.size() <= n) {
		return ""; // nothing but a public suffix: no registrable name
	}
	return Join(labels, labels.size() - n - 1);
}

} // namespace psl
} // namespace quackmail
