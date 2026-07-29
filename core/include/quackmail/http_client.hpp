#pragma once

#include <string>
#include <vector>

namespace quackmail {
namespace httpc {

// An outbound HTTP/1.1 client, for fetching feeds.
//
// Separate from core/http.hpp on purpose. That is a *server* codec, and it
// deliberately refuses chunked transfer encoding — accepting it on the server
// side opens the request-smuggling class for nothing, since browsers never
// chunk a form post. On the client side chunking is not optional: feed servers
// do chunk, and a client that cannot decode it simply cannot read them.

struct Url {
	std::string scheme; // "http" | "https"
	std::string host;
	int port = 0;
	std::string path; // includes the query
	bool Parse(const std::string &url);
};

struct Options {
	int timeout_ms = 30000;
	int max_redirects = 3;
	size_t max_bytes = 4 * 1024 * 1024;
	std::string user_agent = "QuackCit";
	// Conditional-request headers from the last fetch. A feed that honours
	// either turns a poll into a 304 and no body at all, which is the
	// difference between being a polite client and being a nuisance.
	std::string etag;
	std::string last_modified;
};

struct Response {
	bool ok = false;      // a request completed (2xx or 304); see status
	int status = 0;
	std::string error;    // transport-level failure
	std::string body;
	std::string etag;
	std::string last_modified;
	std::string content_type;

	bool NotModified() const {
		return status == 304;
	}
};

// GET `url`. Follows up to `max_redirects` redirects, and refuses to follow one
// that leaves https for http — a feed URL is configuration, and silently
// downgrading its transport is not the caller's decision to make.
Response Get(const std::string &url, const Options &opts);

} // namespace httpc
} // namespace quackmail
