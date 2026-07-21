#include "quackmail/mime.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace quackmail {
namespace mime {

static std::string TrimWs(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
		b++;
	}
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
		e--;
	}
	return s.substr(b, e - b);
}

static std::string UnquoteStr(const std::string &in) {
	std::string s = TrimWs(in);
	if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
		std::string out;
		for (size_t i = 1; i + 1 < s.size(); i++) {
			if (s[i] == '\\' && i + 2 < s.size()) {
				out += s[++i];
			} else {
				out += s[i];
			}
		}
		return out;
	}
	return s;
}

// ---- address-list parsing (RFC 5322 §3.4) ----------------------------------

// Split an address-list into top-level mailbox chunks, breaking on ',' and ';'
// while respecting quoted strings, comments, and angle-addr. Comments are
// dropped (their content is not used as a display name here).
static std::vector<std::string> SplitAddresses(const std::string &value) {
	std::vector<std::string> chunks;
	std::string cur;
	int angle = 0, comment = 0;
	bool quote = false;
	for (size_t i = 0; i < value.size(); i++) {
		char c = value[i];
		if (quote) {
			cur += c;
			if (c == '\\' && i + 1 < value.size()) {
				cur += value[++i];
			} else if (c == '"') {
				quote = false;
			}
			continue;
		}
		if (comment > 0) {
			if (c == '\\' && i + 1 < value.size()) {
				i++;
			} else if (c == '(') {
				comment++;
			} else if (c == ')') {
				comment--;
			}
			continue;
		}
		switch (c) {
		case '"':
			quote = true;
			cur += c;
			break;
		case '(':
			comment++;
			break;
		case '<':
			angle++;
			cur += c;
			break;
		case '>':
			if (angle > 0) {
				angle--;
			}
			cur += c;
			break;
		case ',':
		case ';':
			if (angle == 0) {
				chunks.push_back(cur);
				cur.clear();
			} else {
				cur += c;
			}
			break;
		default:
			cur += c;
			break;
		}
	}
	chunks.push_back(cur);
	return chunks;
}

// Strip a leading "Group name:" label from a chunk (RFC 5322 group syntax). The
// label is a top-level ':' that precedes any '@' or '<'.
static std::string StripGroupLabel(const std::string &chunk) {
	bool quote = false;
	for (size_t i = 0; i < chunk.size(); i++) {
		char c = chunk[i];
		if (quote) {
			if (c == '\\' && i + 1 < chunk.size()) {
				i++;
			} else if (c == '"') {
				quote = false;
			}
			continue;
		}
		if (c == '"') {
			quote = true;
		} else if (c == '@' || c == '<') {
			return chunk; // an address started before any ':'
		} else if (c == ':') {
			return chunk.substr(i + 1);
		}
	}
	return chunk;
}

std::vector<Address> ParseAddressList(const std::string &value) {
	std::vector<Address> out;
	for (auto &raw_chunk : SplitAddresses(value)) {
		std::string chunk = TrimWs(StripGroupLabel(raw_chunk));
		if (chunk.empty()) {
			continue;
		}
		Address addr;
		auto lt = chunk.find('<');
		auto gt = chunk.rfind('>');
		if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
			addr.addr = TrimWs(chunk.substr(lt + 1, gt - lt - 1));
			std::string name = TrimWs(chunk.substr(0, lt));
			addr.name = DecodeEncodedWords(UnquoteStr(name));
		} else {
			addr.addr = TrimWs(chunk);
			addr.name = "";
		}
		if (addr.addr.empty() && addr.name.empty()) {
			continue;
		}
		out.push_back(addr);
	}
	return out;
}

// ---- date parsing (RFC 5322 §3.3) ------------------------------------------

