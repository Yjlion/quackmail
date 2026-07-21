#include "quackmail/mime.hpp"

#include <algorithm>
#include <cctype>

namespace quackmail {
namespace mime {

static std::string ToLower(const std::string &s) {
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
	return out;
}

static std::string Trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace((unsigned char)s[b])) {
		b++;
	}
	while (e > b && std::isspace((unsigned char)s[e - 1])) {
		e--;
	}
	return s.substr(b, e - b);
}

std::string ExtractAddress(const std::string &header_value) {
	auto lt = header_value.find('<');
	auto gt = header_value.find('>', lt == std::string::npos ? 0 : lt);
	if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
		return Trim(header_value.substr(lt + 1, gt - lt - 1));
	}
	return Trim(header_value);
}

ParsedMessage Parse(const std::string &raw) {
	ParsedMessage msg;

	// Split headers from body at the first blank line (CRLF CRLF or LF LF).
	size_t header_end = raw.size();
	{
		size_t p = raw.find("\r\n\r\n");
		if (p != std::string::npos) {
			header_end = p;
		} else {
			size_t q = raw.find("\n\n");
			if (q != std::string::npos) {
				header_end = q;
			}
		}
	}
	std::string header_block = raw.substr(0, header_end);

	// Split into logical (unfolded) header lines.
	std::vector<std::string> lines;
	std::string cur;
	size_t i = 0;
	while (i < header_block.size()) {
		char c = header_block[i];
		if (c == '\r') {
			i++;
			continue;
		}
		if (c == '\n') {
			// Peek: a following space/tab means folded continuation.
			if (i + 1 < header_block.size() && (header_block[i + 1] == ' ' || header_block[i + 1] == '\t')) {
				cur += ' ';
				i++;
				while (i + 1 < header_block.size() && (header_block[i + 1] == ' ' || header_block[i + 1] == '\t')) {
					i++;
				}
				i++;
				continue;
			}
			lines.push_back(cur);
			cur.clear();
			i++;
			continue;
		}
		cur += c;
		i++;
	}
	if (!cur.empty()) {
		lines.push_back(cur);
	}

	for (auto &line : lines) {
		auto colon = line.find(':');
		if (colon == std::string::npos) {
			continue;
		}
		std::string name = Trim(line.substr(0, colon));
		std::string value = Trim(line.substr(colon + 1));
		if (name.empty()) {
			continue;
		}
		msg.headers.emplace_back(name, value);
		std::string lname = ToLower(name);
		if (lname == "subject" && msg.subject.empty()) {
			msg.subject = value;
		} else if (lname == "from" && msg.from.empty()) {
			msg.from = value;
		} else if (lname == "message-id" && msg.message_id.empty()) {
			msg.message_id = value;
		}
	}
	return msg;
}

} // namespace mime
} // namespace quackmail
