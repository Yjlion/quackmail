#include "quackmail/http_client.hpp"

#include "quackmail/net.hpp"
#include "quackmail/tls.hpp"
#include "quackmail/util.hpp"

#include <cstdlib>
#include <memory>

namespace quackmail {
namespace httpc {

namespace {

std::string TrimWs(const std::string &s) {
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t')) {
		a++;
	}
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) {
		b--;
	}
	return s.substr(a, b - a);
}

// Read the status line and headers. Returns the status code, or -1.
int ReadHead(net::ClientStream &s, std::vector<std::pair<std::string, std::string>> &headers) {
	std::string line;
	if (!s.ReadLine(line, 8192)) {
		return -1;
	}
	auto sp = line.find(' ');
	if (sp == std::string::npos) {
		return -1;
	}
	int status = std::atoi(line.c_str() + sp + 1);
	while (s.ReadLine(line, 8192)) {
		if (line.empty() || line == "\r") {
			return status;
		}
		auto colon = line.find(':');
		if (colon == std::string::npos) {
			continue;
		}
		headers.emplace_back(util::Lower(TrimWs(line.substr(0, colon))), TrimWs(line.substr(colon + 1)));
	}
	return -1;
}

std::string Header(const std::vector<std::pair<std::string, std::string>> &headers,
                   const std::string &name) {
	for (const auto &h : headers) {
		if (h.first == name) {
			return h.second;
		}
	}
	return "";
}

// Read a chunked body. Feed servers do chunk, which is the whole reason this
// file exists rather than reusing the server codec.
bool ReadChunked(net::ClientStream &s, std::string &body, size_t max_bytes) {
	std::string line;
	while (s.ReadLine(line, 8192)) {
		// The size may carry chunk extensions after a ';'.
		auto semi = line.find(';');
		std::string sizetok = TrimWs(semi == std::string::npos ? line : line.substr(0, semi));
		size_t n = (size_t)std::strtoull(sizetok.c_str(), nullptr, 16);
		if (n == 0) {
			// Trailers, then a blank line.
			while (s.ReadLine(line, 8192) && !line.empty() && line != "\r") {
			}
			return true;
		}
		if (max_bytes > 0 && body.size() + n > max_bytes) {
			return false;
		}
		std::string chunk;
		if (!s.ReadN(chunk, n)) {
			return false;
		}
		body += chunk;
		if (!s.ReadLine(line, 8)) { // the CRLF after the chunk data
			return false;
		}
	}
	return false;
}

} // namespace

bool Url::Parse(const std::string &url) {
	auto sep = url.find("://");
	if (sep == std::string::npos) {
		return false;
	}
	scheme = util::Lower(url.substr(0, sep));
	if (scheme != "http" && scheme != "https") {
		return false;
	}
	std::string rest = url.substr(sep + 3);
	auto slash = rest.find('/');
	std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
	path = slash == std::string::npos ? "/" : rest.substr(slash);

	// Credentials in the URL are not supported; silently dropping them would
	// send an unauthenticated request that looks like it should have worked.
	if (authority.find('@') != std::string::npos) {
		return false;
	}
	port = scheme == "https" ? 443 : 80;
	if (!authority.empty() && authority[0] == '[') { // IPv6 literal
		auto close = authority.find(']');
		if (close == std::string::npos) {
			return false;
		}
		host = authority.substr(1, close - 1);
		if (close + 1 < authority.size() && authority[close + 1] == ':') {
			port = std::atoi(authority.c_str() + close + 2);
		}
	} else {
		auto colon = authority.find(':');
		if (colon == std::string::npos) {
			host = authority;
		} else {
			host = authority.substr(0, colon);
			port = std::atoi(authority.c_str() + colon + 1);
		}
	}
	return !host.empty() && port > 0;
}

std::string Response::Header(const std::string &name) const {
	const std::string want = util::Lower(name);
	for (const auto &h : headers) {
		if (h.first == want) {
			return h.second;
		}
	}
	return std::string();
}

Response Get(const std::string &url, const Options &opts) {
	return Request(url, opts);
}

