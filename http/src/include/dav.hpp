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

namespace davx = quackmail::dav;

// How a room is exposed, if at all.
enum class DavKind {
	None,
	Calendar,    // VIEW_CALENDAR, VIEW_CALBRIEF, VIEW_TASKS
	AddressBook, // VIEW_ADDRESSBOOK
};

// A room, seen as a DAV collection.
struct DavCollection {
	quackmail::citadel::Room room;
	DavKind kind = DavKind::None;
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
std::string CollectionHref(DavKind kind, const std::string &user, int64_t room_num);
std::string ObjectHref(DavKind kind, const std::string &user, int64_t room_num, const std::string &euid);

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
	std::string body; // the text/calendar or text/vcard part, as stored
	int64_t msgtime = 0;
};

// Every object in the collection, in msgnum order. Messages carrying no part of
// the collection's media type are skipped rather than mis-rendered — a room can
// hold an ordinary message alongside its objects.
std::vector<DavObject> ListObjects(Ctx &ctx, const DavCollection &c);
bool LoadObject(Ctx &ctx, const DavCollection &c, const std::string &euid, DavObject &out);

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
void DavProppatch(Ctx &ctx, const DavPath &p);
void DavReport(Ctx &ctx, const DavPath &p);
void DavGet(Ctx &ctx, const DavPath &p, bool head_only);
void DavPut(Ctx &ctx, const DavPath &p);
void DavDelete(Ctx &ctx, const DavPath &p);

} // namespace qmweb
} // namespace duckdb
