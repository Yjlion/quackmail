#include "quackmail/mime.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace quackmail {
namespace mime {

static std::string LowerAscii(const std::string &s) {
	std::string out = s;
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
	return out;
}

static std::string Trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
		b++;
	}
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
		e--;
	}
	return s.substr(b, e - b);
}

// ---- ContentType accessors -------------------------------------------------

std::string ContentType::Param(const std::string &name) const {
	std::string want = LowerAscii(name);
	for (auto &p : params) {
		if (p.first == want) {
			return p.second;
		}
	}
	return "";
}

std::string ContentType::Mime() const {
	if (type.empty()) {
		return "";
	}
	return type + "/" + subtype;
}

// Strip RFC 822 comments (parenthesized, nestable) that sit outside quoted
// strings. Quoted strings are preserved verbatim.
static std::string StripComments(const std::string &in) {
	std::string out;
	int depth = 0;
	bool in_quote = false;
	for (size_t i = 0; i < in.size(); i++) {
		char c = in[i];
		if (in_quote) {
			out += c;
			if (c == '\\' && i + 1 < in.size()) {
				out += in[++i];
			} else if (c == '"') {
				in_quote = false;
			}
			continue;
		}
		if (depth > 0) {
			if (c == '\\' && i + 1 < in.size()) {
				i++;
			} else if (c == '(') {
				depth++;
			} else if (c == ')') {
				depth--;
			}
			continue;
		}
		if (c == '"') {
			in_quote = true;
			out += c;
		} else if (c == '(') {
			depth++;
		} else {
			out += c;
		}
	}
	return out;
}

