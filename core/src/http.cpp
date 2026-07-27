#include "quackmail/http.hpp"

#include "quackmail/mime.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace quackmail {
namespace http {

// Out-of-line constructors: the extension build treats a struct with default
// member initializers as a non-aggregate, so brace-init needs a real one.
Request::Request() {
}
Response::Response() {
}
Limits::Limits() {
}
Cookie::Cookie() {
}
FormFile::FormFile() {
}

namespace {

bool IEquals(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); i++) {
		char ca = a[i], cb = b[i];
		if (ca >= 'A' && ca <= 'Z') {
			ca = char(ca - 'A' + 'a');
		}
		if (cb >= 'A' && cb <= 'Z') {
			cb = char(cb - 'A' + 'a');
		}
		if (ca != cb) {
			return false;
		}
	}
	return true;
}

std::string Trim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t')) {
		b++;
	}
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) {
		e--;
	}
	return s.substr(b, e - b);
}

int HexVal(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

// A header field name must be an RFC 7230 token: no spaces, no controls. This
// is what stops "Content-Length : 5" and similar smuggling tricks.
bool IsToken(const std::string &s) {
	if (s.empty()) {
		return false;
	}
	static const char *kExtra = "!#$%&'*+-.^_`|~";
	for (unsigned char c : s) {
		bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		if (!alnum && !std::strchr(kExtra, (char)c)) {
			return false;
		}
	}
	return true;
}

enum class LineStatus { Ok, Eof, TooLong };

// Read one CRLF/LF-terminated line with a hard cap, distinguishing "peer closed"
// from "line too long" — ClientStream::ReadLine collapses both into false, and
// the two need different status codes (408/EOF vs 414/431).
LineStatus ReadLimitedLine(net::ClientStream &stream, size_t max_len, std::string &line) {
	line.clear();
	char c = 0;
	while (true) {
		if (!stream.ReadByte(c)) {
			return LineStatus::Eof;
		}
		if (c == '\n') {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			return LineStatus::Ok;
		}
		if (line.size() >= max_len) {
			return LineStatus::TooLong;
		}
		line += c;
	}
}

int64_t ElapsedMs(const std::chrono::steady_clock::time_point &start) {
	auto now = std::chrono::steady_clock::now();
	return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
}

std::string HttpDate() {
	std::time_t now = std::time(nullptr);
	struct tm tm {};
	gmtime_r(&now, &tm);
	char buf[64];
	// RFC 7231 IMF-fixdate. Explicit tables keep it locale-independent.
	static const char *kDay[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char *kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	std::snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT", kDay[tm.tm_wday % 7], tm.tm_mday,
	              kMon[tm.tm_mon % 12], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
	return buf;
}

std::string MimeHeader(const std::vector<std::pair<std::string, std::string>> &headers,
                       const std::string &name) {
	for (auto &h : headers) {
		if (IEquals(h.first, name)) {
			return h.second;
		}
	}
	return std::string();
}

} // namespace

// ---- Request ---------------------------------------------------------------

std::string Request::Header(const std::string &name) const {
	std::string out;
	for (auto &h : headers) {
		if (IEquals(h.first, name)) {
			if (!out.empty()) {
				out += ", ";
			}
			out += h.second;
		}
	}
	return out;
}

bool Request::HasHeader(const std::string &name) const {
	for (auto &h : headers) {
		if (IEquals(h.first, name)) {
			return true;
		}
	}
	return false;
}

std::string Request::Param(const std::string &name) const {
	for (auto &kv : ParseUrlEncoded(query)) {
		if (kv.first == name) {
			return kv.second;
		}
	}
	return std::string();
}

std::string Request::Form(const std::string &name) const {
	for (auto &kv : ParseUrlEncoded(body)) {
		if (kv.first == name) {
			return kv.second;
		}
	}
	return std::string();
}

std::vector<std::string> Request::FormAll(const std::string &name) const {
	std::vector<std::string> out;
	for (auto &kv : ParseUrlEncoded(body)) {
		if (kv.first == name) {
			out.push_back(kv.second);
		}
	}
	return out;
}

bool Request::HasForm(const std::string &name) const {
	for (auto &kv : ParseUrlEncoded(body)) {
		if (kv.first == name) {
			return true;
		}
	}
	return false;
}

// ---- Response --------------------------------------------------------------

void Response::SetHeader(const std::string &name, const std::string &value) {
	// "Set" means exactly one value survives: overwrite the first match and drop
	// any later duplicates, so a handler cannot accidentally emit two
	// Content-Types or two CSPs.
	bool set = false;
	for (auto it = headers.begin(); it != headers.end();) {
		if (!IEquals(it->first, name)) {
			++it;
		} else if (!set) {
			it->second = value;
			set = true;
			++it;
		} else {
			it = headers.erase(it);
		}
	}
	if (!set) {
		headers.emplace_back(name, value);
	}
}

void Response::AddHeader(const std::string &name, const std::string &value) {
	headers.emplace_back(name, value);
}

void Response::Html(const std::string &html, int s) {
	status = s;
	body = html;
	SetHeader("Content-Type", "text/html; charset=utf-8");
}

void Response::Text(const std::string &text, int s) {
	status = s;
	body = text;
	SetHeader("Content-Type", "text/plain; charset=utf-8");
}

void Response::Bytes(const std::string &data, const std::string &content_type) {
	body = data;
	SetHeader("Content-Type", content_type);
}

void Response::Redirect(const std::string &location, int s) {
	status = s;
	SetHeader("Location", location);
	body.clear();
}

// ---- request reading -------------------------------------------------------

int StatusForReadResult(ReadResult r) {
	switch (r) {
	case ReadResult::Ok:
		return 200;
	case ReadResult::UriTooLong:
		return 414;
	case ReadResult::HeadersTooLarge:
		return 431;
	case ReadResult::BodyTooLarge:
		return 413;
	case ReadResult::LengthRequired:
		return 411;
	case ReadResult::Timeout:
		return 408;
	case ReadResult::NotImplemented:
		return 501;
	case ReadResult::Eof:
	case ReadResult::BadRequest:
	default:
		return 400;
	}
}

ReadResult ReadRequest(net::ClientStream &stream, const Limits &limits, Request &out) {
	out = Request();
	out.peer_ip = stream.PeerIp();
	out.tls = stream.IsTls();

	// A socket timeout bounds each individual read; the wall clock below bounds
	// the request as a whole, so a peer dribbling one byte per timeout period
	// still cannot hold this thread.
	stream.SetTimeouts(limits.header_deadline_ms, limits.header_deadline_ms);
	auto start = std::chrono::steady_clock::now();

	std::string line;
	auto st = ReadLimitedLine(stream, limits.max_request_line, line);
	if (st == LineStatus::TooLong) {
		return ReadResult::UriTooLong;
	}
	if (st == LineStatus::Eof) {
		return line.empty() ? ReadResult::Eof : ReadResult::BadRequest;
	}
	// Tolerate leading empty lines, which RFC 7230 allows before a request line.
	int skipped = 0;
	while (line.empty() && skipped++ < 4) {
		st = ReadLimitedLine(stream, limits.max_request_line, line);
		if (st != LineStatus::Ok) {
			return st == LineStatus::TooLong ? ReadResult::UriTooLong : ReadResult::Eof;
		}
	}

	size_t sp1 = line.find(' ');
	if (sp1 == std::string::npos) {
		return ReadResult::BadRequest; // HTTP/0.9 or garbage
	}
	size_t sp2 = line.find(' ', sp1 + 1);
	if (sp2 == std::string::npos) {
		return ReadResult::BadRequest;
	}
	out.method = util::Upper(line.substr(0, sp1));
	out.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
	out.version = line.substr(sp2 + 1);
	if (!IsToken(out.method) || out.target.empty()) {
		return ReadResult::BadRequest;
	}
	if (out.version.rfind("HTTP/1.", 0) != 0) {
		return ReadResult::BadRequest;
	}

	// absolute-form ("GET http://host/path HTTP/1.1") is legal; keep only the
	// path so routing never has to think about it.
	std::string target = out.target;
	for (const char *scheme : {"http://", "https://"}) {
		size_t n = std::strlen(scheme);
		if (target.size() > n && IEquals(target.substr(0, n), scheme)) {
			size_t slash = target.find('/', n);
			target = (slash == std::string::npos) ? "/" : target.substr(slash);
			break;
		}
	}
	if (target[0] != '/') {
		return ReadResult::BadRequest;
	}
	size_t q = target.find('?');
	std::string raw_path = target;
	if (q != std::string::npos) {
		raw_path = target.substr(0, q);
		out.query = target.substr(q + 1);
	}
	if (!NormalizePath(PercentDecode(raw_path, false), out.path)) {
		return ReadResult::BadRequest;
	}

	// ---- headers ----
	size_t header_bytes = 0;
	while (true) {
		if (ElapsedMs(start) > limits.header_deadline_ms) {
			return ReadResult::Timeout;
		}
		std::string h;
		auto hs = ReadLimitedLine(stream, limits.max_header_bytes, h);
		if (hs == LineStatus::TooLong) {
			return ReadResult::HeadersTooLarge;
		}
		if (hs == LineStatus::Eof) {
			return ReadResult::BadRequest;
		}
		if (h.empty()) {
			break;
		}
		header_bytes += h.size() + 2;
		if (header_bytes > limits.max_header_bytes || out.headers.size() >= limits.max_headers) {
			return ReadResult::HeadersTooLarge;
		}
		// Obsolete line folding: a continuation line starts with SP/HTAB. RFC
		// 7230 lets a server reject it, and rejecting removes a whole class of
		// header-splitting ambiguity.
		if (h[0] == ' ' || h[0] == '\t') {
			return ReadResult::BadRequest;
		}
		size_t colon = h.find(':');
		if (colon == std::string::npos) {
			return ReadResult::BadRequest;
		}
		std::string name = h.substr(0, colon);
		if (!IsToken(name)) {
			return ReadResult::BadRequest;
		}
		out.headers.emplace_back(name, Trim(h.substr(colon + 1)));
	}

	// ---- body framing ----
	// We do not implement chunked encoding. Answering 411 rather than ignoring
	// the header is what keeps request smuggling impossible: there is never a
	// second opinion about where the body ends.
	if (out.HasHeader("Transfer-Encoding")) {
		return out.HasHeader("Content-Length") ? ReadResult::BadRequest : ReadResult::LengthRequired;
	}

	size_t content_length = 0;
	bool have_length = false;
	for (auto &h : out.headers) {
		if (!IEquals(h.first, "Content-Length")) {
			continue;
		}
		std::string v = Trim(h.second);
		if (v.empty()) {
			return ReadResult::BadRequest;
		}
		size_t n = 0;
		for (char c : v) {
			if (c < '0' || c > '9') {
				return ReadResult::BadRequest;
			}
			if (n > ((size_t)1 << 40)) {
				return ReadResult::BodyTooLarge;
			}
			n = n * 10 + (size_t)(c - '0');
		}
		// Duplicate Content-Length headers that disagree are the classic
		// smuggling vector; identical ones are merely redundant.
		if (have_length && n != content_length) {
			return ReadResult::BadRequest;
		}
		content_length = n;
		have_length = true;
	}

	if (!have_length || content_length == 0) {
		return ReadResult::Ok;
	}
	if (content_length > limits.max_body) {
		// Do not read it: answering immediately is the whole point of the cap.
		return ReadResult::BodyTooLarge;
	}

	// curl and several upload paths stall for a second if this goes unanswered.
	if (IEquals(Trim(out.Header("Expect")), "100-continue")) {
		stream.Write("HTTP/1.1 100 Continue\r\n\r\n");
	}

	stream.SetTimeouts(limits.body_deadline_ms, limits.body_deadline_ms);
	if (!stream.ReadN(out.body, content_length)) {
		return ReadResult::Timeout;
	}
	return ReadResult::Ok;
}

bool WriteResponse(net::ClientStream &stream, const Response &resp, bool head_only) {
	std::string head = "HTTP/1.1 " + std::to_string(resp.status) + " " + StatusText(resp.status) + "\r\n";
	bool have_type = false;
	for (auto &h : resp.headers) {
		if (IEquals(h.first, "Content-Length") || IEquals(h.first, "Connection") ||
		    IEquals(h.first, "Transfer-Encoding")) {
			continue; // framing is ours to decide, not a handler's
		}
		if (IEquals(h.first, "Content-Type")) {
			have_type = true;
		}
		// A header value containing CR or LF would inject a header; drop the
		// whole field rather than emitting something half-trusted.
		if (h.second.find('\r') != std::string::npos || h.second.find('\n') != std::string::npos) {
			continue;
		}
		head += h.first + ": " + h.second + "\r\n";
	}
	if (!have_type && !resp.body.empty()) {
		head += "Content-Type: text/plain; charset=utf-8\r\n";
	}
	head += "Date: " + HttpDate() + "\r\n";
	head += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
	head += "Connection: close\r\n\r\n";
	if (!stream.Write(head)) {
		return false;
	}
	if (head_only || resp.body.empty()) {
		return true;
	}
	return stream.Write(resp.body);
}

// ---- codecs ----------------------------------------------------------------

std::string PercentDecode(const std::string &in, bool plus_as_space) {
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); i++) {
		char c = in[i];
		if (plus_as_space && c == '+') {
			out += ' ';
		} else if (c == '%' && i + 2 < in.size()) {
			int hi = HexVal(in[i + 1]), lo = HexVal(in[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out += char(hi * 16 + lo);
				i += 2;
			} else {
				out += c; // not a valid escape: keep the literal '%'
			}
		} else {
			out += c;
		}
	}
	return out;
}

