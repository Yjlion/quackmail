#include "quackmail/html_sanitize.hpp"

#include "quackmail/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>
#include <vector>

namespace quackmail {
namespace html {

namespace {

bool IsNameChar(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}

// ---- a very small tag scanner --------------------------------------------
// Both sanitizers work on the same shape of input, so the scanning is shared:
// find `<`, find the matching `>`, pull out the element name and (for the
// allow-list) the attributes. Deliberately not a full HTML5 tokenizer — a
// tokenizer that is 95% right is more dangerous than one that is obviously
// conservative, so anything surprising is dropped rather than interpreted.

struct Tag {
	bool closing = false;
	bool self_closing = false;
	std::string name; // lower-cased
	std::string raw;  // the whole "<...>"
	std::string attrs_raw;
};

// Find the '>' that ends the tag starting at `i`, skipping any inside a quoted
// attribute value. Without this, `<a title="a>b">` truncates mid-tag.
size_t FindTagEnd(const std::string &in, size_t i) {
	char quote = 0;
	for (size_t p = i + 1; p < in.size(); p++) {
		char c = in[p];
		if (quote) {
			if (c == quote) {
				quote = 0;
			}
			continue;
		}
		if (c == '"' || c == '\'') {
			quote = c;
			continue;
		}
		if (c == '>') {
			return p;
		}
	}
	return std::string::npos;
}

bool ScanTag(const std::string &in, size_t i, size_t close, Tag &out) {
	out = Tag();
	out.raw = in.substr(i, close - i + 1);
	size_t p = i + 1;
	if (p < close && in[p] == '/') {
		out.closing = true;
		p++;
	}
	while (p <= close && IsNameChar(in[p])) {
		out.name += (char)std::tolower((unsigned char)in[p]);
		p++;
	}
	if (out.name.empty()) {
		return false;
	}
	out.attrs_raw = in.substr(p, close - p);
	std::string trimmed = out.attrs_raw;
	while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) {
		trimmed.pop_back();
	}
	if (!trimmed.empty() && trimmed.back() == '/') {
		out.self_closing = true;
	}
	return true;
}

// Parse `name="value"` pairs out of a tag's attribute run.
std::vector<std::pair<std::string, std::string>> ParseAttrs(const std::string &raw) {
	std::vector<std::pair<std::string, std::string>> out;
	size_t p = 0;
	while (p < raw.size()) {
		while (p < raw.size() && (std::isspace((unsigned char)raw[p]) || raw[p] == '/')) {
			p++;
		}
		std::string name;
		while (p < raw.size() && IsNameChar(raw[p])) {
			name += (char)std::tolower((unsigned char)raw[p]);
			p++;
		}
		if (name.empty()) {
			p++;
			continue;
		}
		while (p < raw.size() && std::isspace((unsigned char)raw[p])) {
			p++;
		}
		std::string value;
		if (p < raw.size() && raw[p] == '=') {
			p++;
			while (p < raw.size() && std::isspace((unsigned char)raw[p])) {
				p++;
			}
			if (p < raw.size() && (raw[p] == '"' || raw[p] == '\'')) {
				char q = raw[p++];
				while (p < raw.size() && raw[p] != q) {
					value += raw[p++];
				}
				p++;
			} else {
				while (p < raw.size() && !std::isspace((unsigned char)raw[p]) && raw[p] != '>') {
					value += raw[p++];
				}
			}
		}
		out.emplace_back(name, value);
	}
	return out;
}

std::string EscapeAttrValue(const std::string &in) {
	std::string out;
	for (char c : in) {
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '"':
			out += "&quot;";
			break;
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		default:
			out += c;
		}
	}
	return out;
}

// A URL scheme, lower-cased, with whitespace and control characters stripped
// first: `java\tscript:` and `java&#9;script:` are the classic evasions, and
// entity decoding has already happened by the time markup reaches a browser.
std::string SchemeOf(const std::string &url) {
	std::string cleaned;
	for (char c : url) {
		unsigned char u = (unsigned char)c;
		if (u <= 0x20 || u == 0x7f) {
			continue;
		}
		cleaned += (char)std::tolower(u);
	}
	size_t colon = cleaned.find(':');
	size_t slash = cleaned.find('/');
	// No colon before the first slash means it is a relative URL, which has no
	// scheme of its own.
	if (colon == std::string::npos || (slash != std::string::npos && slash < colon)) {
		return std::string();
	}
	return cleaned.substr(0, colon);
}

// ---- the compose allow-list ----------------------------------------------

bool ElementAllowed(const std::string &name) {
	static const char *kAllowed[] = {
	    "p",     "br",    "b",    "i",     "em",  "strong", "u",  "s",   "a",  "ul",
	    "ol",    "li",    "blockquote", "pre", "code", "span", "div", "table", "thead",
	    "tbody", "tr",    "td",   "th",    "img", "h1",     "h2", "h3",  "h4", "hr",
	};
	for (const char *a : kAllowed) {
		if (name == a) {
			return true;
		}
	}
	return false;
}

// Wrappers that are unwrapped rather than kept: a message body is a fragment,
// and a nested <html> or <body> inside one is at best meaningless. Their
// *contents* are still body text, so only the tag goes.
bool ElementUnwrapped(const std::string &name) {
	return name == "html" || name == "body";
}

// Elements whose content is not body text and must go with the tag. Dropping the
// tag alone would spill it into the message: a pasted whole document would show
// its <title> as a stray word before the first paragraph.
bool ElementDroppedWithContent(const std::string &name) {
	return name == "script" || name == "style" || name == "head" || name == "title" ||
	       name == "template" || name == "noscript";
}

// A `style` reduced to declarations that only affect appearance. `position`,
// `behavior`, `expression` and anything with a url() are exactly what a hostile
// paste uses, so the list is what is permitted rather than what is refused.
std::string SafeStyle(const std::string &in) {
	static const char *kProps[] = {"color",       "background-color", "font-weight", "font-style",
	                               "font-size",   "font-family",      "text-align",  "text-decoration",
	                               "margin",      "margin-left",      "margin-right", "margin-top",
	                               "margin-bottom", "padding",        "padding-left", "padding-right",
	                               "padding-top", "padding-bottom",   "border-left", "line-height"};
	std::string out;
	size_t p = 0;
	while (p < in.size()) {
		size_t semi = in.find(';', p);
		std::string decl = in.substr(p, semi == std::string::npos ? std::string::npos : semi - p);
		p = semi == std::string::npos ? in.size() : semi + 1;

		size_t colon = decl.find(':');
		if (colon == std::string::npos) {
			continue;
		}
		std::string prop = util::Lower(decl.substr(0, colon));
		std::string value = decl.substr(colon + 1);
		// Trim.
		auto trim = [](std::string &s) {
			size_t b = s.find_first_not_of(" \t\r\n");
			size_t e = s.find_last_not_of(" \t\r\n");
			s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
		};
		trim(prop);
		trim(value);
		if (prop.empty() || value.empty()) {
			continue;
		}
		std::string lower_value = util::Lower(value);
		// No url(), no expression(), no escapes to hide either in.
		if (lower_value.find("url(") != std::string::npos ||
		    lower_value.find("expression") != std::string::npos ||
		    lower_value.find('\\') != std::string::npos || lower_value.find('(') != std::string::npos) {
			continue;
		}
		bool ok = false;
		for (const char *allowed : kProps) {
			if (prop == allowed) {
				ok = true;
				break;
			}
		}
		if (!ok) {
			continue;
		}
		if (!out.empty()) {
			out += "; ";
		}
		out += prop + ": " + value;
	}
	return out;
}

bool AllDigits(const std::string &s) {
	if (s.empty()) {
		return false;
	}
	for (char c : s) {
		if (!std::isdigit((unsigned char)c)) {
			return false;
		}
	}
	return true;
}

// The attributes an element may keep, and what each has to look like.
std::string SafeAttrs(const std::string &element, const std::string &raw) {
	std::string out;
	for (auto &kv : ParseAttrs(raw)) {
		const std::string &name = kv.first;
		const std::string &value = kv.second;

		if (name == "href" && element == "a") {
			std::string scheme = SchemeOf(value);
			// A relative href in a mail body points nowhere useful, and an
			// unexpected scheme is the whole attack. Only these three.
			if (scheme != "http" && scheme != "https" && scheme != "mailto") {
				continue;
			}
			out += " href=\"" + EscapeAttrValue(value) + "\"";
			// A link out of a mail body should not be able to reach back into
			// the frame that opened it.
			out += " rel=\"noopener noreferrer\"";
			continue;
		}
		if (name == "src" && element == "img") {
			std::string scheme = SchemeOf(value);
			if (scheme == "cid") {
				out += " src=\"" + EscapeAttrValue(value) + "\"";
				continue;
			}
			// A data: image is self-contained and cannot phone home, but only
			// for real image types — data:image/svg+xml is scriptable.
			std::string lower = util::Lower(value);
			if (scheme == "data" &&
			    (lower.rfind("data:image/png", 0) == 0 || lower.rfind("data:image/jpeg", 0) == 0 ||
			     lower.rfind("data:image/gif", 0) == 0 || lower.rfind("data:image/webp", 0) == 0)) {
				out += " src=\"" + EscapeAttrValue(value) + "\"";
				continue;
			}
			// http(s) images are dropped rather than kept: an image loaded from
			// elsewhere is a tracking pixel, and the reader can still opt in to
			// remote images on the display side.
			continue;
		}
		if (name == "alt" || name == "title") {
			out += " " + name + "=\"" + EscapeAttrValue(value) + "\"";
			continue;
		}
		if ((name == "width" || name == "height") && AllDigits(value)) {
			out += " " + name + "=\"" + value + "\"";
			continue;
		}
		if (name == "style") {
			std::string safe = SafeStyle(value);
			if (!safe.empty()) {
				out += " style=\"" + EscapeAttrValue(safe) + "\"";
			}
			continue;
		}
		// Everything else, including every on* handler, class, id and any
		// attribute this code has never heard of.
	}
	return out;
}

} // namespace

