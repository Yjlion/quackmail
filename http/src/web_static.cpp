#include "web.hpp"
#include "web_assets.hpp"

namespace duckdb {
namespace qmweb {

namespace {

// Everything here is public, cacheable and content-addressed, which is the
// opposite of what SecurityHeaders assumes about a page. So the asset response
// deliberately overrides two of its headers, and the order matters: the
// override has to come *after* the call.
void AssetHeaders(Ctx &ctx, const Asset &a) {
	// An asset is inert data. It never loads anything, is never framed, and has
	// no origin-relative behaviour worth granting.
	SecurityHeaders(ctx, "default-src 'none'");
	ctx.resp.SetHeader("ETag", "\"" + std::string(a.etag) + "\"");
	// Overrides SecurityHeaders' `private, no-store`. Safe precisely because
	// the URL contains the hash of the bytes: a changed file is a changed URL,
	// so nothing cached here can ever be stale.
	ctx.resp.SetHeader("Cache-Control", "public, max-age=31536000, immutable");
}

void GetStatic(Ctx &ctx) {
	const Asset *a = FindAsset(ctx.Cap(0));
	if (!a) {
		// A hash that is not ours is either a stale bookmark from before a
		// redeploy or someone poking at paths. Both get the ordinary 404; there
		// is nothing here to disclose either way. Note that traversal never
		// reaches this point — http::NormalizePath rejects "." and ".."
		// segments before routing — and that even if it did, FindAsset only
		// ever compares against a fixed table and never touches a filesystem.
		NotFound(ctx);
		return;
	}

	// The conditional GET. Assets are immutable so a browser should not be
	// revalidating at all, but a shift-reload does, and answering 304 keeps
	// that cheap.
	if (ctx.req.Header("If-None-Match") == "\"" + std::string(a->etag) + "\"") {
		AssetHeaders(ctx, *a);
		ctx.resp.status = 304;
		return;
	}

	ctx.resp.Bytes(std::string(reinterpret_cast<const char *>(a->data), a->size), a->mime);
	AssetHeaders(ctx, *a);
}

} // namespace

void RegisterStaticRoutes(std::vector<Route> &out) {
	// Anon: the login page needs the stylesheet before anyone has a session,
	// and an asset carries no user data. This is the only route in the module
	// that is deliberately reachable by everyone.
	out.push_back({"GET", "/static/*", Role::Anon, GetStatic});
}

} // namespace qmweb
} // namespace duckdb