std::string PercentEncode(const std::string &in, const char *extra_safe) {
	static const char kHex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(in.size());
	for (unsigned char c : in) {
		bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		                  c == '-' || c == '.' || c == '_' || c == '~';
		if (unreserved || (extra_safe && *extra_safe && std::strchr(extra_safe, (char)c))) {
			out += char(c);
		} else {
			out += '%';
			out += kHex[c >> 4];
			out += kHex[c & 0x0F];
		}
	}
	return out;
}

std::vector<std::pair<std::string, std::string>> ParseUrlEncoded(const std::string &in) {
	std::vector<std::pair<std::string, std::string>> out;
	size_t pos = 0;
	while (pos <= in.size()) {
		size_t amp = in.find('&', pos);
		std::string field = in.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
		if (!field.empty()) {
			size_t eq = field.find('=');
			std::string k = eq == std::string::npos ? field : field.substr(0, eq);
			std::string v = eq == std::string::npos ? std::string() : field.substr(eq + 1);
			out.emplace_back(PercentDecode(k, true), PercentDecode(v, true));
		}
		if (amp == std::string::npos) {
			break;
		}
		pos = amp + 1;
	}
	return out;
}

std::string EscapeHtml(const std::string &in) {
	std::string out;
	out.reserve(in.size());
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
			// &#39; and not &apos; — the latter is not valid HTML4.
			out += "&#39;";
			break;
		default:
			out += c;
		}
	}
	return out;
}

