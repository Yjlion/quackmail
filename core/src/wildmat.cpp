#include "quackmail/wildmat.hpp"

#include <vector>

namespace quackmail {
namespace {

// Match one wildmat item (no commas, no leading '!') against text.
bool MatchItem(const std::string &text, size_t ti, const std::string &pat, size_t pi) {
	while (pi < pat.size()) {
		char p = pat[pi];

		if (p == '*') {
			// Collapse runs of '*', then try every possible split.
			while (pi < pat.size() && pat[pi] == '*') {
				pi++;
			}
			if (pi == pat.size()) {
				return true;
			}
			for (size_t k = ti; k <= text.size(); k++) {
				if (MatchItem(text, k, pat, pi)) {
					return true;
				}
			}
			return false;
		}

		if (ti >= text.size()) {
			return false;
		}

		if (p == '?') {
			pi++;
			ti++;
			continue;
		}

		if (p == '[') {
			size_t k = pi + 1;
			bool negate = false;
			if (k < pat.size() && (pat[k] == '^' || pat[k] == '!')) {
				negate = true;
				k++;
			}
			bool hit = false;
			bool first = true;
			for (; k < pat.size() && (pat[k] != ']' || first); k++, first = false) {
				if (k + 2 < pat.size() && pat[k + 1] == '-' && pat[k + 2] != ']') {
					if (text[ti] >= pat[k] && text[ti] <= pat[k + 2]) {
						hit = true;
					}
					k += 2;
				} else if (pat[k] == text[ti]) {
					hit = true;
				}
			}
			if (k >= pat.size() || hit == negate) {
				return false;
			}
			pi = k + 1;
			ti++;
			continue;
		}

		if (p == '\\' && pi + 1 < pat.size()) {
			pi++;
			p = pat[pi];
		}
		if (p != text[ti]) {
			return false;
		}
		pi++;
		ti++;
	}
	return ti == text.size();
}

} // namespace

bool WildmatMatch(const std::string &text, const std::string &pattern) {
	if (pattern.empty()) {
		return true;
	}
	bool result = false;
	bool decided = false;
	size_t start = 0;
	while (start <= pattern.size()) {
		size_t comma = pattern.find(',', start);
		std::string item = pattern.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		bool negate = !item.empty() && item[0] == '!';
		if (negate) {
			item.erase(0, 1);
		}
		// The last matching item wins (RFC 3977 §4.2).
		if (MatchItem(text, 0, item, 0)) {
			result = !negate;
			decided = true;
		}
		if (comma == std::string::npos) {
			break;
		}
		start = comma + 1;
	}
	return decided && result;
}

} // namespace quackmail
