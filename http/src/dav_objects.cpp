#include "dav.hpp"

#include "quackmail/filearea.hpp"
#include "web_views.hpp"

#include "quackmail/ical.hpp"
#include "quackmail/itip.hpp"
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
	if (!ResolveCollection(ctx, p, c) || !LoadObjectByName(ctx, c, p.name, o)) {
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
	if (c.kind == DavKind::Files) {
		// QR_DOWNLOAD is a separate question from "can you see the room": a
		// directory may list its contents to people who may not take them.
		if (!quackmail::filearea::CanDownload(ctx.con, ctx.username, c.room)) {
			DavStatus(ctx, 403);
			return;
		}
		// The type the file was uploaded with, and no charset: a file area holds
		// arbitrary bytes, and declaring UTF-8 over a JPEG would be a lie.
		// Content-Disposition keeps a browser from rendering somebody's uploaded
		// HTML as a page on this origin.
		ctx.resp.Bytes(o.body, o.content_type.empty() ? "application/octet-stream" : o.content_type);
		ctx.resp.SetHeader("Content-Disposition",
		                   "attachment; filename=\"" + quackmail::http::SanitizeFilename(o.name) + "\"");
	} else {
		ctx.resp.Bytes(o.body, std::string(ObjectMediaType(c.kind)) + "; charset=utf-8");
	}
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
	//
	// Not for a file area: it holds whatever was put in it, so there is no type
	// to check against. Checking one here would refuse every upload that is not
	// an .ics.
	std::string ct = quackmail::util::Lower(ctx.req.Header("Content-Type"));
	if (!ct.empty() && c.kind != DavKind::Files) {
		bool ok = c.kind == DavKind::AddressBook
		              ? (ct.rfind("text/vcard", 0) == 0 || ct.rfind("text/x-vcard", 0) == 0)
		              : ct.rfind("text/calendar", 0) == 0;
		if (!ok) {
			DavStatus(ctx, 415);
			return;
		}
	}

	if (c.kind == DavKind::Files) {
		if (!quackmail::filearea::CanUpload(ctx.con, ctx.username, c.room)) {
			DavStatus(ctx, 403);
			return;
		}
		std::string clean = quackmail::filearea::SanitizeName(p.name);
		if (clean.empty() || clean != p.name) {
			// The name is refused rather than silently corrected: a 201 for
			// "notes.txt" when the client asked to PUT "../notes.txt" would tell
			// it the resource is somewhere it is not.
			DavStatus(ctx, 403);
			return;
		}
		// Somebody else's lock. 423 rather than 403: the difference matters to a
		// client, which retries a 423 and gives up on a 403.
		if (LockBlocks(ctx, c, clean)) {
			DavStatus(ctx, 423);
			return;
		}
		quackmail::filearea::File before;
		const bool replacing = quackmail::filearea::StatFile(ctx.con, c.room.room_num, clean, before);

		std::string inm_f = ctx.req.Header("If-None-Match");
		std::string im_f = ctx.req.Header("If-Match");
		if (!inm_f.empty() && replacing) {
			DavStatus(ctx, 412);
			return;
		}
		if (!im_f.empty() && (!replacing || !ETagListMatches(im_f, ETagFor(before.msgnum)))) {
			DavStatus(ctx, 412);
			return;
		}

		std::string err;
		int64_t msgnum = quackmail::filearea::PutFile(
		    ctx.con, c.room.room_num, clean, ctx.req.body, ctx.req.Header("Content-Type"),
		    std::string(), ctx.username, err);
		if (msgnum < 0) {
			// The storage quota lives inside InsertMessage, underneath PutFile,
			// and is transient everywhere else in this server for the same
			// reason: a client that retries should succeed once room is made.
			DavStatus(ctx, 507);
			return;
		}
		DavStatus(ctx, replacing ? 204 : 201);
		ctx.resp.SetHeader("ETag", ETagFor(msgnum));
		if (!replacing) {
			ctx.resp.SetHeader("Location", ObjectHref(c.kind, ctx.username, c.segment, clean));
		}
		return;
	}

	std::string uid;
	std::string why;
	if (!BodyUid(c.kind, ctx.req.body, uid, why) || uid.empty()) {
		DavError(ctx, 415,
		         c.kind == DavKind::AddressBook ? davx::kNsCardDav : davx::kNsCalDav,
		         c.kind == DavKind::AddressBook ? "valid-address-data" : "valid-calendar-data");
		return;
	}

	// The resource name is the client's to choose, and it is not the UID.
	//
	// This module first required the two to match, on the belief that every
	// shipping client names a resource after the object's UID. vdirsyncer does
	// not — it PUTs to a random UUID of its own — so that rule rejected a real
	// client outright. The store still keys the object by its UID, because the
	// native ENT0 path and the web UI depend on that; the name is *bound* to it
	// instead.
	//
	// What is a genuine conflict is the one RFC 4791 actually defines: this UID
	// already living in this collection under a different URI. Accepting that
	// would give the collection two hrefs for one object, and a client that
	// synced both would show the event twice.
	std::string bound_name = ResourceNameFor(ctx, c, uid);
	int64_t existing = quackmail::citadel::FindByEuid(ctx.con, c.room.room_num, uid);
	if (existing >= 0 && bound_name != p.name) {
		DavError(ctx, 409, c.kind == DavKind::AddressBook ? davx::kNsCardDav : davx::kNsCalDav,
		         "no-uid-conflict");
		return;
	}
	// And a name already holding a *different* object is a conflict the other
	// way round: overwriting it would silently destroy the object that is there.
	std::string name_holds = EuidForResource(ctx, c, p.name);
	if (!name_holds.empty() && name_holds != uid &&
	    quackmail::citadel::FindByEuid(ctx.con, c.room.room_num, name_holds) >= 0) {
		DavError(ctx, 409, c.kind == DavKind::AddressBook ? davx::kNsCardDav : davx::kNsCalDav,
		         "no-uid-conflict");
		return;
	}

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

	// Remember what the client calls it, so every href we emit afterwards — in a
	// PROPFIND listing, a REPORT, a sync-collection removal — is the one it
	// already has rather than a second spelling of the same resource.
	BindResourceName(ctx, c, p.name, uid);

	// iMIP: tell the attendees. Only if this user organizes the event —
	// itip::Notify checks — so an attendee storing their own copy does not mail
	// everyone else. Best effort on purpose: the object *is* saved, and failing
	// the PUT because a recipient's MX is down would leave the client believing
	// the event was never created.
	if (c.kind == DavKind::Calendar) {
		quackmail::itip::Sent sent;
		quackmail::itip::Notify(ctx.con, ctx.username, ctx.req.body, quackmail::itip::Method::Request,
		                        "caldav", ctx.tls, sent);
	}

	DavStatus(ctx, existing >= 0 ? 204 : 201);
	ctx.resp.SetHeader("ETag", ETagFor(msgnum));
	if (existing < 0) {
		ctx.resp.SetHeader("Location", ObjectHref(c.kind, ctx.username, c.segment, p.name));
	}
}

