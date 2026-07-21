#include "quackmail/mime.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace quackmail {
namespace mime {

static int HexVal(unsigned char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

static std::string LowerAscii(const std::string &s) {
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
	return out;
}

std::string Latin1ToUtf8(const std::string &in) {
	std::string out;
	out.reserve(in.size());
	for (unsigned char c : in) {
		if (c < 0x80) {
			out += static_cast<char>(c);
		} else {
			out += static_cast<char>(0xC0 | (c >> 6));
			out += static_cast<char>(0x80 | (c & 0x3F));
		}
	}
	return out;
}

// Decode quoted-printable. When `underscore_is_space` is true we are decoding
// the RFC 2047 "Q" word variant, where '_' represents a space.
static std::string DecodeQpImpl(const std::string &in, bool underscore_is_space) {
	std::string out;
	out.reserve(in.size());
	size_t i = 0;
	while (i < in.size()) {
		char c = in[i];
		if (c == '=') {
			// Soft line break: "=" immediately followed by CRLF / LF.
			if (i + 1 < in.size() && in[i + 1] == '\n') {
				i += 2;
				continue;
			}
			if (i + 2 < in.size() && in[i + 1] == '\r' && in[i + 2] == '\n') {
				i += 3;
				continue;
			}
			if (i + 2 < in.size()) {
				int hi = HexVal(static_cast<unsigned char>(in[i + 1]));
				int lo = HexVal(static_cast<unsigned char>(in[i + 2]));
				if (hi >= 0 && lo >= 0) {
					out += static_cast<char>((hi << 4) | lo);
					i += 3;
					continue;
				}
			}
			// Not a valid escape: pass the '=' through literally.
			out += c;
			i++;
			continue;
		}
		if (underscore_is_space && c == '_') {
			out += ' ';
			i++;
			continue;
		}
		out += c;
		i++;
	}
	return out;
}

std::string DecodeQuotedPrintable(const std::string &in) {
	return DecodeQpImpl(in, false);
}

std::string EncodeQuotedPrintable(const std::string &in) {
	static const char kHex[] = "0123456789ABCDEF";
	std::string out;
	size_t line_len = 0;
	auto emit = [&](const std::string &tok) {
		if (line_len + tok.size() > 75) {
			out += "=\r\n";
			line_len = 0;
		}
		out += tok;
		line_len += tok.size();
	};
	for (size_t i = 0; i < in.size(); i++) {
		unsigned char c = static_cast<unsigned char>(in[i]);
		if (c == '\n') {
			out += "\r\n";
			line_len = 0;
			continue;
		}
		if (c == '\r') {
			continue;
		}
		bool printable = (c >= 33 && c <= 126 && c != '=') || c == ' ' || c == '\t';
		if (printable) {
			emit(std::string(1, static_cast<char>(c)));
		} else {
			std::string esc = "=";
			esc += kHex[c >> 4];
			esc += kHex[c & 0x0F];
			emit(esc);
		}
	}
	return out;
}

std::string DecodeContentTransferEncoding(const std::string &encoding, const std::string &body) {
	std::string enc = LowerAscii(encoding);
	// Trim surrounding whitespace from the token.
	size_t b = enc.find_first_not_of(" \t");
	size_t e = enc.find_last_not_of(" \t");
	if (b == std::string::npos) {
		enc.clear();
	} else {
		enc = enc.substr(b, e - b + 1);
	}
	if (enc == "base64") {
		std::string out;
		if (util::Base64Decode(body, out)) {
			return out;
		}
		return out; // partial decode is better than nothing
	}
	if (enc == "quoted-printable") {
		return DecodeQuotedPrintable(body);
	}
	// "7bit", "8bit", "binary", empty, or unknown: identity.
	return body;
}

// Decode a single RFC 2047 encoded word given its charset, encoding letter, and
// encoded text. Returns the UTF-8 (or raw) decoded bytes.
static std::string DecodeOneEncodedWord(const std::string &charset, char enc, const std::string &text) {
	std::string raw;
	char e = static_cast<char>(std::toupper(static_cast<unsigned char>(enc)));
	if (e == 'B') {
		if (!util::Base64Decode(text, raw)) {
			raw.clear();
		}
	} else if (e == 'Q') {
		raw = DecodeQpImpl(text, true);
	} else {
		return text;
	}
	std::string cs = LowerAscii(charset);
	if (cs == "utf-8" || cs == "utf8" || cs == "us-ascii" || cs == "ascii" || cs.empty()) {
		return raw;
	}
	if (cs == "iso-8859-1" || cs == "latin1" || cs == "iso8859-1") {
		return Latin1ToUtf8(raw);
	}
	// Unknown charset: return decoded bytes as-is.
	return raw;
}

std::string DecodeEncodedWords(const std::string &in) {
	std::string out;
	size_t i = 0;
	bool last_was_word = false;      // did we just emit an encoded word?
	size_t pending_ws_start = std::string::npos; // run of whitespace held back
	while (i < in.size()) {
		if (in[i] == '=' && i + 1 < in.size() && in[i + 1] == '?') {
			// Try to parse: =?charset?E?text?=
			size_t q1 = in.find('?', i + 2);
			if (q1 != std::string::npos && q1 + 2 < in.size() && in[q1 + 2] == '?') {
				size_t enc_pos = q1 + 1;
				char enc = in[enc_pos];
				size_t text_start = q1 + 3;
				size_t end = in.find("?=", text_start);
				if (end != std::string::npos) {
					std::string charset = in.substr(i + 2, q1 - (i + 2));
					std::string text = in.substr(text_start, end - text_start);
					// Between two adjacent encoded words, linear whitespace is
					// dropped (RFC 2047 §6.2); otherwise it is preserved.
					if (!(last_was_word && pending_ws_start != std::string::npos)) {
						if (pending_ws_start != std::string::npos) {
							out += in.substr(pending_ws_start, i - pending_ws_start);
						}
					}
					pending_ws_start = std::string::npos;
					out += DecodeOneEncodedWord(charset, enc, text);
					last_was_word = true;
					i = end + 2;
					continue;
				}
			}
		}
		// Whitespace between potential encoded words is buffered so we can drop
		// it if the next token turns out to be another encoded word.
		if (in[i] == ' ' || in[i] == '\t' || in[i] == '\r' || in[i] == '\n') {
			if (pending_ws_start == std::string::npos) {
				pending_ws_start = i;
			}
			i++;
			continue;
		}
		if (pending_ws_start != std::string::npos) {
			out += in.substr(pending_ws_start, i - pending_ws_start);
			pending_ws_start = std::string::npos;
		}
		out += in[i];
		last_was_word = false;
		i++;
	}
	if (pending_ws_start != std::string::npos) {
		out += in.substr(pending_ws_start);
	}
	return out;
}

} // namespace mime
} // namespace quackmail