std::string EscapeAttr(const std::string &in) {
	std::string out;
	out.reserve(in.size());
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
		case '`':
			out += "&#96;";
			break;
		case '=':
			out += "&#61;";
			break;
		default:
			out += c;
		}
	}
	return out;
}

bool NormalizePath(const std::string &decoded, std::string &out) {
	out.clear();
	if (decoded.empty() || decoded[0] != '/') {
		return false;
	}
	for (unsigned char c : decoded) {
		if (c < 0x20 || c == 0x7F) {
			return false; // NUL, CR, LF and friends never belong in a path
		}
	}
	std::vector<std::string> segs;
	size_t pos = 1;
	while (pos <= decoded.size()) {
		size_t slash = decoded.find('/', pos);
		std::string seg = decoded.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
		if (seg == "." || seg == "..") {
			// Neither is ever legitimate here, and rejecting outright is safer
			// than resolving: there is no traversal to get subtly wrong.
			return false;
		}
		if (!seg.empty()) {
			segs.push_back(seg);
		}
		if (slash == std::string::npos) {
			break;
		}
		pos = slash + 1;
	}
	if (segs.empty()) {
		out = "/";
		return true;
	}
	for (auto &s : segs) {
		out += "/";
		out += s;
	}
	// Preserve a meaningful trailing slash ("/mail/" is a route of its own).
	if (decoded.size() > 1 && decoded.back() == '/') {
		out += "/";
	}
	return true;
}

