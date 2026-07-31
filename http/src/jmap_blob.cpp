#include "jmap.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include "quackmail/util.hpp"

#include <cstdlib>
#include <ctime>

namespace duckdb {
namespace qmweb {

namespace {

// Uploaded blobs live in their own id namespace, marked by a leading 'U'.
//
// Every other blob id is "<msgnum>" or "<msgnum>.<section>" and resolves to
// bytes already in the message store. An uploaded one has no message yet — it
// is what a client sends *before* the Email/set that will reference it — so the
// two cannot share a namespace without one being mistakable for the other.
constexpr const char *kUploadPrefix = "U";

} // namespace

bool IsUploadBlobId(const std::string &blob_id) {
	return blob_id.size() > 1 && blob_id[0] == 'U' &&
	       blob_id.find_first_not_of("0123456789abcdef", 1) == std::string::npos;
}

std::string StoreBlob(Ctx &ctx, const std::string &content_type, const std::string &body) {
	std::string id = kUploadPrefix + quackmail::util::RandomHex(16);
	if (id.size() <= 1) {
		return std::string(); // the RNG failed; a predictable blob id is not one
	}
	auto r = Exec(ctx.con,
	              "INSERT INTO quackmail_jmap_blobs "
	              "(blob_id, username, content_type, size_bytes, payload, created_at) "
	              "VALUES ($1, $2, $3, $4, $5, $6)",
	              {Value(id), Value(ctx.username), Value(content_type),
	               Value::BIGINT((int64_t)body.size()),
	               Value::BLOB(reinterpret_cast<const duckdb::data_t *>(body.data()), body.size()),
	               Value::BIGINT((int64_t)std::time(nullptr))});
	return r ? id : std::string();
}

bool LoadBlob(Ctx &ctx, const std::string &blob_id, std::string &body, std::string &content_type) {
	if (!IsUploadBlobId(blob_id)) {
		return false;
	}
	// Scoped to the uploader. A blob id is random, but "unguessable" is not an
	// access rule, and one user's draft attachment is not another's to read.
	auto r = Exec(ctx.con,
	              "SELECT content_type, payload FROM quackmail_jmap_blobs "
	              "WHERE blob_id = $1 AND username = $2",
	              {Value(blob_id), Value(ctx.username)});
	if (!r) {
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() < 1) {
		return false;
	}
	Value type_v = mat.GetValue(0, 0);
	content_type = type_v.IsNull() ? std::string() : type_v.ToString();
	Value payload = mat.GetValue(1, 0);
	body = payload.IsNull() ? std::string() : duckdb::StringValue::Get(payload);
	return true;
}

void PruneBlobs(Connection &con, int64_t older_than_seconds) {
	if (older_than_seconds <= 0) {
		return;
	}
	// Unreferenced by definition: once Email/set copies a blob into a message
	// the bytes live in citadel_messages, and nothing reads the staging row
	// again. JMAP says a blob is temporary until an object references it, so
	// dropping the stale ones is the contract rather than a cleanup shortcut.
	int64_t cutoff = (int64_t)std::time(nullptr) - older_than_seconds;
	auto stmt = con.Prepare("DELETE FROM quackmail_jmap_blobs WHERE created_at < $1");
	if (stmt->HasError()) {
		return;
	}
	duckdb::vector<Value> params = {Value::BIGINT(cutoff)};
	stmt->Execute(params, false);
}

namespace {

// POST /jmap/upload/{accountId}
//
// The one JMAP endpoint that is not a method call: it takes raw bytes with the
// client's own Content-Type and answers with the blobId an Email/set can then
// reference.
void JmapUpload(Ctx &ctx) {
	if (ctx.req.method != "POST") {
		ctx.resp.SetHeader("Allow", "POST");
		ctx.resp.status = 405;
		ctx.resp.body.clear();
		return;
	}
	if (ctx.Cap(0) != ctx.username) {
		// Not 403: whether another account exists is not something to confirm.
		ctx.resp.status = 404;
		ctx.resp.body.clear();
		return;
	}
	if (ctx.req.body.empty()) {
		ctx.resp.status = 400;
		ctx.resp.body.clear();
		return;
	}

	// The codec's own ceiling already refused anything larger with a 413 before
	// routing; this is the same number, advertised so a client can refuse first.
	if (ctx.req.body.size() > http::Limits().max_body) {
		ctx.resp.status = 413;
		ctx.resp.body.clear();
		return;
	}

	std::string type = ctx.req.Header("Content-Type");
	if (type.empty()) {
		type = "application/octet-stream";
	}
	std::string id = StoreBlob(ctx, type, ctx.req.body);
	if (id.empty()) {
		ctx.resp.status = 500;
		ctx.resp.body.clear();
		return;
	}

	js::Value out = js::Value::MakeObject();
	out.Set("accountId", ctx.username);
	out.Set("blobId", id);
	out.Set("type", type);
	out.Set("size", (int64_t)ctx.req.body.size());
	ctx.resp.Bytes(js::Serialize(out), "application/json; charset=utf-8");
	ctx.resp.status = 201;
	ctx.resp.SetHeader("Cache-Control", "no-store");
	ctx.resp.SetHeader("X-Content-Type-Options", "nosniff");
}

} // namespace

void RegisterJmapUploadRoute(std::vector<Route> &out) {
	out.push_back({"*", "/jmap/upload/:account", Role::Api, JmapUpload});
}

} // namespace qmweb
} // namespace duckdb
