#include "quackmail/xmlstream.hpp"

#include <cctype>

namespace quackmail {
namespace xmlstream {
namespace {

bool IsSpace(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Decode the five predefined entities plus numeric character references.
std::string Unescape(const std::string &in) {
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); i++) {
		if (in[i] != '&') {
			out += in[i];
			continue;
		}
		size_t semi = in.find(';', i);
		if (semi == std::string::npos || semi - i > 10) {
			out += in[i];
			continue;
		}
		std::string ent = in.substr(i + 1, semi - i - 1);
		if (ent == "lt") {
			out += '<';
		} else if (ent == "gt") {
			out += '>';
		} else if (ent == "amp") {
			out += '&';
		} else if (ent == "quot") {
			out += '"';
		} else if (ent == "apos") {
			out += '\'';
		} else if (!ent.empty() && ent[0] == '#') {
			long code = ent[1] == 'x' ? std::strtol(ent.c_str() + 2, nullptr, 16)
			                          : std::strtol(ent.c_str() + 1, nullptr, 10);
			// UTF-8 encode.
			if (code < 0x80) {
				out += (char)code;
			} else if (code < 0x800) {
				out += (char)(0xC0 | (code >> 6));
				out += (char)(0x80 | (code & 0x3F));
			} else {
				out += (char)(0xE0 | (code >> 12));
				out += (char)(0x80 | ((code >> 6) & 0x3F));
				out += (char)(0x80 | (code & 0x3F));
			}
		} else {
			out += in.substr(i, semi - i + 1); // unknown entity: pass through
		}
		i = semi;
	}
	return out;
}

} // namespace

std::string Event::Attr(const std::string &key) const {
	for (auto &a : attrs) {
		if (a.first == key) {
			return a.second;
		}
	}
	return "";
}

std::string Event::LocalName() const {
	size_t colon = name.find(':');
	return colon == std::string::npos ? name : name.substr(colon + 1);
}

bool Tokenizer::Next(Event &out) {
	// The closing half of a self-closing element, queued by the previous call.
	if (!pending_end_.empty()) {
		out = Event();
		out.kind = Event::END;
		out.name = pending_end_;
		pending_end_.clear();
		return true;
	}

	// Drop fully consumed input so the buffer does not grow without bound.
	if (pos_ > 0 && pos_ == buf_.size()) {
		buf_.clear();
		pos_ = 0;
	}
	if (pos_ >= buf_.size()) {
		return false;
	}

	// A CDATA section is character data with no escaping inside it. RSS and
	// Atom wrap HTML payloads in one constantly, and without this the generic
	// tag scan below would stop at the first '>' *inside* that HTML and produce
	// nonsense. Emitted verbatim: the whole point of CDATA is that its contents
	// are not entity-decoded.
	if (buf_.compare(pos_, 9, "<![CDATA[") == 0) {
		size_t end = buf_.find("]]>", pos_ + 9);
		if (end == std::string::npos) {
			return false; // not complete yet
		}
		out = Event();
		out.kind = Event::TEXT;
		out.text = buf_.substr(pos_ + 9, end - pos_ - 9);
		pos_ = end + 3;
		return true;
	}

	if (buf_[pos_] != '<') {
		size_t lt = buf_.find('<', pos_);
		// Without a following '<' the text may still be incomplete; wait unless
		// there is clearly nothing more coming.
		if (lt == std::string::npos) {
			return false;
		}
		std::string text = buf_.substr(pos_, lt - pos_);
		pos_ = lt;
		out = Event();
		out.kind = Event::TEXT;
		out.text = Unescape(text);
		return true;
	}

	// Declarations and comments carry no events.
	if (buf_.compare(pos_, 2, "<?") == 0) {
		size_t end = buf_.find("?>", pos_);
		if (end == std::string::npos) {
			return false;
		}
		pos_ = end + 2;
		return Next(out);
	}
	if (buf_.compare(pos_, 4, "<!--") == 0) {
		size_t end = buf_.find("-->", pos_);
		if (end == std::string::npos) {
			return false;
		}
		pos_ = end + 3;
		return Next(out);
	}

	// Find the '>' that closes this tag, ignoring those inside quoted values.
	size_t i = pos_ + 1;
	char quote = 0;
	for (; i < buf_.size(); i++) {
		char c = buf_[i];
		if (quote) {
			if (c == quote) {
				quote = 0;
			}
		} else if (c == '"' || c == '\'') {
			quote = c;
		} else if (c == '>') {
			break;
		}
	}
	if (i >= buf_.size()) {
		return false; // tag not complete yet
	}

	std::string tag = buf_.substr(pos_ + 1, i - pos_ - 1);
	pos_ = i + 1;

	out = Event();
	if (!tag.empty() && tag[0] == '/') {
		out.kind = Event::END;
		size_t s = 1;
		while (s < tag.size() && IsSpace(tag[s])) {
			s++;
		}
		size_t e = s;
		while (e < tag.size() && !IsSpace(tag[e])) {
			e++;
		}
		out.name = tag.substr(s, e - s);
		return true;
	}

	bool self_closing = !tag.empty() && tag.back() == '/';
	if (self_closing) {
		tag.pop_back();
	}

	// Element name.
	size_t p = 0;
	while (p < tag.size() && IsSpace(tag[p])) {
		p++;
	}
	size_t name_end = p;
	while (name_end < tag.size() && !IsSpace(tag[name_end])) {
		name_end++;
	}
	out.kind = Event::START;
	out.name = tag.substr(p, name_end - p);
	p = name_end;

	// Attributes.
	while (p < tag.size()) {
		while (p < tag.size() && IsSpace(tag[p])) {
			p++;
		}
		if (p >= tag.size()) {
			break;
		}
		size_t key_start = p;
		while (p < tag.size() && tag[p] != '=' && !IsSpace(tag[p])) {
			p++;
		}
		std::string key = tag.substr(key_start, p - key_start);
		while (p < tag.size() && IsSpace(tag[p])) {
			p++;
		}
		std::string value;
		if (p < tag.size() && tag[p] == '=') {
			p++;
			while (p < tag.size() && IsSpace(tag[p])) {
				p++;
			}
			if (p < tag.size() && (tag[p] == '"' || tag[p] == '\'')) {
				char q = tag[p++];
				size_t val_start = p;
				while (p < tag.size() && tag[p] != q) {
					p++;
				}
				value = Unescape(tag.substr(val_start, p - val_start));
				if (p < tag.size()) {
					p++;
				}
			} else {
				size_t val_start = p;
				while (p < tag.size() && !IsSpace(tag[p])) {
					p++;
				}
				value = Unescape(tag.substr(val_start, p - val_start));
			}
		}
		if (!key.empty()) {
			out.attrs.emplace_back(key, value);
		}
	}

	if (self_closing) {
		pending_end_ = out.name;
	}
	return true;
}

std::string Escape(const std::string &in) {
	std::string out;
	out.reserve(in.size());
	for (char c : in) {
		switch (c) {
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		case '&':
			out += "&amp;";
			break;
		case '"':
			out += "&quot;";
			break;
		case '\'':
			out += "&apos;";
			break;
		default:
			out += c;
		}
	}
	return out;
}

} // namespace xmlstream
} // namespace quackmail
