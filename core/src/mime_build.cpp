#include "quackmail/mime.hpp"

#include "quackmail/util.hpp"

#include <algorithm>
#include <cctype>

namespace quackmail {
namespace mime {

BuildPart::BuildPart() {
}

namespace {

bool IEquals(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); i++) {
		if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) {
			return false;
		}
	}
	return true;
}

bool IsTextType(const std::string &type) {
	return util::Lower(type).rfind("text/", 0) == 0;
}

// Strip anything that could break out of a header value. Callers pass subjects
// and filenames that came from a form or from inbound mail.
std::string HeaderSafe(const std::string &in) {
	std::string out;
	out.reserve(in.size());
	for (char c : in) {
		if (c == '\r' || c == '\n') {
			continue;
		}
		out += c;
	}
	return out;
}

// A filename safe to put inside a quoted header parameter. Deliberately not
// http::SanitizeFilename, which exists for Content-Disposition on the way *out*
// of the web server: MIME must not depend on the HTTP layer, and the rule here
// is narrower — no path separators, no quotes or backslashes to end the quoted
// string early, no control characters.
std::string ParamFilename(const std::string &name) {
	std::string out;
	for (char c : name) {
		unsigned char u = (unsigned char)c;
		if (c == '/' || c == '\\' || c == '"' || u < 0x20 || u == 0x7f) {
			continue;
		}
		out += c;
	}
	// Leading dots would make a hidden file, and "" has to become something.
	while (!out.empty() && out.front() == '.') {
		out.erase(0, 1);
	}
	return out.empty() ? "attachment" : out;
}

// Normalize to CRLF and ensure a trailing one, which is what every part body
// needs before a boundary line follows it.
std::string CrlfBody(const std::string &in) {
	std::string out;
	out.reserve(in.size() + 16);
	for (size_t i = 0; i < in.size(); i++) {
		if (in[i] == '\r') {
			continue;
		}
		if (in[i] == '\n') {
			out += "\r\n";
			continue;
		}
		out += in[i];
	}
	if (out.size() < 2 || out.compare(out.size() - 2, 2, "\r\n") != 0) {
		out += "\r\n";
	}
	return out;
}

std::string Base64Wrapped(const std::string &raw) {
	std::string b64 = util::Base64Encode(raw);
	std::string out;
	for (size_t i = 0; i < b64.size(); i += 76) {
		out += b64.substr(i, 76);
		out += "\r\n";
	}
	if (out.empty()) {
		out = "\r\n";
	}
	return out;
}

// A boundary no part contains. Random first, then *checked*: a part whose bytes
// happen to include the delimiter would truncate the message there, and an
// attacker who controls an attachment controls those bytes.
std::string SafeBoundary(const std::vector<BuildPart> &parts) {
	for (int attempt = 0; attempt < 8; attempt++) {
		std::string candidate = "=_qc_" + util::RandomHex(16);
		bool collides = false;
		for (auto &p : parts) {
			if (p.content.find(candidate) != std::string::npos) {
				collides = true;
				break;
			}
		}
		if (!collides) {
			return candidate;
		}
	}
	// Eight 128-bit values all colliding means something is very wrong; a longer
	// one is still better than giving up and emitting a broken message.
	return "=_qc_" + util::RandomHex(32);
}

bool IsAttachment(const BuildPart &p) {
	if (IEquals(p.disposition, "attachment")) {
		return true;
	}
	if (IEquals(p.disposition, "inline")) {
		return false;
	}
	// No explicit disposition: a filename means attachment, a Content-ID means
	// inline, and text with neither is body.
	if (!p.filename.empty()) {
		return true;
	}
	return false;
}

bool IsInlineRelated(const BuildPart &p) {
	return !p.content_id.empty() && !IsAttachment(p);
}

// Serialize one leaf part: its headers, a blank line, then its encoded body.
std::string EmitPart(const BuildPart &p) {
	std::string out;
	std::string type = p.content_type.empty() ? "application/octet-stream" : p.content_type;
	out += "Content-Type: " + HeaderSafe(type);
	if (IsTextType(type) && !p.charset.empty()) {
		out += "; charset=" + HeaderSafe(p.charset);
	}
	if (!p.filename.empty()) {
		out += "; name=\"" + ParamFilename(p.filename) + "\"";
	}
	out += "\r\n";

	std::string encoding = ChooseEncoding(p);
	out += "Content-Transfer-Encoding: " + encoding + "\r\n";

	if (!p.content_id.empty()) {
		// Angle brackets are part of the header syntax, not the id.
		std::string id = HeaderSafe(p.content_id);
		if (id.front() != '<') {
			id = "<" + id + ">";
		}
		out += "Content-ID: " + id + "\r\n";
	}
	if (!p.filename.empty()) {
		out += "Content-Disposition: " + std::string(IsAttachment(p) ? "attachment" : "inline") +
		       "; filename=\"" + ParamFilename(p.filename) + "\"\r\n";
	} else if (IEquals(p.disposition, "inline") || !p.content_id.empty()) {
		out += "Content-Disposition: inline\r\n";
	}
	out += "\r\n";

	if (encoding == "base64") {
		out += Base64Wrapped(p.content);
	} else if (encoding == "quoted-printable") {
		out += CrlfBody(EncodeQuotedPrintable(p.content));
	} else {
		out += CrlfBody(p.content);
	}
	return out;
}

