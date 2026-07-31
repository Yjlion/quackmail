#define DUCKDB_EXTENSION_MAIN

#include "quackmail_http_extension.hpp"

#include "web.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/http.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/net.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"

#include <chrono>
#include <cstdlib>

namespace duckdb {
namespace {

using namespace quackmail;

// Two listeners over one implementation, as pop3/pop3s does: qm_http speaks
// plaintext (dev 8080, real 80) and qm_https implicit TLS (dev 8443, real 443).
ServerController g_http;
ServerController g_https;

// A tiny error page for failures that happen before there is a Ctx — a
// malformed request line has no session, no nonce and no chrome.
void WriteEarlyError(net::ClientStream &stream, int status, bool head_only) {
	http::Response resp;
	resp.status = status;
	resp.Html("<!doctype html><html><head><meta charset=\"utf-8\"><title>" +
	              std::string(http::StatusText(status)) + "</title></head><body><h1>" +
	              std::to_string(status) + " " + http::StatusText(status) + "</h1></body></html>",
	          status);
	resp.SetHeader("X-Content-Type-Options", "nosniff");
	resp.SetHeader("Cache-Control", "no-store");
	http::WriteResponse(stream, resp, head_only);
}

// Methods this server will route at all. The three the web pages use, plus the
// WebDAV verbs the /dav/ subtree answers — the router still 405s any of the
// latter aimed anywhere else, so widening this list does not widen the site.
//
// Note what is absent: LOCK and UNLOCK. We advertise `DAV: 1` and never level 2,
// so no client is entitled to them, and a lock table is a great deal of state to
// keep for a guarantee CalDAV already gets from ETags.
bool MethodSupported(const std::string &method) {
	static const char *const kMethods[] = {"GET",      "HEAD",   "POST",     "OPTIONS", "PROPFIND",
	                                       "PROPPATCH", "REPORT", "PUT",     "DELETE",  "MKCOL",
	                                       "MKCALENDAR", "COPY",  "MOVE"};
	for (const char *m : kMethods) {
		if (method == m) {
			return true;
		}
	}
	return false;
}

// Serve requests until the peer stops asking or one of the limits says stop.
//
// The schema check and the DuckDB connection are per *connection*, not per
// request, which is most of what makes serving a page's assets over one socket
// cheaper than over five.
void HandleHttp(DatabaseInstance &db, net::ClientStream &stream) {
	Connection con(db);
	store::EnsureSchema(con);

	http::Limits limits;
	auto opened = std::chrono::steady_clock::now();

	for (size_t served = 0; served < limits.max_requests_per_conn; served++) {
		if (served > 0) {
			auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
			               std::chrono::steady_clock::now() - opened)
			               .count();
			if (age > limits.conn_lifetime_ms) {
				return;
			}
			// WaitReadable answers from the internal and OpenSSL buffers before
			// it polls, so a pipelined request that already arrived is not lost
			// to a poll() that would block waiting for the next packet.
			if (!stream.WaitReadable(limits.idle_deadline_ms)) {
				return;
			}
		}

		http::Request req;
		auto result = http::ReadRequest(stream, limits, req, served == 0);

		// A clean close with no request at all is the common case for port
		// scans, for a browser opening a speculative connection, and for the
		// ordinary end of a keep-alive socket; say nothing.
		if (result == http::ReadResult::Eof) {
			return;
		}
		bool head_only = req.method == "HEAD";

		// Every failure below ends the connection, and that is load-bearing
		// rather than lazy. 413 and 411 answer *without* consuming the body the
		// peer announced, so those bytes are still on the wire; reading them as
		// the next request line is exactly the smuggling this codec refuses to
		// allow chunked encoding in order to prevent. The same goes for a
		// method we never read a body for.
		if (result != http::ReadResult::Ok) {
			WriteEarlyError(stream, http::StatusForReadResult(result), head_only);
			return;
		}
		if (!MethodSupported(req.method)) {
			WriteEarlyError(stream, 405, head_only);
			return;
		}

		// The last request the cap allows is answered, then closed — offering
		// keep-alive and hanging up would leave the peer waiting on a socket we
		// have already abandoned.
		bool keep = (served + 1 < limits.max_requests_per_conn) && http::ShouldKeepAlive(req);

		http::Response resp;
		qmweb::Dispatch(con, req, resp);
		if (!http::WriteResponse(stream, resp, head_only, keep) || !keep) {
			return;
		}
	}
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);

	// Connections are persistent now, so bound how many may be open at once.
	// Read here rather than per-accept: this is an operator's ceiling, not a
	// per-request decision, and `quackcitadm.sh config set` plus a restart is
	// the documented way to change it.
	int cap = (int)std::strtol(citadel::GetConfig(con, "qm_http_max_connections", "256").c_str(), nullptr, 10);
	if (cap < 0) {
		cap = 0;
	}
	g_http.SetMaxConnections(cap);
	g_https.SetMaxConnections(cap);

	RegisterServerControls(loader, "qm_http", 8080, g_http, HandleHttp);
	RegisterServerControls(loader, "qm_https", 8443, g_https, HandleHttp);
}

} // namespace

void QuackmailHttpExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string QuackmailHttpExtension::Name() {
	return "quackmail_http";
}

std::string QuackmailHttpExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_HTTP
	return EXT_VERSION_QUACKMAIL_HTTP;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_http, loader) {
	duckdb::LoadInternal(loader);
}
}
