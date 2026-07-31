#include "dav.hpp"
#include "web_views.hpp"

#include "quackmail/ical.hpp"
#include "quackmail/util.hpp"
#include "quackmail/vcard.hpp"

namespace duckdb {
namespace qmweb {

namespace {

namespace ical = quackmail::ical;
namespace vcard = quackmail::vcard;

// Does the request's If-Match / If-None-Match allow it to proceed?
//
// Only the two forms a DAV client actually sends are honoured: `*` and a single
// entity tag. A list is treated as a match if any member matches, which is the
// spec's rule and costs one loop.
bool ETagListMatches(const std::string &header, const std::string &etag) {
	size_t pos = 0;
	while (pos < header.size()) {
		size_t comma = header.find(',', pos);
		std::string item = header.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
		// Trim, and drop a weak-comparison marker: we only ever mint strong tags,
		// so W/"12" and "12" name the same version.
		while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
			item.erase(0, 1);
		}
		while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
			item.pop_back();
		}
		if (item.rfind("W/", 0) == 0) {
			item = item.substr(2);
		}
		if (item == etag || item == "*") {
			return true;
		}
		if (comma == std::string::npos) {
			break;
		}
		pos = comma + 1;
	}
	return false;
}

// The subject a stored groupware message carries. It is what the BBS listing and
// IMAP show for the object, so it should read like a summary rather than a UID.
std::string SubjectFor(DavKind kind, const std::string &body, const std::string &euid) {
	if (kind == DavKind::AddressBook) {
		vcard::Card card;
		if (vcard::ParseOne(body, card)) {
			std::string fn = card.Fn();
			if (!fn.empty()) {
				return fn;
			}
		}
		return euid;
	}
	std::vector<ical::Item> items;
	if (ical::ParseItems(body, items) && !items.empty() && !items[0].summary.empty()) {
		return items[0].summary;
	}
	return euid;
}

// The UID the submitted body claims, and whether it parsed as the kind of object
// this collection holds at all.
bool BodyUid(DavKind kind, const std::string &body, std::string &uid, std::string &why) {
	if (kind == DavKind::AddressBook) {
		vcard::Card card;
		if (!vcard::ParseOne(body, card)) {
			why = "not a vCard";
			return false;
		}
		uid = vcard::EuidFor(card);
		return true;
	}
	std::vector<ical::Item> items;
	if (!ical::ParseItems(body, items) || items.empty()) {
		why = "not an iCalendar object";
		return false;
	}
	// A resource may hold several components — a recurring event and its
	// overrides — but RFC 4791 requires them all to share one UID, and it is
	// that UID the resource is stored under.
	uid = ical::EuidFor(items[0]);
	return true;
}

} // namespace

void DavGet(Ctx &ctx, const DavPath &p, bool head_only) {
	// Only objects have bodies. A GET of a collection is legal but has no
	// defined representation, and serving the web UI's HTML here would confuse a
	// client that followed a calendar-home href by mistake.
	if (p.type != DavRes::Object) {
		DavStatus(ctx, 404);
		return;
	}
	DavCollection c;
	DavObject o;
	if (!ResolveCollection(ctx, p, c) || !LoadObject(ctx, c, p.euid, o)) {
		DavStatus(ctx, 404);
		return;
	}

	std::string etag = ETagFor(o.msgnum);
	std::string inm = ctx.req.Header("If-None-Match");
	if (!inm.empty() && ETagListMatches(inm, etag)) {
		DavStatus(ctx, 304);
		ctx.resp.SetHeader("ETag", etag);
		return;
	}

	// The stored bytes, verbatim. Re-emitting through ical::Emit would drop
	// every property this server does not model, and a round trip that silently
	// edits a client's data is the one thing a sync target must never do.
	//
	// The body goes in even for HEAD: http::WriteResponse suppresses it on the
	// way out while keeping Content-Length honest, which is the whole point of
	// its head_only parameter. Clearing it here would advertise a length of nil.
	(void)head_only;
	ctx.resp.Bytes(o.body, std::string(ObjectMediaType(c.kind)) + "; charset=utf-8");
	ctx.resp.SetHeader("ETag", etag);
	ctx.resp.SetHeader("Cache-Control", "no-store");
	ctx.resp.SetHeader("X-Content-Type-Options", "nosniff");
}

