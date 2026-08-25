#include "quackmail/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

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

std::string Lower(const std::string &s) {
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
	return out;
}

std::string LocalPart(const std::string &addr) {
	auto at = addr.find('@');
	return at == std::string::npos ? addr : addr.substr(0, at);
}

std::string RfcDate(int64_t epoch) {
	std::time_t t = epoch > 0 ? static_cast<std::time_t>(epoch) : std::time(nullptr);
	std::tm tm_utc {};
#if defined(_WIN32)
	gmtime_s(&tm_utc, &t);
#else
	gmtime_r(&t, &tm_utc);
#endif
	// The names are spelled out rather than left to strftime's %a/%b, which
	// follow LC_TIME — a server started under a non-English locale would
	// otherwise put non-ASCII day names into a header that must be English.
	static const char *const kDay[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char *const kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	char buf[64];
	std::snprintf(buf, sizeof buf, "%s, %d %s %d %02d:%02d:%02d +0000", kDay[tm_utc.tm_wday % 7],
	              tm_utc.tm_mday, kMon[tm_utc.tm_mon % 12], tm_utc.tm_year + 1900, tm_utc.tm_hour,
	              tm_utc.tm_min, tm_utc.tm_sec);
	return std::string(buf);
}

bool RandomBytes(size_t n, std::string &out) {
	out.assign(n, '\0');
	if (n == 0) {
		return true;
	}
	return RAND_bytes(reinterpret_cast<unsigned char *>(&out[0]), (int)n) == 1;
}

std::string RandomHex(size_t bytes) {
	std::string raw;
	if (!RandomBytes(bytes, raw)) {
		return std::string();
	}
	static const char kHex[] = "0123456789abcdef";
	std::string out;
	out.reserve(bytes * 2);
	for (unsigned char c : raw) {
		out += kHex[c >> 4];
		out += kHex[c & 0x0F];
	}
	return out;
}

std::string Base64UrlEncode(const std::string &in) {
	std::string b64 = Base64Encode(in);
	std::string out;
	out.reserve(b64.size());
	for (char c : b64) {
		if (c == '=' || c == '\r' || c == '\n') {
			continue; // unpadded, and never folded
		}
		out += (c == '+') ? '-' : (c == '/') ? '_' : c;
	}
	return out;
}

bool Base64UrlDecode(const std::string &in, std::string &out) {
	std::string b64;
	b64.reserve(in.size() + 3);
	for (char c : in) {
		if (c == '\r' || c == '\n') {
			continue;
		}
		b64 += (c == '-') ? '+' : (c == '_') ? '/' : c;
	}
	while (b64.size() % 4 != 0) {
		b64 += '=';
	}
	return Base64Decode(b64, out);
}

std::string RandomBase64Url(size_t bytes) {
	std::string raw;
	if (!RandomBytes(bytes, raw)) {
		return std::string();
	}
	return Base64UrlEncode(raw);
}

std::string Sha256Raw(const std::string &in) {
	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char *>(in.data()), in.size(), digest);
	return std::string(reinterpret_cast<const char *>(digest), sizeof(digest));
}

std::string Sha256Hex(const std::string &in) {
	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char *>(in.data()), in.size(), digest);
	static const char kHex[] = "0123456789abcdef";
	std::string out;
	out.reserve(sizeof(digest) * 2);
	for (unsigned char c : digest) {
		out += kHex[c >> 4];
		out += kHex[c & 0x0F];
	}
	return out;
}

bool SecureEquals(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) {
		return false;
	}
	if (a.empty()) {
		return true;
	}
	return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace util
} // namespace quackmail
