#pragma once

#include <string>
#include <utility>
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

	// ---- everything below is for callers other than the feed reader --------

	// Request method, body and content type. Empty method means GET.
	std::string method;
	std::string body;
	std::string content_type;
	// Extra request headers, sent verbatim after the built-in ones.
	std::vector<std::pair<std::string, std::string>> headers;
	// Overrides the feed-flavoured default Accept.
	std::string accept;

	// **Verify the peer certificate and its host name.** Off by default so the
	// feed reader's behaviour is unchanged; anything talking to a service whose
	// identity matters has to ask for it. `ca_bundle` replaces the system trust
	// store, which is what makes a private ACME server usable.
	bool verify_peer = false;
	std::string ca_bundle;
};

struct Response {
	bool ok = false;      // a request completed (2xx or 304); see status
	int status = 0;
	std::string error;    // transport-level failure
	std::string body;
	std::string etag;
	std::string last_modified;
	std::string content_type;
	// Every response header, in order, with lowercased names. The feed reader
	// needs three of them and they are broken out above; ACME needs
	// Replay-Nonce and Location, and a client that discards what it does not
	// recognise cannot be extended without editing this struct again.
	std::vector<std::pair<std::string, std::string>> headers;

	// Case-insensitive lookup; "" when absent.
	std::string Header(const std::string &name) const;

	bool NotModified() const {
		return status == 304;
	}
};

// Perform a request. `opts.method` (default GET), `opts.body` and
// `opts.headers` decide what is sent.
//
// Follows up to `max_redirects` redirects for a GET, and refuses to follow one
// that leaves https for http — a URL here is configuration, and silently
// downgrading its transport is not this code's decision to make. A redirect on
// a **non-GET is not followed at all**: whether the body is resent depends on
// 303-versus-307 semantics that no caller here needs, and guessing wrong would
// either drop a request or replay it.
Response Request(const std::string &url, const Options &opts);

// GET `url`. The original entry point, kept because it is what every feed call
// site says and because a GET is worth spelling as a GET.
Response Get(const std::string &url, const Options &opts);

} // namespace httpc
} // namespace quackmail
