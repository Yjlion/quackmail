#include "quackmail/contentline.hpp"

#include "quackmail/util.hpp"

#include <algorithm>
#include <cctype>

namespace quackmail {
namespace contentline {

Line::Line() {
}

namespace {

std::string Trim(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return std::string();
	}
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

} // namespace

std::vector<std::string> Unfold(const std::string &text) {
	std::vector<std::string> lines;
	std::string cur;
	bool have = false;
	for (size_t i = 0; i <= text.size(); i++) {
		if (i == text.size() || text[i] == '\n') {
			std::string line = cur;
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && have) {
				lines.back() += line.substr(1);
			} else {
				lines.push_back(line);
				have = true;
			}
			cur.clear();
			if (i == text.size()) {
				break;
			}
		} else {
			cur += text[i];
		}
	}
	return lines;
}

void AppendFolded(std::string &out, const std::string &line) {
	const size_t kLimit = 75;
	size_t i = 0;
	bool first = true;
	while (i < line.size()) {
		// A continuation spends one of its 75 octets on the leading space.
		size_t budget = first ? kLimit : kLimit - 1;
		size_t take = std::min(budget, line.size() - i);
		if (i + take < line.size()) {
			// Back off while the first *untaken* byte is a continuation byte
			// (10xxxxxx), so the cut never lands inside a character.
			while (take > 1 && ((unsigned char)line[i + take] & 0xC0) == 0x80) {
				take--;
			}
		}
		if (!first) {
			out += " ";
		}
		out.append(line, i, take);
		out += "\r\n";
		i += take;
		first = false;
	}
	if (line.empty()) {
		out += "\r\n";
	}
}

std::string Unescape(const std::string &in) {
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); i++) {
		if (in[i] == '\\' && i + 1 < in.size()) {
			char n = in[i + 1];
			switch (n) {
			case 'n':
			case 'N':
				out += '\n';
				break;
			case ',':
				out += ',';
				break;
			case ';':
				out += ';';
				break;
			case '\\':
				out += '\\';
				break;
			default:
				// Not an escape we know. Keep both characters rather than
				// guessing which one was meant.
				out += '\\';
				out += n;
				break;
			}
			i++;
			continue;
		}
		out += in[i];
	}
	return out;
}

std::string Escape(const std::string &in) {
	std::string out;
	out.reserve(in.size());
	for (char c : in) {
		switch (c) {
		case '\\':
			out += "\\\\";
			break;
		case ';':
			out += "\\;";
			break;
		case ',':
			out += "\\,";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			// Folded away; a bare CR inside a value is never meaningful.
			break;
		default:
			out += c;
		}
	}
	return out;
}

std::vector<std::string> SplitEscaped(const std::string &in, char sep) {
	std::vector<std::string> out;
	for (auto &piece : SplitRaw(in, sep)) {
		out.push_back(Unescape(piece));
	}
	return out;
}

std::vector<std::string> SplitRaw(const std::string &in, char sep) {
	std::vector<std::string> out;
	std::string cur;
	for (size_t i = 0; i < in.size(); i++) {
		if (in[i] == '\\' && i + 1 < in.size()) {
			// Carry the escape through untouched, so an escaped separator does
			// not split here.
			cur += in[i];
			cur += in[i + 1];
			i++;
			continue;
		}
		if (in[i] == sep) {
			out.push_back(cur);
			cur.clear();
			continue;
		}
		cur += in[i];
	}
	out.push_back(cur);
	return out;
}

bool Parse(const std::string &line, Line &out, bool allow_group) {
	out = Line();

	// The colon that ends the name-and-parameters part, skipping any inside a
	// quoted parameter value.
	size_t colon = std::string::npos;
	bool quoted = false;
	for (size_t i = 0; i < line.size(); i++) {
		if (line[i] == '"') {
			quoted = !quoted;
		} else if (line[i] == ':' && !quoted) {
			colon = i;
			break;
		}
	}
	if (colon == std::string::npos) {
		return false;
	}
	std::string head = line.substr(0, colon);
	out.value = line.substr(colon + 1);

	if (allow_group) {
		size_t dot = head.find('.');
		size_t semi = head.find(';');
		if (dot != std::string::npos && (semi == std::string::npos || dot < semi)) {
			out.group = head.substr(0, dot);
			head = head.substr(dot + 1);
		}
	}

	// Parameters, split on unquoted ';'. The quotes themselves are dropped:
	// they are grammar, not content.
	std::vector<std::string> parts;
	std::string cur;
	quoted = false;
	for (char c : head) {
		if (c == '"') {
			quoted = !quoted;
			continue;
		}
		if (c == ';' && !quoted) {
			parts.push_back(cur);
			cur.clear();
			continue;
		}
		cur += c;
	}
	parts.push_back(cur);

	if (parts.empty() || Trim(parts[0]).empty()) {
		return false;
	}
	out.name = util::Upper(Trim(parts[0]));
	for (size_t i = 1; i < parts.size(); i++) {
		std::string kv = Trim(parts[i]);
		if (kv.empty()) {
			continue;
		}
		size_t eq = kv.find('=');
		if (eq == std::string::npos) {
			// vCard 2.1 shorthand: "TEL;WORK;VOICE:..." with no "TYPE=".
			out.params.emplace_back("TYPE", util::Upper(kv));
		} else {
			out.params.emplace_back(util::Upper(Trim(kv.substr(0, eq))), Trim(kv.substr(eq + 1)));
		}
	}
	return true;
}

std::string Format(const Line &line) {
	std::string out;
	if (!line.group.empty()) {
		out += line.group + ".";
	}
	out += line.name;
	for (auto &kv : line.params) {
		out += ";" + kv.first + "=";
		// Not ',': a comma in a parameter value is the list separator, so
		// "TYPE=INTERNET,PREF" is two types and quoting it would make it one
		// whose name contains a comma.
		if (kv.second.find_first_of(":;") != std::string::npos) {
			out += "\"" + kv.second + "\"";
		} else {
			out += kv.second;
		}
	}
	out += ":" + line.value;
	return out;
}

} // namespace contentline
} // namespace quackmail
