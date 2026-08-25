#include "web.hpp"

#include "quackmail/acme.hpp"
#include "quackmail/util.hpp"

#include "duckdb/main/materialized_query_result.hpp"

namespace duckdb {
namespace qmweb {

namespace {

const char *kPrefix = "/.well-known/acme-challenge/";

} // namespace

bool IsAcmeChallengePath(const std::string &path) {
	return path.compare(0, strlen(kPrefix), kPrefix) == 0;
}

// Answer an http-01 challenge.
//
// This is dispatched next to /healthz, *before* the qm_web_force_https
// redirect, and that placement is the whole point. The redirect sends every
// other request to https://c_fqdn, and at first issuance there is nothing there
// but the self-signed certificate we are trying to replace — so a challenge
// routed through it cannot be answered. Let's Encrypt does follow redirects,
// but only to somewhere that already works.
//
// The token is not ours: the CA mints it, so it is unguessable by construction
// and we never have to generate one. What this handler does is refuse anything
// that does not look like one before it touches SQL, answer with the stored key
// authorization, and emit no Location, no cookie and nothing from the
// filesystem — so it is neither an open redirect nor a traversal.
void AcmeChallenge(Ctx &ctx) {
	quackmail::acme::EnsureSchema(ctx.con);

	const std::string token = ctx.req.path.substr(strlen(kPrefix));
	// RFC 8555 §8.3: the token is base64url, at least 128 bits of entropy.
	bool shaped = token.size() >= 16 && token.size() <= 128;
	for (char c : token) {
		if (!isalnum((unsigned char)c) && c != '-' && c != '_') {
			shaped = false;
			break;
		}
	}
	ctx.resp.SetHeader("Cache-Control", "no-store");
	if (!shaped) {
		ctx.resp.Text("not found", 404);
		return;
	}

	auto stmt = ctx.con.Prepare(
	    "SELECT key_auth FROM quackmail_acme_challenges WHERE token = $1 AND expires > $2");
	if (!stmt || stmt->HasError()) {
		ctx.resp.Text("not found", 404);
		return;
	}
	duckdb::vector<Value> params {Value(token), Value::BIGINT((int64_t)std::time(nullptr))};
	auto res = stmt->Execute(params, false);
	if (!res || res->HasError()) {
		ctx.resp.Text("not found", 404);
		return;
	}
	auto &mat = res->Cast<MaterializedQueryResult>();
	if (mat.RowCount() == 0 || mat.GetValue(0, 0).IsNull()) {
		ctx.resp.Text("not found", 404);
		return;
	}
	ctx.resp.Text(mat.GetValue(0, 0).ToString());
}

} // namespace qmweb
} // namespace duckdb