Response Request(const std::string &url, const Options &opts) {
	Response res;
	std::string current = url;
	const std::string method = opts.method.empty() ? std::string("GET") : opts.method;
	const bool is_get = method == "GET";

	for (int hop = 0; hop <= opts.max_redirects; hop++) {
		Url u;
		if (!u.Parse(current)) {
			res.error = "cannot parse URL '" + current + "'";
			return res;
		}

		std::string err;
		int fd = net::Connect(u.host, u.port, opts.timeout_ms, err);
		if (fd < 0) {
			res.error = err;
			return res;
		}
		net::ClientStream s(fd);
		s.SetTimeouts(opts.timeout_ms, opts.timeout_ms);

		tls::ClientContext ctx;
		if (u.scheme == "https") {
			tls::ClientTlsConfig tls_config;
			tls_config.verify_peer = opts.verify_peer;
			tls_config.ca_bundle = opts.ca_bundle;
			if (!ctx.Init(tls_config, err) || !s.ConnectTls(ctx.Get(), u.host, err)) {
				res.error = "TLS handshake failed: " + err;
				return res;
			}
		}

		std::string req = method + " " + u.path + " HTTP/1.1\r\n";
		req += "Host: " + u.host + "\r\n";
		req += "User-Agent: " + opts.user_agent + "\r\n";
		req += "Accept: " +
		       (opts.accept.empty()
		            ? std::string("application/rss+xml, application/atom+xml, application/xml, "
		                          "text/xml, */*")
		            : opts.accept) +
		       "\r\n";
		if (!opts.content_type.empty()) {
			req += "Content-Type: " + opts.content_type + "\r\n";
		}
		for (const auto &h : opts.headers) {
			// A header value carrying CR or LF is a request-splitting attempt,
			// and there is no legitimate caller for one.
			if (h.first.find_first_of("\r\n:") != std::string::npos ||
			    h.second.find_first_of("\r\n") != std::string::npos) {
				res.error = "invalid request header";
				return res;
			}
			req += h.first + ": " + h.second + "\r\n";
		}
		if (!opts.body.empty() || !is_get) {
			req += "Content-Length: " + std::to_string(opts.body.size()) + "\r\n";
		}
		// Only on the first hop: a redirect target is a different resource, and
		// its cache validators are not the ones we hold.
		if (hop == 0 && !opts.etag.empty()) {
			req += "If-None-Match: " + opts.etag + "\r\n";
		}
		if (hop == 0 && !opts.last_modified.empty()) {
			req += "If-Modified-Since: " + opts.last_modified + "\r\n";
		}
		// One request per connection: keep-alive buys nothing for a poll that
		// happens once every fifteen minutes, and costs connection-reuse bugs.
		req += "Connection: close\r\n\r\n";
		req += opts.body;
		if (!s.Write(req)) {
			res.error = "could not send the request";
			return res;
		}

		std::vector<std::pair<std::string, std::string>> headers;
		int status = ReadHead(s, headers);
		if (status < 0) {
			res.error = "malformed response";
			return res;
		}
		res.status = status;

		// 304 shares the 3xx range with the redirects but is not one: it is the
		// success case for a conditional request, and has no Location to follow.
		res.headers = headers;

		if (status == 304) {
			res.etag = Header(headers, "etag");
			res.last_modified = Header(headers, "last-modified");
			res.ok = true; // nothing changed, and no body to read
			return res;
		}

		if (!is_get && (status == 301 || status == 302 || status == 303 || status == 307 ||
		                status == 308)) {
			// Deliberately not followed. Whether the body may be resent depends
			// on which of these it is, and no caller here needs the answer; the
			// Location is on the response for one that does.
			res.ok = true;
			return res;
		}

		if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
			std::string location = Header(headers, "location");
			if (location.empty() || hop == opts.max_redirects) {
				res.error = "too many redirects";
				return res;
			}
			if (location.compare(0, 4, "http") != 0) {
				// A relative Location; resolve against the current origin.
				std::string base = u.scheme + "://" + u.host;
				if ((u.scheme == "https" && u.port != 443) || (u.scheme == "http" && u.port != 80)) {
					base += ":" + std::to_string(u.port);
				}
				location = location.empty() || location[0] == '/' ? base + location : base + "/" + location;
			}
			Url next;
			if (!next.Parse(location)) {
				res.error = "cannot parse the redirect target";
				return res;
			}
			// A feed URL is configuration. Quietly following it off TLS is not
			// this code's decision to make.
			if (u.scheme == "https" && next.scheme != "https") {
				res.error = "refusing to follow a redirect from https to http";
				return res;
			}
			current = location;
			continue;
		}

		res.etag = Header(headers, "etag");
		res.last_modified = Header(headers, "last-modified");
		res.content_type = Header(headers, "content-type");

		std::string te = util::Lower(Header(headers, "transfer-encoding"));
		std::string cl = Header(headers, "content-length");
		if (te.find("chunked") != std::string::npos) {
			if (!ReadChunked(s, res.body, opts.max_bytes)) {
				res.error = "chunked body was truncated or too large";
				return res;
			}
		} else if (!cl.empty()) {
			size_t n = (size_t)std::strtoull(cl.c_str(), nullptr, 10);
			if (opts.max_bytes > 0 && n > opts.max_bytes) {
				res.error = "response is larger than the configured limit";
				return res;
			}
			if (!s.ReadN(res.body, n)) {
				res.error = "body was truncated";
				return res;
			}
		} else {
			// Neither framing header: the body runs to end of connection, which
			// is legal for a response and is why Connection: close is sent.
			std::string part;
			while (s.ReadAvailable(part, 65536)) {
				res.body += part;
				if (opts.max_bytes > 0 && res.body.size() > opts.max_bytes) {
					res.error = "response is larger than the configured limit";
					return res;
				}
			}
		}

		res.ok = status >= 200 && status < 300;
		if (!res.ok) {
			res.error = "HTTP " + std::to_string(status);
		}
		return res;
	}

	res.error = "too many redirects";
	return res;
}

} // namespace httpc
} // namespace quackmail