static int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = static_cast<unsigned>(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

static int MonthIndex(const std::string &tok) {
	static const char *kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun",
	                                "jul", "aug", "sep", "oct", "nov", "dec"};
	std::string t = tok.substr(0, 3);
	std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return std::tolower(c); });
	for (int i = 0; i < 12; i++) {
		if (t == kMonths[i]) {
			return i + 1;
		}
	}
	return 0;
}

// Return zone offset in seconds; sets ok=false when unrecognized.
static int ZoneOffsetSeconds(const std::string &z, bool &ok) {
	ok = true;
	if (z.empty()) {
		ok = false;
		return 0;
	}
	if (z[0] == '+' || z[0] == '-') {
		std::string digits;
		for (char c : z) {
			if (std::isdigit(static_cast<unsigned char>(c))) {
				digits += c;
			}
		}
		if (digits.size() < 4) {
			ok = false;
			return 0;
		}
		int hh = std::stoi(digits.substr(0, 2));
		int mm = std::stoi(digits.substr(2, 2));
		int off = hh * 3600 + mm * 60;
		return z[0] == '-' ? -off : off;
	}
	std::string u = z;
	std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return std::toupper(c); });
	if (u == "UT" || u == "UTC" || u == "GMT" || u == "Z") {
		return 0;
	}
	if (u == "EST") return -5 * 3600;
	if (u == "EDT") return -4 * 3600;
	if (u == "CST") return -6 * 3600;
	if (u == "CDT") return -5 * 3600;
	if (u == "MST") return -7 * 3600;
	if (u == "MDT") return -6 * 3600;
	if (u == "PST") return -8 * 3600;
	if (u == "PDT") return -7 * 3600;
	// Obsolete single-letter military zones are treated as -0000 per RFC 2822.
	return 0;
}

bool ParseDate(const std::string &value, int64_t &epoch_seconds) {
	// Tokenize on whitespace and commas.
	std::vector<std::string> toks;
	std::string cur;
	for (char c : value) {
		if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
			if (!cur.empty()) {
				toks.push_back(cur);
				cur.clear();
			}
		} else {
			cur += c;
		}
	}
	if (!cur.empty()) {
		toks.push_back(cur);
	}

	// Find the month token.
	int month = 0;
	size_t mi = 0;
	for (size_t i = 0; i < toks.size(); i++) {
		int m = MonthIndex(toks[i]);
		if (m) {
			month = m;
			mi = i;
			break;
		}
	}
	if (month == 0 || mi == 0 || mi + 1 >= toks.size()) {
		return false;
	}

	int day = 0, year = 0;
	try {
		day = std::stoi(toks[mi - 1]);
		year = std::stoi(toks[mi + 1]);
	} catch (...) {
		return false;
	}
	if (year < 100) {
		year += (year < 50) ? 2000 : 1900;
	}

	// Time token (contains ':') is the next after year.
	int hh = 0, mm = 0, ss = 0;
	size_t ti = mi + 2;
	if (ti >= toks.size() || toks[ti].find(':') == std::string::npos) {
		return false;
	}
	{
		const std::string &t = toks[ti];
		int vals[3] = {0, 0, 0};
		int n = 0;
		std::string part;
		for (size_t i = 0; i <= t.size(); i++) {
			if (i == t.size() || t[i] == ':') {
				if (n < 3) {
					vals[n++] = part.empty() ? 0 : std::atoi(part.c_str());
				}
				part.clear();
			} else if (std::isdigit(static_cast<unsigned char>(t[i]))) {
				part += t[i];
			}
		}
		hh = vals[0];
		mm = vals[1];
		ss = vals[2];
	}

	int offset = 0;
	if (ti + 1 < toks.size()) {
		bool ok = false;
		int off = ZoneOffsetSeconds(toks[ti + 1], ok);
		if (ok) {
			offset = off;
		}
	}

	int64_t days = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
	int64_t secs = days * 86400 + hh * 3600 + mm * 60 + ss;
	epoch_seconds = secs - offset;
	return true;
}

} // namespace mime
} // namespace quackmail