void DavPut(Ctx &ctx, const DavPath &p) {
	if (p.type != DavRes::Object) {
		// PUT onto a collection would be creating one, which is MKCALENDAR's job
		// and which this server does not offer — see dav_router.cpp.
		DavStatus(ctx, 405);
		return;
	}
	DavCollection c;
	if (!ResolveCollection(ctx, p, c)) {
		DavStatus(ctx, 404);
		return;
	}
	if (ctx.req.body.empty()) {
		DavError(ctx, 400, davx::kNsDav, "");
		return;
	}

	// The media type, when the client bothered to send one. Checked loosely:
	// the body is parsed below either way, and rejecting on the header alone
	// would turn a client's sloppy Content-Type into a sync failure.
	std::string ct = quackmail::util::Lower(ctx.req.Header("Content-Type"));
	if (!ct.empty()) {
		bool ok = c.kind == DavKind::AddressBook
		              ? (ct.rfind("text/vcard", 0) == 0 || ct.rfind("text/x-vcard", 0) == 0)
		              : ct.rfind("text/calendar", 0) == 0;
		if (!ok) {
			DavStatus(ctx, 415);
			return;
		}
	}

	std::string uid;
	std::string why;
	if (!BodyUid(c.kind, ctx.req.body, uid, why) || uid.empty()) {
		DavError(ctx, 415,
		         c.kind == DavKind::AddressBook ? davx::kNsCardDav : davx::kNsCalDav,
		         c.kind == DavKind::AddressBook ? "valid-address-data" : "valid-calendar-data");
		return;
	}

	// The resource name *is* the UID here.
	//
	// The store keys a groupware object by its own UID — that is the invariant
	// the native ENT0 path, the web UI and this module all rely on to agree
	// about what a second save means — and there is no name-to-UID mapping table
	// to make a differently named resource resolvable afterwards. Storing under
	// the body's UID while the client remembers a different href would give it a
	// 404 on the very next GET, so the disagreement is reported now instead.
	//
	// In practice this costs nothing: every client that ships names the resource
	// after the UID.
	if (p.euid != uid) {
		DavError(ctx, 409, c.kind == DavKind::AddressBook ? davx::kNsCardDav : davx::kNsCalDav,
		         "no-uid-conflict");
		return;
	}

	int64_t existing = quackmail::citadel::FindByEuid(ctx.con, c.room.room_num, uid);

	// The conditional headers, which are how two clients editing one event do
	// not silently overwrite each other.
	std::string inm = ctx.req.Header("If-None-Match");
	std::string im = ctx.req.Header("If-Match");
	if (!inm.empty() && existing >= 0) {
		// "Create only" against something that already exists.
		DavStatus(ctx, 412);
		return;
	}
	if (!im.empty()) {
		if (existing < 0 || !ETagListMatches(im, ETagFor(existing))) {
			DavStatus(ctx, 412);
			return;
		}
	}

	int status = 200;
	std::string err;
	int64_t msgnum = SaveObjectRaw(ctx, c.room, uid, SubjectFor(c.kind, ctx.req.body, uid),
	                               ObjectMediaType(c.kind), ctx.req.body, status, err);
	if (msgnum < 0) {
		DavStatus(ctx, status == 403 ? 403 : 400);
		return;
	}

	DavStatus(ctx, existing >= 0 ? 204 : 201);
	ctx.resp.SetHeader("ETag", ETagFor(msgnum));
	if (existing < 0) {
		ctx.resp.SetHeader("Location", ObjectHref(c.kind, ctx.username, c.room.room_num, uid));
	}
}

void DavDelete(Ctx &ctx, const DavPath &p) {
	if (p.type != DavRes::Object) {
		// Deleting a collection means deleting a room, which takes rights and
		// consequences a DELETE cannot express. The room-admin UI owns it.
		DavStatus(ctx, 405);
		return;
	}
	DavCollection c;
	DavObject o;
	if (!ResolveCollection(ctx, p, c) || !LoadObject(ctx, c, p.euid, o)) {
		DavStatus(ctx, 404);
		return;
	}

	std::string im = ctx.req.Header("If-Match");
	if (!im.empty() && !ETagListMatches(im, ETagFor(o.msgnum))) {
		DavStatus(ctx, 412);
		return;
	}

	// The same predicate the web UI's own delete uses. Deletion is a change to
	// the room, and Citadel expresses "may change this room" as CanPost.
	if (!quackmail::citadel::CanPost(ctx.con, ctx.username, c.room)) {
		DavStatus(ctx, 403);
		return;
	}

	std::string err;
	if (!quackmail::citadel::DeleteMessage(ctx.con, c.room.room_num, o.msgnum, err)) {
		DavStatus(ctx, 409);
		return;
	}
	DavStatus(ctx, 204);
}

} // namespace qmweb
} // namespace duckdb
