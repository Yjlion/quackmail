#include "quackmail/markdown.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace quackmail {
namespace markdown {

namespace {

void Escape(std::string &out, const std::string &in) {
	for (char c : in) {
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		case '"':
			out += "&quot;";
			break;
		case '\'':
			out += "&#39;";
			break;
		default:
			out += c;
		}
	}
}

std::string Escaped(const std::string &in) {
	std::string out;
	Escape(out, in);
	return out;
}

std::string Trim(const std::string &s) {
	size_t a = s.find_first_not_of(" \t\r");
	if (a == std::string::npos) {
		return "";
	}
	size_t b = s.find_last_not_of(" \t\r");
	return s.substr(a, b - a + 1);
}

// Only these three schemes reach an href. A wiki page is public to its room,
// and `javascript:` in a link somebody typed is the whole attack.
bool SafeUrl(const std::string &url) {
	if (url.empty()) {
		return false;
	}
	if (url[0] == '/' || url[0] == '#' || url[0] == '?') {
		return true; // same-origin
	}
	size_t colon = url.find(':');
	size_t slash = url.find('/');
	if (colon == std::string::npos || (slash != std::string::npos && slash < colon)) {
		return true; // relative
	}
	std::string scheme;
	for (size_t i = 0; i < colon; i++) {
		scheme += (char)tolower((unsigned char)url[i]);
	}
	return scheme == "http" || scheme == "https" || scheme == "mailto";
}

std::vector<std::string> SplitLines(const std::string &s) {
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= s.size()) {
		size_t nl = s.find('\n', start);
		if (nl == std::string::npos) {
			if (start < s.size()) {
				out.push_back(s.substr(start));
			}
			break;
		}
		std::string line = s.substr(start, nl - start);
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		out.push_back(line);
		start = nl + 1;
	}
	return out;
}

size_t Indent(const std::string &s) {
	size_t n = 0;
	for (char c : s) {
		if (c == ' ') {
			n++;
		} else if (c == '\t') {
			n += 4;
		} else {
			break;
		}
	}
	return n;
}

bool IsBlank(const std::string &s) {
	return Trim(s).empty();
}

bool IsRule(const std::string &s) {
	std::string t = Trim(s);
	if (t.size() < 3) {
		return false;
	}
	char c = t[0];
	if (c != '-' && c != '*' && c != '_') {
		return false;
	}
	for (char x : t) {
		if (x != c && x != ' ') {
			return false;
		}
	}
	return true;
}

// "- item" / "* item" / "+ item"
bool IsBullet(const std::string &s, std::string &content) {
	std::string t = s.substr(std::min(Indent(s), s.size()));
	if (t.size() < 2 || (t[0] != '-' && t[0] != '*' && t[0] != '+')) {
		return false;
	}
	if (t[1] != ' ' && t[1] != '\t') {
		return false;
	}
	if (IsRule(s)) {
		return false; // "- - -" is a rule, not a list
	}
	content = Trim(t.substr(2));
	return true;
}

// "1. item"
bool IsOrdered(const std::string &s, std::string &content) {
	std::string t = s.substr(std::min(Indent(s), s.size()));
	size_t i = 0;
	while (i < t.size() && isdigit((unsigned char)t[i])) {
		i++;
	}
	if (i == 0 || i > 9 || i + 1 >= t.size()) {
		return false;
	}
	if (t[i] != '.' && t[i] != ')') {
		return false;
	}
	if (t[i + 1] != ' ' && t[i + 1] != '\t') {
		return false;
	}
	content = Trim(t.substr(i + 2));
	return true;
}

bool IsFence(const std::string &s, std::string &info) {
	std::string t = Trim(s);
	if (t.size() < 3 || (t.compare(0, 3, "```") != 0 && t.compare(0, 3, "~~~") != 0)) {
		return false;
	}
	size_t i = 3;
	while (i < t.size() && t[i] == t[0]) {
		i++;
	}
	info = Trim(t.substr(i));
	return true;
}