// ---- display: the deny-list ----------------------------------------------

std::string SanitizeForDisplay(const std::string &in) {
	static const char *kDropWithContent[] = {"script", "style", "iframe", "object", "embed", "applet",
	                                         "form",   "frame", "frameset"};
	std::string out;
	out.reserve(in.size());
	size_t i = 0;
	while (i < in.size()) {
		if (in[i] != '<') {
			out += in[i++];
			continue;
		}
		size_t close = FindTagEnd(in, i);
		if (close == std::string::npos) {
			break; // a truncated tag: drop the remainder rather than guess
		}
		Tag tag;
		if (!ScanTag(in, i, close, tag)) {
			i = close + 1;
			continue;
		}
		std::string lower = util::Lower(tag.raw);

		bool drop = false;
		for (const char *bad : kDropWithContent) {
			if (tag.name == bad) {
				drop = true;
				break;
			}
		}
		if (tag.name == "link" || tag.name == "meta" || tag.name == "base") {
			drop = true;
		}
		if (drop) {
			// Skip the element's content too, for the ones that have any.
			if (!tag.closing && (tag.name == "script" || tag.name == "style")) {
				size_t search = in.find("</" + tag.name, close);
				i = search == std::string::npos ? in.size() : in.find('>', search);
				i = i == std::string::npos ? in.size() : i + 1;
			} else {
				i = close + 1;
			}
			continue;
		}
		// Event handlers and javascript:/vbscript: URLs, whatever element they
		// are on.
		if (lower.find(" on") != std::string::npos || lower.find("javascript:") != std::string::npos ||
		    lower.find("vbscript:") != std::string::npos) {
			// Keep the element, lose its attributes.
			out += "<" + std::string(tag.closing ? "/" : "") + tag.name + ">";
			i = close + 1;
			continue;
		}
		out += tag.raw;
		i = close + 1;
	}
	return out;
}