bool IsSafeRedirectTarget(const std::string &target) {
	if (target.size() < 1 || target[0] != '/') {
		return false;
	}
	if (target.size() >= 2 && (target[1] == '/' || target[1] == '\\')) {
		return false; // "//evil.example" is a scheme-relative absolute URL
	}
	for (unsigned char c : target) {
		if (c < 0x20 || c == 0x7F) {
			return false;
		}
	}
	return true;
}

// ---- cookies ---------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> ParseCookies(const std::string &cookie_header) {
	std::vector<std::pair<std::string, std::string>> out;
	size_t pos = 0;
	while (pos <= cookie_header.size()) {
		size_t semi = cookie_header.find(';', pos);
		std::string field =
		    cookie_header.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
		std::string trimmed = Trim(field);
		if (!trimmed.empty()) {
			size_t eq = trimmed.find('=');
			if (eq != std::string::npos) {
				std::string k = Trim(trimmed.substr(0, eq));
				std::string v = Trim(trimmed.substr(eq + 1));
				if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
					v = v.substr(1, v.size() - 2);
				}
				if (!k.empty()) {
					out.emplace_back(k, v);
				}
			}
		}
		if (semi == std::string::npos) {
			break;
		}
		pos = semi + 1;
	}
	return out;
}

std::string CookieValue(const std::string &cookie_header, const std::string &name) {
	for (auto &kv : ParseCookies(cookie_header)) {
		if (kv.first == name) {
			return kv.second;
		}
	}
	return std::string();
}