bool IsTableRule(const std::string &s) {
	std::string t = Trim(s);
	if (t.find('|') == std::string::npos) {
		return false;
	}
	for (char c : t) {
		if (c != '|' && c != '-' && c != ':' && c != ' ') {
			return false;
		}
	}
	return t.find('-') != std::string::npos;
}

std::vector<std::string> TableCells(const std::string &line) {
	std::string t = Trim(line);
	if (!t.empty() && t.front() == '|') {
		t.erase(t.begin());
	}
	if (!t.empty() && t.back() == '|') {
		t.pop_back();
	}
	std::vector<std::string> cells;
	std::string cur;
	for (size_t i = 0; i < t.size(); i++) {
		if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
			cur += '|';
			i++;
			continue;
		}
		if (t[i] == '|') {
			cells.push_back(Trim(cur));
			cur.clear();
			continue;
		}
		cur += t[i];
	}
	cells.push_back(Trim(cur));
	return cells;
}

// Emphasis is the only inline construct that nests, and eight levels is more
// than any real page uses.
constexpr int kMaxInlineDepth = 8;

class Renderer {
public:
	explicit Renderer(const Options &opts) : opts_(opts) {
	}

	std::string Inline(const std::string &src) {
		std::string out;
		EmitInline(out, src, 0);
		return out;
	}

	std::string Block(const std::string &src) {
		lines_ = SplitLines(src.size() > opts_.max_input ? src.substr(0, opts_.max_input) : src);
		std::string out;
		Blocks(out, 0, lines_.size(), 0);
		if (src.size() > opts_.max_input) {
			out += "<p class=\"truncated\">(the rest of this page was too long to render)</p>\n";
		}
		return out;
	}

private:
	const Options &opts_;
	std::vector<std::string> lines_;

	bool Full(const std::string &out) const {
		return out.size() >= opts_.max_output;
	}

	// ---- inline ----------------------------------------------------------

	void EmitLink(std::string &out, const std::string &text, const std::string &url, int depth) {
		if (!SafeUrl(url)) {
			Escape(out, text);
			return;
		}
		out += "<a href=\"";
		Escape(out, url);
		out += "\">";
		EmitInline(out, text, depth);
		out += "</a>";
	}

	void EmitWikiLink(std::string &out, const std::string &target) {
		std::string page = target;
		std::string label = target;
		size_t bar = target.find('|');
		if (bar != std::string::npos) {
			page = Trim(target.substr(0, bar));
			label = Trim(target.substr(bar + 1));
		}
		std::string href = opts_.link ? opts_.link(page) : std::string();
		if (href.empty() || !SafeUrl(href)) {
			Escape(out, label);
			return;
		}
		const bool exists = opts_.exists ? opts_.exists(page) : true;
		out += "<a href=\"";
		Escape(out, href);
		out += "\"";
		if (!exists) {
			out += " class=\"";
			Escape(out, opts_.wanted_class);
			out += "\" title=\"This page does not exist yet\"";
		}
		out += ">";
		Escape(out, label);
		out += "</a>";
	}

	// Bare http(s) URLs become links. Bounded: it only ever scans forward.
	size_t Autolink(std::string &out, const std::string &s, size_t i) {
		size_t end = i;
		while (end < s.size() && !isspace((unsigned char)s[end]) && s[end] != '<' && s[end] != ')') {
			end++;
		}
		// Trailing punctuation is almost never part of the URL.
		while (end > i && strchr(".,;:!?", s[end - 1]) != nullptr) {
			end--;
		}
		const std::string url = s.substr(i, end - i);
		// Emitted directly rather than through EmitLink: the link text of an
		// autolink *is* the URL, and handing it back to the inline parser makes
		// it find the same URL again and recurse forever.
		if (SafeUrl(url)) {
			out += "<a href=\"";
			Escape(out, url);
			out += "\">";
			Escape(out, url);
			out += "</a>";
		} else {
			Escape(out, url);
		}
		return end;
	}