// ---- compose: the allow-list ---------------------------------------------

std::string SanitizeForCompose(const std::string &in) {
	// A stored message is not a place for an unbounded paste.
	const size_t kMaxBytes = 512 * 1024;
	std::string source = in.size() > kMaxBytes ? in.substr(0, kMaxBytes) : in;

	std::string out;
	out.reserve(source.size());
	size_t i = 0;
	while (i < source.size()) {
		if (source[i] != '<') {
			out += source[i++];
			continue;
		}
		// A comment can hide markup from a naive scanner; drop it whole.
		if (source.compare(i, 4, "<!--") == 0) {
			size_t end = source.find("-->", i + 4);
			i = (end == std::string::npos) ? source.size() : end + 3;
			continue;
		}
		size_t close = FindTagEnd(source, i);
		if (close == std::string::npos) {
			// An unterminated tag at the end: escape the '<' so it shows as text
			// rather than swallowing the rest of the message.
			out += "&lt;";
			i++;
			continue;
		}
		Tag tag;
		if (!ScanTag(source, i, close, tag)) {
			i = close + 1;
			continue;
		}

		if (ElementUnwrapped(tag.name)) {
			i = close + 1; // keep the contents, drop the wrapper
			continue;
		}
		if (ElementDroppedWithContent(tag.name)) {
			// Content too, or a script body — or a document's <title> — becomes
			// visible text in the message.
			if (!tag.closing) {
				size_t endtag = source.find("</" + tag.name, close);
				i = endtag == std::string::npos ? source.size() : source.find('>', endtag);
				i = i == std::string::npos ? source.size() : i + 1;
			} else {
				i = close + 1;
			}
			continue;
		}
		if (!ElementAllowed(tag.name)) {
			// Not on the list: drop the tag, keep whatever is inside it. A
			// <font> or <center> loses its formatting but not its words.
			i = close + 1;
			continue;
		}

		if (tag.closing) {
			out += "</" + tag.name + ">";
		} else {
			bool void_element = tag.name == "br" || tag.name == "img" || tag.name == "hr";
			out += "<" + tag.name + SafeAttrs(tag.name, tag.attrs_raw) + ">";
			(void)void_element;
		}
		i = close + 1;
	}
	return out;
}

