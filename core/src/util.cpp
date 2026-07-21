#include "quackmail/util.hpp"

#include <algorithm>
#include <cctype>

namespace quackmail {
namespace util {

static const char kEnc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::string &in) {
	std::string out;
	int val = 0, bits = -6;
	for (unsigned char c : in) {
		val = (val << 8) + c;
		bits += 8;
		while (bits >= 0) {
			out += kEnc[(val >> bits) & 0x3F];
			bits -= 6;
		}
	}
	if (bits > -6) {
		out += kEnc[((val << 8) >> (bits + 8)) & 0x3F];
	}
	while (out.size() % 4) {
		out += '=';
	}
	return out;
}

bool Base64Decode(const std::string &in, std::string &out) {
	static int table[256];
	static bool init = false;
	if (!init) {
		std::fill(std::begin(table), std::end(table), -1);
		for (int i = 0; i < 64; i++) {
			table[(unsigned char)kEnc[i]] = i;
		}
		init = true;
	}
	out.clear();
	int val = 0, bits = -8;
	for (unsigned char c : in) {
		if (c == '=' || std::isspace(c)) {
			continue;
		}
		int d = table[c];
		if (d == -1) {
			return false;
		}
		val = (val << 6) + d;
		bits += 6;
		if (bits >= 0) {
			out += char((val >> bits) & 0xFF);
			bits -= 8;
		}
	}
	return true;
}

std::string Upper(const std::string &s) {
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::toupper(c); });
	return out;
}

std::string LocalPart(const std::string &addr) {
	auto at = addr.find('@');
	return at == std::string::npos ? addr : addr.substr(0, at);
}

} // namespace util
} // namespace quackmail