std::string SerializeCookie(const Cookie &c) {
	std::string out = c.name + "=" + c.value;
	if (!c.path.empty()) {
		out += "; Path=" + c.path;
	}
	if (c.max_age == 0) {
		// Expire it now, with a date too, for clients that ignore Max-Age.
		out += "; Max-Age=0; Expires=Thu, 01 Jan 1970 00:00:00 GMT";
	} else if (c.max_age > 0) {
		out += "; Max-Age=" + std::to_string(c.max_age);
	}
	if (c.http_only) {
		out += "; HttpOnly";
	}
	if (c.secure) {
		out += "; Secure";
	}
	if (!c.same_site.empty()) {
		out += "; SameSite=" + c.same_site;
	}
	return out;
}

// ---- multipart/form-data ---------------------------------------------------

bool ParseMultipart(const std::string &content_type, const std::string &body,
                    std::vector<std::pair<std::string, std::string>> &fields, std::vector<FormFile> &files) {
	fields.clear();
	files.clear();
	mime::ContentType ct = mime::ParseContentType(content_type);
	if (ct.type != "multipart" || ct.Param("boundary").empty()) {
		return false;
	}
	// mime::ParseEntity is a full RFC 2046 multipart splitter and is index-based
	// over std::string::find, so it is binary-safe. Give it a synthetic entity.
	mime::MimeEntity root = mime::ParseEntity("Content-Type: " + content_type + "\r\n\r\n" + body);
	for (auto &child : root.children) {
		// MimeEntity surfaces `filename` but not the form field `name`, so the
		// disposition has to be re-parsed here.
		mime::ContentType cd = mime::ParseContentType(MimeHeader(child.headers, "Content-Disposition"));
		std::string name = cd.Param("name");
		if (name.empty()) {
			continue;
		}
		std::string filename = cd.Param("filename");
		if (filename.empty()) {
			filename = child.filename;
		}
		if (filename.empty()) {
			fields.emplace_back(name, child.body_decoded);
		} else {
			FormFile f;
			f.field = name;
			f.filename = filename;
			f.content_type = child.content_type.Mime();
			f.content = child.body_decoded;
			files.push_back(std::move(f));
		}
	}
	return true;
}