	void EmitInline(std::string &out, const std::string &s, int depth) {
		// Emphasis nests, so this recurses; "*****...*" would otherwise recurse
		// once per pair and take the stack with it. Past the cap the remainder
		// is emitted as text, which is what it looks like anyway.
		if (depth > kMaxInlineDepth) {
			Escape(out, s);
			return;
		}
		// Where the last closer of each kind is. A run of "[a](" with no ")"
		// anywhere would otherwise make every position pay a full scan to the
		// end of the string, which is quadratic in a page somebody typed.
		const size_t last_rbracket = s.rfind(']');
		const size_t last_rparen = s.rfind(')');
		const size_t last_tick = s.rfind('`');
		for (size_t i = 0; i < s.size();) {
			if (Full(out)) {
				return;
			}
			char c = s[i];

			// A backslash escapes the next punctuation character, so a page can
			// contain a literal asterisk or bracket.
			if (c == '\\' && i + 1 < s.size() && ispunct((unsigned char)s[i + 1])) {
				Escape(out, std::string(1, s[i + 1]));
				i += 2;
				continue;
			}

			if (c == '`' && last_tick != std::string::npos && i < last_tick) {
				size_t ticks = 0;
				while (i + ticks < s.size() && s[i + ticks] == '`') {
					ticks++;
				}
				std::string fence(ticks, '`');
				size_t close = s.find(fence, i + ticks);
				if (close != std::string::npos) {
					out += "<code>";
					Escape(out, s.substr(i + ticks, close - i - ticks));
					out += "</code>";
					i = close + ticks;
					continue;
				}
			}

			if (c == '[' && i + 1 < s.size() && s[i + 1] == '[' &&
			    last_rbracket != std::string::npos && i < last_rbracket) {
				size_t close = s.find("]]", i + 2);
				if (close != std::string::npos) {
					EmitWikiLink(out, Trim(s.substr(i + 2, close - i - 2)));
					i = close + 2;
					continue;
				}
			}

			if (c == '!' && i + 1 < s.size() && s[i + 1] == '[' &&
			    last_rbracket != std::string::npos && last_rparen != std::string::npos &&
			    i < last_rbracket && i < last_rparen) {
				size_t close = s.find(']', i + 2);
				if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == '(') {
					size_t paren = s.find(')', close + 2);
					if (paren != std::string::npos) {
						std::string alt = s.substr(i + 2, close - i - 2);
						std::string url = Trim(s.substr(close + 2, paren - close - 2));
						if (SafeUrl(url)) {
							out += "<img src=\"";
							Escape(out, url);
							out += "\" alt=\"";
							Escape(out, alt);
							out += "\">";
						} else {
							Escape(out, alt);
						}
						i = paren + 1;
						continue;
					}
				}
			}

			if (c == '[' && last_rbracket != std::string::npos &&
			    last_rparen != std::string::npos && i < last_rbracket && i < last_rparen) {
				size_t close = s.find(']', i + 1);
				if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == '(') {
					size_t paren = s.find(')', close + 2);
					if (paren != std::string::npos) {
						EmitLink(out, s.substr(i + 1, close - i - 1),
						         Trim(s.substr(close + 2, paren - close - 2)), depth + 1);
						i = paren + 1;
						continue;
					}
				}
			}

			if ((c == '*' || c == '_') && i + 1 < s.size()) {
				const bool strong = s[i + 1] == c;
				const std::string mark = strong ? std::string(2, c) : std::string(1, c);
				size_t from = i + mark.size();
				if (from < s.size() && !isspace((unsigned char)s[from])) {
					size_t close = s.find(mark, from);
					if (close != std::string::npos && close > from) {
						out += strong ? "<strong>" : "<em>";
						EmitInline(out, s.substr(from, close - from), depth + 1);
						out += strong ? "</strong>" : "</em>";
						i = close + mark.size();
						continue;
					}
				}
			}

			if ((c == 'h' && (s.compare(i, 7, "http://") == 0 || s.compare(i, 8, "https://") == 0)) &&
			    (i == 0 || isspace((unsigned char)s[i - 1]) || s[i - 1] == '(')) {
				i = Autolink(out, s, i);
				continue;
			}

