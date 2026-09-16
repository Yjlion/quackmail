#pragma once

#include "web.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/davxml.hpp"

namespace duckdb {
namespace qmweb {

// CalDAV and CardDAV over the groupware rooms.
//
// Nothing here is a new store. A calendar collection *is* a room whose
// default_view is VIEW_CALENDAR, and an event *is* the format_type 4 message in
// it carrying a text/calendar part — the same objects the web UI at
// /bbs/room/:n/item/... edits and the same ones IMAP serves. So the whole module
// is a projection: parse a path into (user, room, euid), apply the permission
// predicates every other front-end applies, and serialize.
//
// The URL space uses room *numbers* for the reason web_views.hpp already gives:
// a Citadel room name may contain '/', which no amount of percent-encoding
// survives once a path has been split into segments.
//
//   /.well-known/caldav                          -> /dav/
//   /.well-known/carddav                         -> /dav/
//   /dav/                                        the root; current-user-principal
//   /dav/principals/users/<user>/                the principal; the home sets
//   /dav/calendars/<user>/                       calendar home
//   /dav/calendars/<user>/<room>/                one calendar
//   /dav/calendars/<user>/<room>/<name>.ics      one event or task
//   /dav/addressbooks/<user>/                    addressbook home
//   /dav/addressbooks/<user>/<room>/             one addressbook
//   /dav/addressbooks/<user>/<room>/<name>.vcf   one contact
//   /dav/files/<user>/                           file-area home
//   /dav/files/<user>/<room>/                    one file area
//   /dav/files/<user>/<room>/<name>              one file, name as uploaded
//
// <room> is the room number, because a Citadel room name may contain '/' and
// ParseDavPath has to split a path without a database. A collection a client
// created with MKCOL/MKCALENDAR is served under the segment *it* chose instead,
// resolved through citadel_dav_collections — a 201 that then serves the
// collection at a different URL has broken the client that asked for it.

namespace davx = quackmail::dav;

// How a room is exposed, if at all.
enum class DavKind {
	None,
	Calendar,    // VIEW_CALENDAR, VIEW_CALBRIEF, VIEW_TASKS
	AddressBook, // VIEW_ADDRESSBOOK
	// A QR_DIRECTORY room: plain WebDAV, no groupware type at all. Unlike the
	// two above it is selected by a room *flag* rather than by its view, because
	// a file area is a property of the room's flags in Citadel and always has
	// been — the three bits were simply never read until now.
	Files,
};

// Room creation gates, defined in web_rooms.cpp. DAV asks rather than deriving
// its own answer, for the same reason every front-end that accepts a message
// asks CanPost: two copies of a permission rule is one rule and one bug.
bool AxLevelMayCreateRooms(const Ctx &ctx);
bool MayCreateRoomOnFloor(const Ctx &ctx, int64_t floor);

// A room, seen as a DAV collection.
struct DavCollection {
	quackmail::citadel::Room room;
	DavKind kind = DavKind::None;
	// The URL segment this collection is served under: the room number, or the
	// name a client gave it through MKCOL. Resolved once, where the Connection
	// is, so the href builders stay pure.
	std::string segment;
	// The component set is VTODO rather than VEVENT. Still a calendar
	// collection — CalDAV has no separate task collection — but a client that
	// asks for VEVENTs should not be offered this one.
	bool tasks = false;
};

// What a request path points at.
enum class DavRes {
	None, // not a path this module serves
	Root,
	Principal,
	Home,
	Collection,
	Object,
};

struct DavPath {
	DavRes type = DavRes::None;
	DavKind kind = DavKind::None; // which home the path sits under
	std::string user;
	int64_t room_num = -1;
	// The collection segment exactly as it arrived. Normally the decimal room
	// number, in which case room_num is set and this is redundant; for a
	// collection a client named itself through MKCOL/MKCALENDAR, room_num stays
	// -1 and this is the only handle on it until the binding is resolved
	// against the database. Parsing stays pure either way.
	std::string segment;
	std::string name; // the encoded resource name, extension included
	std::string euid; // the name decoded, extension stripped
};

// Parse the tail of /dav/... . Pure: it never touches the database, so an
// unauthorized user learns nothing from it that the URL did not already say.
DavPath ParseDavPath(const std::string &tail);

// ---- hrefs ---------------------------------------------------------------
// Always absolute paths, never absolute URLs. A client resolves them against
// the request URL, which keeps us out of the business of guessing our own
// scheme and host — the same reason the HTTPS redirect refuses to build a
// Location out of the Host header.

std::string PrincipalHref(const std::string &user);
std::string HomeHref(DavKind kind, const std::string &user);
std::string CollectionHref(DavKind kind, const std::string &user, const std::string &segment);
// Convenience for the numeric form, which is what every collection this server
// created before MKCOL existed is still served under.
std::string CollectionHref(DavKind kind, const std::string &user, int64_t room_num);
// The href for a resource *name* — the last path segment, extension included.
// Not for an euid: the two are not the same thing (see the naming section
// below), and a caller that has only an euid asks ResourceNameFor first.
std::string ObjectHref(DavKind kind, const std::string &user, int64_t room_num, const std::string &name);
std::string ObjectHref(DavKind kind, const std::string &user, const std::string &segment,
                       const std::string &name);

// ".ics" / ".vcf", and "text/calendar" / "text/vcard".
const char *ObjectExt(DavKind kind);
const char *ObjectMediaType(DavKind kind);

// ---- collections and objects ---------------------------------------------

DavKind KindForView(int64_t default_view);

// Every collection of this kind the signed-in user may see. Passworded rooms
// they have not unlocked are left out: there is no way to prompt for a password
// over DAV, and listing a collection whose objects then 403 is worse than not
// listing it.
std::vector<DavCollection> ListCollections(Ctx &ctx, DavKind kind);

// Resolve the collection a path names. False when it does not exist, is not
// visible, or is not a collection of the kind the path claimed.
bool ResolveCollection(Ctx &ctx, const DavPath &p, DavCollection &out);

struct DavObject {
	int64_t msgnum = 0;
	std::string euid;
	std::string name;  // the resource name it is served under, extension included
	std::string body;  // the text/calendar or text/vcard part, as stored
	int64_t msgtime = 0;
	// The media type to serve this object as. Empty means "whatever the
	// collection kind implies", which is what every groupware object wants —
	// a calendar holds text/calendar and nothing else. A file carries its own,
	// because a file area holds whatever was put in it.
	std::string content_type;
	// The payload size, for getcontentlength. Set for a file even when `body`
	// was not loaded, because a PROPFIND over a directory of large files must be
	// able to report their sizes without reading all of them.
	int64_t size = 0;
};

// ---- resource naming -----------------------------------------------------
//
// A resource name is *not* the object's UID, and assuming it was is a mistake
// this module made once. The store keys an object by its own UID — an invariant
// the native ENT0 path and the web UI both rely on — but a WebDAV client owns
// the URL space, and several shipping ones (vdirsyncer among them) PUT a new
// object to a random UUID of their own choosing. Refusing that is refusing the
// client.
//
// So the two are bound rather than equated. `citadel_dav_names` records the
// binding where it is not the default, and the default is the euid run through
// dav::NameForEuid plus the collection's extension — which is what an object
// created through the web UI or by the native protocol is served as, with no
// row needed.

// The name a stored object is served under.
std::string ResourceNameFor(Ctx &ctx, const DavCollection &c, const std::string &euid);
// The euid a resource name resolves to, or "" when it names nothing here.
// Falls back to decoding the name as an euid, so a client that *does* use the
// UID as the name needs no binding.
std::string EuidForResource(Ctx &ctx, const DavCollection &c, const std::string &name);
// Record (or forget) that `name` in this collection holds `euid`.
void BindResourceName(Ctx &ctx, const DavCollection &c, const std::string &name,
                      const std::string &euid);
void UnbindResourceName(Ctx &ctx, const DavCollection &c, const std::string &name);

// Every object in the collection, in msgnum order. Messages carrying no part of
// the collection's media type are skipped rather than mis-rendered — a room can
// hold an ordinary message alongside its objects.
std::vector<DavObject> ListObjects(Ctx &ctx, const DavCollection &c);
bool LoadObject(Ctx &ctx, const DavCollection &c, const std::string &euid, DavObject &out);
// The same, by resource name rather than by euid — what a request path gives.
bool LoadObjectByName(Ctx &ctx, const DavCollection &c, const std::string &name, DavObject &out);

// The ETag for a stored object. It is the message number, which UpsertByEuid
// allocates afresh on every save — so it changes exactly when the object does,
// which is what If-Match needs of it.
std::string ETagFor(int64_t msgnum);

// The collection's sync-token and ctag, which are the same number wearing two
// hats. Opaque to clients by contract; ours happens to be readable.
std::string SyncToken(Ctx &ctx, int64_t room_num);
// The number inside one of our tokens, or -1 if it is not one of ours (a token
// from another server, or a corrupted one) — which the caller must answer with
// a full re-listing rather than an empty diff.
int64_t SyncTokenValue(Ctx &ctx, const std::string &token);

// ---- request and response plumbing ---------------------------------------

// Parse the request body as an XML document. Renders 400 and returns false when
// it is malformed. An empty body yields false with `empty` set and nothing
// rendered — several verbs define "no body" as a request of its own.
bool DavBody(Ctx &ctx, davx::Node &out, bool &empty);

// The Depth header, clamped to what we implement (0 or 1; `infinity` is refused
// by returning `dflt` only when the caller allows it).
int DavDepth(Ctx &ctx, int dflt);

// A bare status with no body, for the verbs that answer with one.
void DavStatus(Ctx &ctx, int status);
// A DAV error document: <D:error><C:some-condition/></D:error>. The precondition
// name is what tells a client *which* rule it broke, and is the difference
// between a client retrying correctly and a client retrying forever.
void DavError(Ctx &ctx, int status, const std::string &ns, const std::string &condition);
// Send an assembled multistatus body as 207.
void SendMultiStatus(Ctx &ctx, const std::string &xml);

// ---- properties ----------------------------------------------------------

// What a PROPFIND or REPORT asked for.
struct PropRequest {
	bool allprop = false;
	bool propname = false;
	std::vector<std::pair<std::string, std::string>> props; // (namespace URI, local name)

