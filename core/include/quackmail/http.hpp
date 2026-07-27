#pragma once

#include "quackmail/net.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace quackmail {
namespace http {

// A minimal HTTP/1.1 server codec: request reading, response writing, and the
// pure codecs (percent-encoding, form parsing, HTML escaping, cookies) that go
// with them. Transport-neutral by design — it takes a net::ClientStream and
// knows nothing about routing or pages, so the pure halves are unit-testable
// through SQL scalar functions with no socket in the loop.
//
// Deliberately not supported: chunked transfer encoding (browsers never chunk a
// form post; accepting it only opens the request-smuggling class) and
// persistent connections (see WriteResponse).

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Request {
	std::string method;  // upper-cased token ("GET")
	std::string target;  // request-target exactly as received
	std::string path;    // percent-decoded and normalized, no query
	std::string query;   // raw query string, still encoded
	std::string version; // "HTTP/1.1"
	Headers headers;     // order and case as received
	std::string body;
	std::string peer_ip;
	bool tls = false; // arrived over TLS (or a trusted proxy said so)

	Request();

	// Case-insensitive header lookup; "" when absent. Duplicate headers are
	// joined with ", " so a split value cannot be used to hide a directive.
	std::string Header(const std::string &name) const;
	bool HasHeader(const std::string &name) const;

	// Query-string and urlencoded-body accessors. Both return "" when absent;
	// FormAll returns every occurrence, for checkboxes and repeated fields.
	std::string Param(const std::string &name) const;
	std::string Form(const std::string &name) const;
	std::vector<std::string> FormAll(const std::string &name) const;
	bool HasForm(const std::string &name) const;
};

struct Response {
	int status = 200;
	Headers headers;
	std::string body;

	Response();

	void SetHeader(const std::string &name, const std::string &value); // replaces
	void AddHeader(const std::string &name, const std::string &value); // appends
	void Html(const std::string &html, int status = 200);
	void Text(const std::string &text, int status = 200);
	void Bytes(const std::string &data, const std::string &content_type);
	void Redirect(const std::string &location, int status = 303);
};

struct Limits {
	size_t max_request_line = 8192;
	size_t max_header_bytes = 16384;
	size_t max_headers = 64;
	size_t max_body = 10 * 1024 * 1024; // the attachment ceiling
	int header_deadline_ms = 15000;
	int body_deadline_ms = 60000;

	Limits();
};

enum class ReadResult {
	Ok,
	Eof,             // clean close before a request line
	BadRequest,      // 400
	UriTooLong,      // 414
	HeadersTooLarge, // 431
	BodyTooLarge,    // 413
	LengthRequired,  // 411 (chunked, which we do not accept)
	Timeout,         // 408
	NotImplemented,  // 501
};

// The status code a failed ReadRequest should be answered with.
int StatusForReadResult(ReadResult r);

// Read one request. Applies socket timeouts and an overall wall-clock deadline,
// answers `Expect: 100-continue`, and reads exactly Content-Length bytes.
ReadResult ReadRequest(net::ClientStream &stream, const Limits &limits, Request &out);

// Write a response and its body. Always emits `Connection: close`: with one
// thread per connection and no connection cap, an idle keep-alive socket costs
// a thread, and a page here is exactly one request because every asset is
// inlined. `head_only` suppresses the body but keeps Content-Length honest.
bool WriteResponse(net::ClientStream &stream, const Response &resp, bool head_only);

// ---- codecs -------------------------------------------------------------

// Percent-decoding. An invalid escape ("%zz", a truncated "%4") is passed
// through literally rather than dropped, so decoding never loses bytes.
std::string PercentDecode(const std::string &in, bool plus_as_space);
// Percent-encoding. Unreserved characters (RFC 3986) and anything in
// `extra_safe` pass through; everything else becomes %HH.
std::string PercentEncode(const std::string &in, const char *extra_safe = "");

// Parse "a=1&b=2" into pairs, percent-decoding both halves.
std::vector<std::pair<std::string, std::string>> ParseUrlEncoded(const std::string &in);

// HTML escaping. Emits &#39; rather than &apos; (which is not valid HTML4), and
// is deliberately separate from xmlstream::Escape so webmail's XSS posture is
// not coupled to XMPP stanza serialization.
std::string EscapeHtml(const std::string &in);
// As EscapeHtml, plus the characters that can terminate an unquoted attribute.
std::string EscapeAttr(const std::string &in);

// Normalize an already-percent-decoded path: require a leading '/', reject NUL,
// control characters and any "." or ".." segment, and collapse repeated
// slashes. Returns false if the path is unacceptable.
bool NormalizePath(const std::string &decoded, std::string &out);

// True for a safe same-site redirect target: exactly one leading '/', not "//"
// or "/\", no control characters. Check this *after* percent-decoding.
bool IsSafeRedirectTarget(const std::string &target);

// ---- cookies ------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> ParseCookies(const std::string &cookie_header);
std::string CookieValue(const std::string &cookie_header, const std::string &name);

struct Cookie {
	std::string name;
	std::string value;
	std::string path = "/";
	std::string same_site = "Lax";
	bool http_only = true;
	bool secure = false;
	int64_t max_age = -1; // < 0 = session cookie, 0 = delete now

	Cookie();
};

std::string SerializeCookie(const Cookie &c);

// ---- multipart/form-data ------------------------------------------------

struct FormFile {
	std::string field;
	std::string filename;
	std::string content_type;
	std::string content;

	FormFile();
};

// Parse a multipart/form-data body. `content_type` is the raw header value
// (boundary included). Returns false if it is not multipart or has no boundary.
bool ParseMultipart(const std::string &content_type, const std::string &body,
                    std::vector<std::pair<std::string, std::string>> &fields, std::vector<FormFile> &files);

// Strip a client-supplied filename down to something safe to put in a
// Content-Disposition header or on a disk: no path separators, no CR/LF, no
// quotes, no control characters. Returns "attachment" if nothing survives.
std::string SanitizeFilename(const std::string &name);

// ---- routing ------------------------------------------------------------

// Match a path against a pattern of literal segments, ":name" (one segment) and
// "*" (the rest, only as the final segment). Captures are appended in order.
bool MatchPath(const std::string &pattern, const std::string &path, std::vector<std::string> &captures);

const char *StatusText(int status);

} // namespace http
} // namespace quackmail
