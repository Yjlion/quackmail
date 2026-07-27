#define DUCKDB_EXTENSION_MAIN

#include "quackmail_http_extension.hpp"

#include "web.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/http.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/net.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"

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

void HandleHttp(DatabaseInstance &db, net::ClientStream &stream) {
	Connection con(db);
	store::EnsureSchema(con);

	http::Limits limits;
	http::Request req;
	auto result = http::ReadRequest(stream, limits, req);

	// A clean close with no request at all is the common case for port scans
	// and for a browser opening a speculative connection; say nothing.
	if (result == http::ReadResult::Eof) {
		return;
	}
	bool head_only = req.method == "HEAD";
	if (result != http::ReadResult::Ok) {
		WriteEarlyError(stream, http::StatusForReadResult(result), head_only);
		return;
	}
	if (req.method != "GET" && req.method != "HEAD" && req.method != "POST") {
		WriteEarlyError(stream, 405, head_only);
		return;
	}

	http::Response resp;
	qmweb::Dispatch(con, req, resp);
	http::WriteResponse(stream, resp, head_only);
	// The connection closes here, always: see WriteResponse.
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
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