// Split a header value on top-level ';' (respecting quoted strings).
static std::vector<std::string> SplitSemicolons(const std::string &in) {
	std::vector<std::string> out;
	std::string cur;
	bool in_quote = false;
	for (size_t i = 0; i < in.size(); i++) {
		char c = in[i];
		if (in_quote) {
			if (c == '\\' && i + 1 < in.size()) {
				cur += c;
				cur += in[++i];
				continue;
			}
			if (c == '"') {
				in_quote = false;
			}
			cur += c;
			continue;
		}
		if (c == '"') {
			in_quote = true;
			cur += c;
		} else if (c == ';') {
			out.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	out.push_back(cur);
	return out;
}

static std::string Unquote(const std::string &in) {
	std::string s = Trim(in);
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

ContentType ParseContentType(const std::string &value) {
	ContentType ct;
	std::string v = StripComments(value);
	auto fields = SplitSemicolons(v);
	if (fields.empty()) {
		return ct;
	}
	std::string token = Trim(fields[0]);
	auto slash = token.find('/');
	if (slash != std::string::npos) {
		ct.type = LowerAscii(Trim(token.substr(0, slash)));
		ct.subtype = LowerAscii(Trim(token.substr(slash + 1)));
	} else if (!token.empty()) {
		ct.type = LowerAscii(token);
	}
	for (size_t i = 1; i < fields.size(); i++) {
		std::string f = Trim(fields[i]);
		if (f.empty()) {
			continue;
		}
		auto eq = f.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		std::string name = LowerAscii(Trim(f.substr(0, eq)));
		std::string pval = Unquote(f.substr(eq + 1));
		if (!name.empty()) {
			ct.params.emplace_back(name, pval);
		}
	}
	return ct;
}

// ---- header/body splitting -------------------------------------------------

static void SplitHeadersBody(const std::string &raw, std::string &header_block, std::string &body) {
	size_t p = raw.find("\r\n\r\n");
	if (p != std::string::npos) {
		header_block = raw.substr(0, p);
		body = raw.substr(p + 4);
		return;
	}
	size_t q = raw.find("\n\n");
	if (q != std::string::npos) {
		header_block = raw.substr(0, q);
		body = raw.substr(q + 2);
		return;
	}
	header_block = raw;
	body.clear();
}

// Parse a header block into unfolded (name, value) pairs (case preserved).
static std::vector<std::pair<std::string, std::string>> ParseHeaderBlock(const std::string &header_block) {
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

	std::vector<std::pair<std::string, std::string>> headers;
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
		headers.emplace_back(name, value);
	}
	return headers;
}

static std::string HeaderValue(const std::vector<std::pair<std::string, std::string>> &headers, const std::string &name) {
	std::string want = LowerAscii(name);
	for (auto &h : headers) {
		if (LowerAscii(h.first) == want) {
			return h.second;
		}
	}
	return "";
}

// ---- multipart splitting (RFC 2046 §5.1.1) ---------------------------------

static std::vector<std::string> SplitMultipart(const std::string &body, const std::string &boundary) {
	std::vector<std::string> parts;
	std::string dash = "--" + boundary;

	std::vector<size_t> delim_pos;     // index of each delimiter's leading dash
	std::vector<size_t> content_start; // index where the following body-part begins
	std::vector<bool> is_close;

	size_t search = 0;
	while (search <= body.size()) {
		size_t p = body.find(dash, search);
		if (p == std::string::npos) {
			break;
		}
		bool at_line_start = (p == 0) || body[p - 1] == '\n';
		if (!at_line_start) {
			search = p + dash.size();
			continue;
		}
		size_t after = p + dash.size();
		bool close = false;
		if (after + 1 < body.size() && body[after] == '-' && body[after + 1] == '-') {
			close = true;
			after += 2;
		}
		// Skip transport padding to end of the delimiter line.
		size_t le = after;
		while (le < body.size() && body[le] != '\n') {
			le++;
		}
		delim_pos.push_back(p);
		is_close.push_back(close);
		content_start.push_back(le < body.size() ? le + 1 : body.size());
		if (close) {
			break;
		}
		search = content_start.back();
	}

	// A body-part sits between consecutive delimiters. The CRLF immediately
	// preceding a delimiter belongs to the boundary, not the content.
	for (size_t i = 0; i + 1 < delim_pos.size(); i++) {
		size_t cs = content_start[i];
		size_t ce = delim_pos[i + 1];
		if (ce > cs && body[ce - 1] == '\n') {
			ce--;
			if (ce > cs && body[ce - 1] == '\r') {
				ce--;
			}
		}
		parts.push_back(body.substr(cs, ce - cs));
	}
	return parts;
}

// ---- entity parsing --------------------------------------------------------

MimeEntity ParseEntity(const std::string &raw) {
	MimeEntity ent;
	std::string header_block, body;
	SplitHeadersBody(raw, header_block, body);
	ent.headers = ParseHeaderBlock(header_block);

	std::string ct_raw = HeaderValue(ent.headers, "Content-Type");
	if (ct_raw.empty()) {
		// RFC 2045/2046 default.
		ent.content_type.type = "text";
		ent.content_type.subtype = "plain";
		ent.charset = "us-ascii";
	} else {
		ent.content_type = ParseContentType(ct_raw);
		if (ent.content_type.type.empty()) {
			ent.content_type.type = "text";
			ent.content_type.subtype = "plain";
		}
		ent.charset = LowerAscii(ent.content_type.Param("charset"));
		if (ent.charset.empty() && ent.content_type.type == "text") {
			ent.charset = "us-ascii";
		}
	}

	std::string cte = Trim(HeaderValue(ent.headers, "Content-Transfer-Encoding"));
	ent.encoding = cte.empty() ? "7bit" : LowerAscii(cte);

	std::string cd_raw = HeaderValue(ent.headers, "Content-Disposition");
	std::string filename;
	if (!cd_raw.empty()) {
		ContentType cd = ParseContentType(cd_raw);
		ent.disposition = cd.type; // "inline" / "attachment"
		filename = cd.Param("filename");
	}
	if (filename.empty()) {
		filename = ent.content_type.Param("name");
	}
	ent.filename = filename;

	std::string cid = Trim(HeaderValue(ent.headers, "Content-ID"));
	if (cid.size() >= 2 && cid.front() == '<' && cid.back() == '>') {
		cid = cid.substr(1, cid.size() - 2);
	}
	ent.content_id = cid;

	if (ent.content_type.type == "multipart") {
		std::string boundary = ent.content_type.Param("boundary");
		if (!boundary.empty()) {
			for (auto &part : SplitMultipart(body, boundary)) {
				ent.children.push_back(ParseEntity(part));
			}
		}
		ent.body_raw = body;
		return ent;
	}

	if (ent.content_type.type == "message" && ent.content_type.subtype == "rfc822") {
		ent.body_raw = body;
		ent.body_decoded = DecodeContentTransferEncoding(ent.encoding, body);
		ent.children.push_back(ParseEntity(ent.body_decoded));
		return ent;
	}

	// Leaf part.
	ent.body_raw = body;
	ent.body_decoded = DecodeContentTransferEncoding(ent.encoding, body);
	return ent;
}

// ---- flattening (IMAP body section numbering) ------------------------------

static void Walk(const MimeEntity &ent, const std::string &section, std::vector<MimePart> &out) {
	MimePart part;
	part.section = section;
	part.content_type = ent.content_type.Mime();
	part.charset = ent.charset;
	part.encoding = ent.encoding;
	part.filename = ent.filename;
	part.content = ent.IsMultipart() ? "" : ent.body_decoded;
	part.size_bytes = static_cast<int64_t>(part.content.size());
	out.push_back(part);

	for (size_t i = 0; i < ent.children.size(); i++) {
		std::string child_section = section.empty() ? std::to_string(i + 1) : section + "." + std::to_string(i + 1);
		Walk(ent.children[i], child_section, out);
	}
}

std::vector<MimePart> FlattenParts(const MimeEntity &root) {
	std::vector<MimePart> out;
	if (!root.children.empty()) {
		// The top-level container itself is not a numbered part; number its
		// children 1..n (RFC 3501 §6.4.5).
		for (size_t i = 0; i < root.children.size(); i++) {
			Walk(root.children[i], std::to_string(i + 1), out);
		}
	} else {
		Walk(root, "1", out);
	}
	return out;
}

} // namespace mime
} // namespace quackmail