			Escape(out, std::string(1, c));
			i++;
		}
	}

	// ---- blocks ----------------------------------------------------------

	void Paragraph(std::string &out, size_t from, size_t to) {
		bool any = false;
		for (size_t i = from; i < to; i++) {
			if (!Trim(lines_[i]).empty()) {
				any = true;
				break;
			}
		}
		if (!any) {
			return;
		}
		out += "<p>";
		for (size_t i = from; i < to; i++) {
			if (i > from) {
				// A line ending in two spaces is a hard break. It is emitted
				// here rather than folded into the text handed to EmitInline,
				// which would escape it into a visible "<br>".
				const std::string &prev = lines_[i - 1];
				out += (prev.size() >= 2 && prev.compare(prev.size() - 2, 2, "  ") == 0)
				           ? "<br>\n"
				           : "\n";
			}
			EmitInline(out, Trim(lines_[i]), 0);
		}
		out += "</p>\n";
	}

	void Table(std::string &out, size_t from, size_t to) {
		out += "<table>\n<thead><tr>";
		for (const std::string &c : TableCells(lines_[from])) {
			out += "<th>";
			EmitInline(out, c, 0);
			out += "</th>";
		}
		out += "</tr></thead>\n<tbody>\n";
		for (size_t i = from + 2; i < to; i++) {
			out += "<tr>";
			for (const std::string &c : TableCells(lines_[i])) {
				out += "<td>";
				EmitInline(out, c, 0);
				out += "</td>";
			}
			out += "</tr>\n";
		}
		out += "</tbody>\n</table>\n";
	}

	void Blocks(std::string &out, size_t from, size_t to, int depth) {
		if (depth > opts_.max_nesting) {
			for (size_t i = from; i < to; i++) {
				out += "<p>";
				Escape(out, lines_[i]);
				out += "</p>\n";
			}
			return;
		}
		size_t i = from;
		while (i < to && !Full(out)) {
			const std::string &line = lines_[i];

			if (IsBlank(line)) {
				i++;
				continue;
			}

			std::string info;
			if (IsFence(line, info)) {
				size_t j = i + 1;
				while (j < to) {
					std::string ignored;
					if (IsFence(lines_[j], ignored) && Trim(lines_[j]).find_first_not_of("`~") ==
					                                       std::string::npos) {
						break;
					}
					j++;
				}
				out += "<pre><code";
				if (!info.empty()) {
					out += " class=\"lang-";
					Escape(out, info);
					out += "\"";
				}
				out += ">";
				for (size_t k = i + 1; k < j && k < to; k++) {
					Escape(out, lines_[k]);
					out += "\n";
				}
				out += "</code></pre>\n";
				i = (j < to) ? j + 1 : to;
				continue;
			}

			if (IsRule(line)) {
				out += "<hr>\n";
				i++;
				continue;
			}

			if (line[Indent(line) == line.size() ? 0 : Indent(line)] == '#') {
				std::string t = Trim(line);
				size_t hashes = 0;
				while (hashes < t.size() && t[hashes] == '#') {
					hashes++;
				}
				if (hashes >= 1 && hashes <= 6 && hashes < t.size() && t[hashes] == ' ') {
					std::string text = Trim(t.substr(hashes));
					while (!text.empty() && text.back() == '#') {
						text.pop_back();
					}
					const std::string h = std::to_string(hashes);
					out += "<h" + h + ">";
					EmitInline(out, Trim(text), 0);
					out += "</h" + h + ">\n";
					i++;
					continue;
				}
			}

			if (Trim(line)[0] == '>') {
				size_t j = i;
				std::vector<std::string> inner;
				while (j < to && !IsBlank(lines_[j]) && !Trim(lines_[j]).empty() &&
				       Trim(lines_[j])[0] == '>') {
					std::string t = Trim(lines_[j]).substr(1);
					if (!t.empty() && t[0] == ' ') {
						t.erase(t.begin());
					}
					inner.push_back(t);
					j++;
				}
				out += "<blockquote>\n";
				Renderer sub(opts_);
				sub.lines_ = inner;
				sub.Blocks(out, 0, inner.size(), depth + 1);
				out += "</blockquote>\n";
				i = j;
				continue;
			}

			// An indented code block, but only where a list is not in play.
			if (Indent(line) >= 4) {
				size_t j = i;
				out += "<pre><code>";
				while (j < to && (Indent(lines_[j]) >= 4 || IsBlank(lines_[j]))) {
					if (IsBlank(lines_[j])) {
						// Trailing blanks belong to whatever follows.
						size_t k = j;
						while (k < to && IsBlank(lines_[k])) {
							k++;
						}
						if (k >= to || Indent(lines_[k]) < 4) {
							break;
						}
					}
					std::string t = lines_[j];
					out += Escaped(t.size() >= 4 ? t.substr(4) : std::string());
					out += "\n";
					j++;
				}
				out += "</code></pre>\n";
				i = j;
				continue;
			}

			std::string item;
			if (IsBullet(line, item) || IsOrdered(line, item)) {
				const bool ordered = !IsBullet(line, item) && IsOrdered(line, item);
				out += ordered ? "<ol>\n" : "<ul>\n";
				size_t j = i;
				while (j < to) {
					std::string content;
					const bool bullet = IsBullet(lines_[j], content);
					const bool number = !bullet && IsOrdered(lines_[j], content);
					if (!bullet && !number) {
						break;
					}
					if (bullet == ordered) {
						break; // a different list starts here
					}
					const size_t item_indent = Indent(lines_[j]);
					// Continuation and nested lines belong to this item.
					std::vector<std::string> sub_lines;
					size_t k = j + 1;
					while (k < to) {
						std::string ignored;
						const bool is_item = IsBullet(lines_[k], ignored) || IsOrdered(lines_[k], ignored);
						if (is_item && Indent(lines_[k]) <= item_indent) {
							break;
						}
						if (!is_item && !IsBlank(lines_[k]) && Indent(lines_[k]) <= item_indent) {
							break;
						}
						if (IsBlank(lines_[k])) {
							break;
						}
						const size_t strip = std::min(Indent(lines_[k]), item_indent + 2);
						sub_lines.push_back(lines_[k].substr(std::min(strip, lines_[k].size())));
						k++;
					}
					out += "<li>";
					EmitInline(out, content, 0);
					if (!sub_lines.empty()) {
						out += "\n";
						Renderer sub(opts_);
						sub.lines_ = sub_lines;
						sub.Blocks(out, 0, sub_lines.size(), depth + 1);
					}
					out += "</li>\n";
					j = k;
				}
				out += ordered ? "</ol>\n" : "</ul>\n";
				i = j;
				continue;
			}

			if (line.find('|') != std::string::npos && i + 1 < to && IsTableRule(lines_[i + 1])) {
				size_t j = i + 2;
				while (j < to && !IsBlank(lines_[j]) && lines_[j].find('|') != std::string::npos) {
					j++;
				}
				Table(out, i, j);
				i = j;
				continue;
			}

			size_t j = i;
			while (j < to && !IsBlank(lines_[j])) {
				std::string ignored;
				if (IsRule(lines_[j]) || IsFence(lines_[j], ignored)) {
					break;
				}
				if (j > i && (IsBullet(lines_[j], ignored) || IsOrdered(lines_[j], ignored))) {
					break;
				}
				if (j > i && !Trim(lines_[j]).empty() && Trim(lines_[j])[0] == '>') {
					break;
				}
				j++;
			}
			Paragraph(out, i, j);
			i = j;
		}
	}
};

} // namespace

std::string Render(const std::string &src, const Options &opts) {
	Renderer r(opts);
	return r.Block(src);
}

std::string RenderInline(const std::string &src, const Options &opts) {
	Renderer r(opts);
	return r.Inline(src);
}

} // namespace markdown
} // namespace quackmail