	bool Wants(const char *ns, const char *name) const;
};

// Read the <D:prop>/<D:allprop>/<D:propname> out of a request body. An absent
// body means allprop, so callers pass a default-constructed Node for that case.
PropRequest ParsePropRequest(const davx::Node &root, bool body_present);

// Everything a <D:response> might need, so one writer serves every resource
// type rather than five near-copies.
struct PropSource {
	std::string href;
	DavRes type = DavRes::None;
	DavKind kind = DavKind::None;
	std::string user;
	const DavCollection *coll = nullptr; // for Collection and Object
	const DavObject *obj = nullptr;      // for Object
	// Inline the object's body as <C:calendar-data>/<CARD:address-data>. Set by
	// the multiget and query REPORTs, never by PROPFIND — RFC 4791 is explicit
	// that PROPFIND must not return calendar data.
	bool want_data = false;
};

// Write one <D:response>. Properties the resource does not have come back in a
// second propstat with 404, which is what a client uses to learn what we
// support; dropping them silently is what makes a client ask forever.
void WriteResponse(Ctx &ctx, davx::Writer &w, const PropSource &src, const PropRequest &pr);
// A <D:response> saying only that a resource is gone, for sync-collection.
void WriteGoneResponse(davx::Writer &w, const std::string &href);

// ---- the verbs -----------------------------------------------------------
// Each is defined in the file named after it and dispatched by dav_router.cpp.

void DavPropfind(Ctx &ctx, const DavPath &p);
// MKCOL (RFC 4918 §9.3, extended by RFC 5689) and MKCALENDAR (RFC 4791 §5.3.1).
// Both create a room; which verb it was decides only how the client said so.
void DavMkcol(Ctx &ctx, const DavPath &p);
// LOCK/UNLOCK (RFC 4918 §9.10-9.11), for file areas only. See dav_lock.cpp for
// why the "no locking" decision is reversed there and nowhere else.
void DavLock(Ctx &ctx, const DavPath &p);
void DavUnlock(Ctx &ctx, const DavPath &p);
// Is a write to `resource` blocked by somebody else's lock? False for every
// collection kind but Files, which is the only one that locks.
bool LockBlocks(Ctx &ctx, const DavCollection &c, const std::string &resource);
// What a 405 lists, and what OPTIONS advertises.
extern const char *const kAllowHeader;
// One writable collection property, shared by PROPPATCH and by MKCOL's creation
// body. Returns the per-property status to report.
int ApplyCollectionProp(Ctx &ctx, DavCollection &c, const davx::Node &item, bool removing);
void DavProppatch(Ctx &ctx, const DavPath &p);
void DavReport(Ctx &ctx, const DavPath &p);
void DavGet(Ctx &ctx, const DavPath &p, bool head_only);
void DavPut(Ctx &ctx, const DavPath &p);
void DavDelete(Ctx &ctx, const DavPath &p);

} // namespace qmweb
} // namespace duckdb