void DavDelete(Ctx &ctx, const DavPath &p) {
	if (p.type == DavRes::Collection) {
		// Deleting a collection deletes the room. This used to be a flat 405 on
		// the reasoning that the consequences were more than a DELETE could
		// express — but a client can now create a collection with MKCOL, and one
		// that can make a calendar and not remove it leaves litter it has no way
		// to clean up.
		//
		// Gated on CanAdminister rather than on CanPost: this is the "may change
		// this room's existence" right, the same one the room-admin UI requires
		// to kill a room, not the "may write in it" one.
		DavCollection c;
		if (!ResolveCollection(ctx, p, c)) {
			DavStatus(ctx, 404);
			return;
		}
		if (!quackmail::citadel::CanAdminister(ctx.con, ctx.username, c.room)) {
			DavStatus(ctx, 403);
			return;
		}
		std::string err;
		if (!quackmail::citadel::KillRoom(ctx.con, c.room.room_num, err)) {
			DavStatus(ctx, 409);
			return;
		}
		AideLog(ctx, "Room deleted: " + c.room.display_name,
		        "A collection was deleted over DAV.\n\nRoom: " + c.room.display_name +
		            "\nBy: " + ctx.username);
		DavStatus(ctx, 204);
		return;
	}
	if (p.type != DavRes::Object) {
		// The root, a principal or a home. None of those is a resource a client
		// owns, so none of them is deletable.
		DavStatus(ctx, 405);
		return;
	}
	DavCollection c;
	DavObject o;
	if (!ResolveCollection(ctx, p, c) || !LoadObjectByName(ctx, c, p.name, o)) {
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

	if (c.kind == DavKind::Files) {
		// Removing a file is writing to the room, which is CanPost — the same
		// predicate the upload side asks, minus the QR_UPLOAD bit, because a
		// directory that no longer accepts deposits should still let its
		// existing files be tidied away.
		if (!quackmail::citadel::CanPost(ctx.con, ctx.username, c.room)) {
			DavStatus(ctx, 403);
			return;
		}
		if (LockBlocks(ctx, c, o.name)) {
			DavStatus(ctx, 423);
			return;
		}
		std::string ferr;
		if (!quackmail::filearea::RemoveFile(ctx.con, c.room.room_num, o.name, ferr)) {
			DavStatus(ctx, 409);
			return;
		}
		DavStatus(ctx, 204);
		return;
	}

	// The cancellation goes out *before* the delete, because it is built from
	// the stored bytes and there is nothing to build it from afterwards.
	if (c.kind == DavKind::Calendar) {
		quackmail::itip::Sent sent;
		quackmail::itip::Notify(ctx.con, ctx.username, o.body, quackmail::itip::Method::Cancel,
		                        "caldav", ctx.tls, sent);
	}

	std::string err;
	if (!quackmail::citadel::DeleteMessage(ctx.con, c.room.room_num, o.msgnum, err)) {
		DavStatus(ctx, 409);
		return;
	}
	// The binding goes with it, or the name would still resolve — and a later
	// PUT reusing it would look like a conflict with an object that is gone.
	UnbindResourceName(ctx, c, p.name);
	DavStatus(ctx, 204);
}

} // namespace qmweb
} // namespace duckdb
