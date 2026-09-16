#include "dav.hpp"

#include "quackmail/filearea.hpp"
#include "quackmail/util.hpp"

#include <ctime>

namespace duckdb {
namespace qmweb {

namespace {

// RFC 4918 §10.5 says a server may pick any timeout. Ten minutes is long enough
// that a save over a slow link does not lose the lock, and short enough that a
// client which crashed mid-edit does not hold a file hostage until someone
// notices. Refreshed by a LOCK with no body and the token in `If`.
const int64_t kLockSeconds = 600;

int64_t Now() {
	return (int64_t)std::time(nullptr);
}

// Drop anything expired before answering. There is no lock reaper and there
// should not be one: a lock is only ever consulted here, so sweeping on the way
// past is both sufficient and impossible to forget.
void Expire(Ctx &ctx) {
	Exec(ctx.con, "DELETE FROM citadel_dav_locks WHERE expires_at < $1", {Value::BIGINT(Now())});
}

// Every lock token in an `If:` header. Deliberately not a full RFC 4918 §10.4
// If-header parser — the tagged-list and etag forms are not used by the clients
// this exists for, and accepting a token that appears anywhere in the header is
// the permissive direction: it can let a request through, never block one that
// should have passed.
std::vector<std::string> TokensIn(const std::string &header) {
	std::vector<std::string> out;
	size_t at = 0;
	while ((at = header.find('<', at)) != std::string::npos) {
		size_t end = header.find('>', at);
		if (end == std::string::npos) {
			break;
		}
		out.push_back(header.substr(at + 1, end - at - 1));
		at = end + 1;
	}
	return out;
}

struct Lock {
	std::string token;
	std::string username;
	std::string owner;
	int64_t expires_at = 0;
};

bool LockOn(Ctx &ctx, int64_t room_num, const std::string &resource, Lock &out) {
	auto r = Exec(ctx.con,
	              "SELECT token, username, owner, expires_at FROM citadel_dav_locks "
	              "WHERE room_num = $1 AND resource = $2 LIMIT 1",
	              {Value::BIGINT(room_num), Value(resource)});
	if (!r) {
		return false;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	if (mat.RowCount() == 0) {
		return false;
	}
	out.token = mat.GetValue(0, 0).ToString();
	out.username = mat.GetValue(1, 0).ToString();
	out.owner = mat.GetValue(2, 0).ToString();
	out.expires_at = mat.GetValue(3, 0).GetValue<int64_t>();
	return true;
}

void WriteLockDiscovery(davx::Writer &w, const Lock &lock, const std::string &href) {
	w.Open(davx::kNsDav, "activelock");
	w.Open(davx::kNsDav, "locktype");
	w.Empty(davx::kNsDav, "write");
	w.Close();
	w.Open(davx::kNsDav, "lockscope");
	w.Empty(davx::kNsDav, "exclusive");
	w.Close();
	w.TextElem(davx::kNsDav, "depth", "0");
	if (!lock.owner.empty()) {
		w.Open(davx::kNsDav, "owner");
		w.Text(lock.owner);
		w.Close();
	}
	w.TextElem(davx::kNsDav, "timeout", "Second-" + std::to_string(kLockSeconds));
	w.Open(davx::kNsDav, "locktoken");
	w.TextElem(davx::kNsDav, "href", lock.token);
	w.Close();
	w.Open(davx::kNsDav, "lockroot");
	w.TextElem(davx::kNsDav, "href", href);
	w.Close();
	w.Close();
}

} // namespace

bool LockBlocks(Ctx &ctx, const DavCollection &c, const std::string &resource) {
	// Only file areas lock at all, so nothing else pays for this.
	if (c.kind != DavKind::Files) {
		return false;
	}
	Expire(ctx);
	Lock lock;
	if (!LockOn(ctx, c.room.room_num, resource, lock)) {
		return false;
	}
	// The holder's own requests go through whether or not they quote the token.
	// Explorer does quote it; some clients do not, and locking a user out of the
	// file they themselves locked is worse than the lock being slightly loose.
	if (lock.username == ctx.username) {
		return false;
	}
	for (const auto &t : TokensIn(ctx.req.Header("If"))) {
		if (t == lock.token) {
			return false;
		}
	}
	return true;
}

// LOCK (RFC 4918 §9.10). Exclusive write locks on files, and nothing else.
//
// This is the one place the "no locking" decision recorded in TODO.md is
// reversed, and only here: ETags and If-Match remain the whole consistency
// story for calendars and contacts, because every client that speaks CalDAV
// speaks that. A file share is the case where it is not enough — Windows
// Explorer and macOS Finder will not mount a class-1 DAV share read-write.
void DavLock(Ctx &ctx, const DavPath &p) {
	if (p.kind != DavKind::Files || p.type != DavRes::Object) {
		// A collection lock would have to cover every file under it and every
		// file added while it was held, which is a promise with no way to keep
		// it cheaply and no client that needs it.
		DavStatus(ctx, 405);
		return;
	}
	DavCollection c;
	if (!ResolveCollection(ctx, p, c)) {
		DavStatus(ctx, 404);
		return;
	}
	// Taking a lock is a write, so it takes the right to write.
	if (!quackmail::citadel::CanPost(ctx.con, ctx.username, c.room)) {
		DavStatus(ctx, 403);
		return;
	}

	Expire(ctx);
	std::string href = ObjectHref(c.kind, ctx.username, c.segment, p.name);
	Lock held;
	const bool existing = LockOn(ctx, c.room.room_num, p.name, held);

	davx::Node root;
	bool empty = true;
	DavBody(ctx, root, empty);

	if (empty) {
		// A LOCK with no body refreshes an existing lock (RFC 4918 §9.10.2). It
		// is not a way to take one: without a body there is no lockinfo saying
		// what kind.
		if (!existing) {
			DavStatus(ctx, 412);
			return;
		}
		bool ours = held.username == ctx.username;
		if (!ours) {
			for (const auto &t : TokensIn(ctx.req.Header("If"))) {
				if (t == held.token) {
					ours = true;
					break;
				}
			}
		}
		if (!ours) {
			DavStatus(ctx, 423);
			return;
		}
		Exec(ctx.con, "UPDATE citadel_dav_locks SET expires_at = $1 WHERE token = $2",
		     {Value::BIGINT(Now() + kLockSeconds), Value(held.token)});
		held.expires_at = Now() + kLockSeconds;
	} else {
		if (existing && held.username != ctx.username) {
			DavStatus(ctx, 423);
			return;
		}
		// Shared locks are not offered. Answering a shared request with an
		// exclusive lock would be lying about what was granted, so it is
		// refused with the precondition that says exactly which part failed.
		if (const davx::Node *scope = root.Child(davx::kNsDav, "lockscope")) {
			if (scope->Child(davx::kNsDav, "shared")) {
				DavError(ctx, 409, davx::kNsDav, "lock-token-matches-request-uri");
				return;
			}
		}
		std::string owner;
		if (const davx::Node *o = root.Child(davx::kNsDav, "owner")) {
			// Kept verbatim as text: it is the client's own identification of
			// who holds the lock, and it is echoed back to clients rather than
			// interpreted here.
			owner = o->text;
			if (owner.empty() && !o->children.empty()) {
				owner = o->children.front().text;
			}
		}
		held.token = "opaquelocktoken:" + quackmail::util::RandomHex(16);
		held.username = ctx.username;
		held.owner = owner;
		held.expires_at = Now() + kLockSeconds;
		Exec(ctx.con,
		     "INSERT OR REPLACE INTO citadel_dav_locks "
		     "(token, room_num, resource, username, owner, depth, expires_at) "
		     "VALUES ($1, $2, $3, $4, $5, 0, $6)",
		     {Value(held.token), Value::BIGINT(c.room.room_num), Value(p.name),
		      Value(ctx.username), Value(owner), Value::BIGINT(held.expires_at)});
	}

	davx::Writer w;
	w.StartDoc(davx::kNsDav, "prop");
	w.Open(davx::kNsDav, "lockdiscovery");
	WriteLockDiscovery(w, held, href);
	w.Close();
	w.Close();
	ctx.resp.Bytes(w.Str(), "application/xml; charset=utf-8");
	ctx.resp.status = 200;
	// The token goes in the header as well as the body: that is where a client
	// reads it from to put in the If: header of the PUT that follows.
	ctx.resp.SetHeader("Lock-Token", "<" + held.token + ">");
	ctx.resp.SetHeader("Cache-Control", "no-store");
}

// UNLOCK (RFC 4918 §9.11).
void DavUnlock(Ctx &ctx, const DavPath &p) {
	if (p.kind != DavKind::Files || p.type != DavRes::Object) {
		DavStatus(ctx, 405);
		return;
	}
	DavCollection c;
	if (!ResolveCollection(ctx, p, c)) {
		DavStatus(ctx, 404);
		return;
	}
	Expire(ctx);

	std::string tok = ctx.req.Header("Lock-Token");
	if (tok.size() > 1 && tok.front() == '<' && tok.back() == '>') {
		tok = tok.substr(1, tok.size() - 2);
	}
	if (tok.empty()) {
		DavStatus(ctx, 400);
		return;
	}
	Lock held;
	if (!LockOn(ctx, c.room.room_num, p.name, held) || held.token != tok) {
		// RFC 4918 §9.11.1: the token has to name a lock on *this* resource.
		DavError(ctx, 409, davx::kNsDav, "lock-token-matches-request-uri");
		return;
	}
	if (held.username != ctx.username &&
	    !quackmail::citadel::CanAdminister(ctx.con, ctx.username, c.room)) {
		// Someone else's lock. A room administrator can break it — otherwise a
		// client that died holding one would block the file until it expired.
		DavStatus(ctx, 403);
		return;
	}
	Exec(ctx.con, "DELETE FROM citadel_dav_locks WHERE token = $1", {Value(tok)});
	DavStatus(ctx, 204);
}

} // namespace qmweb
} // namespace duckdb