// Wrap `children` (already-serialized parts) in a multipart of `subtype`.
std::string EmitMultipart(const std::string &subtype, const std::string &boundary,
                          const std::vector<std::string> &children, const std::string &extra_params) {
	std::string out;
	out += "Content-Type: multipart/" + subtype + "; boundary=\"" + boundary + "\"" + extra_params +
	       "\r\n\r\n";
	// A preamble for clients that cannot read multipart at all. Never shown by
	// one that can.
	out += "This is a multi-part message in MIME format.\r\n";
	for (auto &child : children) {
		out += "\r\n--" + boundary + "\r\n";
		out += child;
	}
	out += "\r\n--" + boundary + "--\r\n";
	return out;
}

} // namespace

std::string ChooseEncoding(const BuildPart &part) {
	std::string type = part.content_type.empty() ? "application/octet-stream" : part.content_type;
	// A message/* part carries a whole message, and RFC 2046 §5.2.1 allows it
	// only 7bit, 8bit or binary — base64 or quoted-printable around one is
	// illegal, and the clients that do accept it are the ones being generous.
	// This is what makes "forward as attachment" produce something another
	// client will open rather than offer to download.
	if (util::Lower(type).rfind("message/", 0) == 0) {
		return "8bit";
	}
	if (!IsTextType(type)) {
		return "base64";
	}
	// Text: 8bit is the cheapest and keeps the message readable in a raw dump,
	// but only while every line is short enough and there are no stray control
	// characters. SMTP allows 998 octets per line; 900 leaves room.
	size_t line = 0;
	for (char c : part.content) {
		if (c == '\n') {
			line = 0;
			continue;
		}
		if (c == '\r') {
			continue;
		}
		unsigned char u = (unsigned char)c;
		if (u < 0x09 || (u > 0x0d && u < 0x20) || u == 0x7f) {
			return "quoted-printable";
		}
		if (++line > 900) {
			return "quoted-printable";
		}
	}
	return "8bit";
}

std::string BuildMessage(const HeaderList &headers, const std::vector<BuildPart> &parts) {
	// Sort the parts into the three roles the nesting is built from.
	std::vector<const BuildPart *> body;     // text/plain, text/html
	std::vector<const BuildPart *> inlines;  // cid:-referenced images
	std::vector<const BuildPart *> attached; // everything else
	for (auto &p : parts) {
		if (p.content.empty() && p.filename.empty()) {
			continue; // nothing to say
		}
		if (IsAttachment(p)) {
			attached.push_back(&p);
		} else if (IsInlineRelated(p)) {
			inlines.push_back(&p);
		} else {
			body.push_back(&p);
		}
	}

	// text/plain before text/html: multipart/alternative is least-preferred
	// first, and a client shows the last part it understands.
	std::stable_sort(body.begin(), body.end(), [](const BuildPart *a, const BuildPart *b) {
		auto rank = [](const BuildPart *p) {
			std::string t = util::Lower(p->content_type);
			if (t.rfind("text/plain", 0) == 0) {
				return 0;
			}
			if (t.rfind("text/html", 0) == 0) {
				return 1;
			}
			return 2;
		};
		return rank(a) < rank(b);
	});

	std::string head;
	for (auto &h : headers) {
		// The framing headers are ours; a caller that also set them would only
		// be able to disagree with what is actually emitted.
		if (IEquals(h.first, "Content-Type") || IEquals(h.first, "Content-Transfer-Encoding") ||
		    IEquals(h.first, "MIME-Version")) {
			continue;
		}
		head += HeaderSafe(h.first) + ": " + HeaderSafe(h.second) + "\r\n";
	}
	head += "MIME-Version: 1.0\r\n";

	// An empty message still has to be a message.
	if (body.empty() && inlines.empty() && attached.empty()) {
		BuildPart empty;
		empty.content_type = "text/plain";
		return head + "Content-Type: text/plain; charset=utf-8\r\n"
		              "Content-Transfer-Encoding: 8bit\r\n\r\n\r\n";
	}

	// Innermost: the body, alone or as an alternative set.
	std::string inner;
	if (body.size() == 1) {
		inner = EmitPart(*body[0]);
	} else if (body.size() > 1) {
		std::vector<std::string> children;
		for (auto *p : body) {
			children.push_back(EmitPart(*p));
		}
		inner = EmitMultipart("alternative", SafeBoundary(parts), children, "");
	}

	// Then the inline parts the HTML refers to by cid:.
	if (!inlines.empty()) {
		std::vector<std::string> children;
		if (!inner.empty()) {
			children.push_back(inner);
		}
		for (auto *p : inlines) {
			children.push_back(EmitPart(*p));
		}
		// type="..." tells a client which child is the root of the related set.
		std::string root = body.empty() ? "" : "; type=\"" + util::Lower(body.back()->content_type) + "\"";
		inner = EmitMultipart("related", SafeBoundary(parts), children, root);
	}

	// Outermost: attachments.
	if (!attached.empty()) {
		std::vector<std::string> children;
		if (!inner.empty()) {
			children.push_back(inner);
		}
		for (auto *p : attached) {
			children.push_back(EmitPart(*p));
		}
		inner = EmitMultipart("mixed", SafeBoundary(parts), children, "");
	}

	return head + inner;
}

} // namespace mime
} // namespace quackmail