// ---- cid: rewriting ------------------------------------------------------

std::string RewriteCidUrls(const std::string &in, const std::string &prefix) {
	std::string out;
	out.reserve(in.size());
	size_t i = 0;
	while (i < in.size()) {
		// Match `cid:` only where an attribute value could begin, so a literal
		// "cid:" in prose is left alone.
		size_t at = in.find("cid:", i);
		if (at == std::string::npos) {
			out += in.substr(i);
			break;
		}
		bool quoted_start = at > 0 && (in[at - 1] == '"' || in[at - 1] == '\'');
		if (!quoted_start) {
			out += in.substr(i, at + 4 - i);
			i = at + 4;
			continue;
		}
		char quote = in[at - 1];
		size_t end = in.find(quote, at);
		if (end == std::string::npos) {
			out += in.substr(i);
			break;
		}
		std::string id = in.substr(at + 4, end - at - 4);
		out += in.substr(i, at - i);
		out += prefix;
		// Percent-encode: the id came from a message and can contain anything.
		for (char c : id) {
			unsigned char u = (unsigned char)c;
			if (std::isalnum(u) || c == '-' || c == '.' || c == '_' || c == '~') {
				out += c;
			} else {
				char buf[4];
				std::snprintf(buf, sizeof buf, "%%%02X", u);
				out += buf;
			}
		}
		i = end;
	}
	return out;
}

std::string FirstCidReference(const std::string &in) {
	size_t at = in.find("cid:");
	if (at == std::string::npos) {
		return std::string();
	}
	size_t p = at + 4;
	std::string id;
	while (p < in.size() && in[p] != '"' && in[p] != '\'' && in[p] != '>' && in[p] != ' ') {
		id += in[p++];
	}
	return id;
}

// ---- plain-text rendering ------------------------------------------------

std::string ToPlainText(const std::string &in) {
	static const char *kBreakBefore[] = {"p",  "div", "br", "li", "tr", "h1",
	                                     "h2", "h3",  "h4", "hr", "blockquote", "pre"};
	std::string out;
	size_t i = 0;
	while (i < in.size()) {
		if (in[i] == '<') {
			size_t close = FindTagEnd(in, i);
			if (close == std::string::npos) {
				break;
			}
			Tag tag;
			if (ScanTag(in, i, close, tag)) {
				if (tag.name == "script" || tag.name == "style") {
					size_t endtag = in.find("</" + tag.name, close);
					i = endtag == std::string::npos ? in.size() : in.find('>', endtag);
					i = i == std::string::npos ? in.size() : i + 1;
					continue;
				}
				for (const char *b : kBreakBefore) {
					if (tag.name == b) {
						if (!out.empty() && out.back() != '\n') {
							out += "\n";
						}
						break;
					}
				}
				if (!tag.closing && tag.name == "li") {
					out += "- ";
				}
			}
			i = close + 1;
			continue;
		}
		if (in[i] == '&') {
			size_t semi = in.find(';', i);
			if (semi != std::string::npos && semi - i <= 8) {
				std::string ent = util::Lower(in.substr(i + 1, semi - i - 1));
				const char *rep = nullptr;
				if (ent == "amp") {
					rep = "&";
				} else if (ent == "lt") {
					rep = "<";
				} else if (ent == "gt") {
					rep = ">";
				} else if (ent == "quot") {
					rep = "\"";
				} else if (ent == "nbsp" || ent == "#160") {
					rep = " ";
				} else if (ent == "#39" || ent == "apos") {
					rep = "'";
				}
				if (rep) {
					out += rep;
					i = semi + 1;
					continue;
				}
			}
		}
		out += in[i++];
	}
	// Collapse the runs of blank lines that block elements leave behind.
	std::string tidy;
	int newlines = 0;
	for (char c : out) {
		if (c == '\n') {
			if (++newlines > 2) {
				continue;
			}
		} else if (c != '\r') {
			newlines = 0;
		}
		tidy += c;
	}
	return tidy;
}

} // namespace html
} // namespace quackmail