std::string SanitizeFilename(const std::string &name) {
	std::string out;
	for (unsigned char c : name) {
		if (c < 0x20 || c == 0x7F || c == '"' || c == '\\' || c == '/' || c == ':') {
			continue;
		}
		out += char(c);
	}
	// A leading dot would hide the file; a bare ".." is meaningless here anyway.
	while (!out.empty() && out.front() == '.') {
		out.erase(0, 1);
	}
	if (out.size() > 200) {
		out.resize(200);
	}
	return out.empty() ? std::string("attachment") : out;
}

// ---- routing ---------------------------------------------------------------

bool MatchPath(const std::string &pattern, const std::string &path, std::vector<std::string> &captures) {
	captures.clear();
	auto split = [](const std::string &s) {
		std::vector<std::string> segs;
		size_t pos = 0;
		while (pos <= s.size()) {
			size_t slash = s.find('/', pos);
			std::string seg = s.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
			segs.push_back(seg);
			if (slash == std::string::npos) {
				break;
			}
			pos = slash + 1;
		}
		return segs;
	};
	auto ps = split(pattern);
	auto xs = split(path);
	for (size_t i = 0; i < ps.size(); i++) {
		if (ps[i] == "*") {
			// Wildcard tail: everything left, joined back together.
			std::string rest;
			for (size_t j = i; j < xs.size(); j++) {
				if (j > i) {
					rest += "/";
				}
				rest += xs[j];
			}
			captures.push_back(rest);
			return true;
		}
		if (i >= xs.size()) {
			return false;
		}
		if (!ps[i].empty() && ps[i][0] == ':') {
			if (xs[i].empty()) {
				return false;
			}
			captures.push_back(xs[i]);
			continue;
		}
		if (ps[i] != xs[i]) {
			return false;
		}
	}
	return ps.size() == xs.size();
}

const char *StatusText(int status) {
	switch (status) {
	case 100:
		return "Continue";
	case 200:
		return "OK";
	case 204:
		return "No Content";
	case 301:
		return "Moved Permanently";
	case 302:
		return "Found";
	case 303:
		return "See Other";
	case 304:
		return "Not Modified";
	case 400:
		return "Bad Request";
	case 401:
		return "Unauthorized";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 408:
		return "Request Timeout";
	case 411:
		return "Length Required";
	case 413:
		return "Payload Too Large";
	case 414:
		return "URI Too Long";
	case 429:
		return "Too Many Requests";
	case 431:
		return "Request Header Fields Too Large";
	case 500:
		return "Internal Server Error";
	case 501:
		return "Not Implemented";
	case 503:
		return "Service Unavailable";
	default:
		return "Unknown";
	}
}

} // namespace http
} // namespace quackmail
