#include "dav.hpp"

#include "quackmail/util.hpp"

namespace duckdb {
namespace qmweb {

namespace {

namespace citadel = quackmail::citadel;

// The view a new collection gets, from what the client said it was for.
//
// This is KindForView run backwards, and it is the only place the mapping goes
// that direction — a client says "this holds VTODOs" and the room has to come
// out with VIEW_TASKS, or the collection it just made will not resolve as the
// kind it asked for.
//
// `set` is <C:supported-calendar-component-set> if the client sent one.
int64_t ViewForRequest(DavKind kind, const davx::Node *set) {
	if (kind == DavKind::AddressBook) {
		return citadel::VIEW_ADDRESSBOOK;
	}
	if (set) {
		bool todo = false, event = false;
		for (const auto *comp : set->Children(davx::kNsCalDav, "comp")) {
			std::string name = quackmail::util::Upper(comp->Attr("name"));
			if (name == "VTODO") {
				todo = true;
			} else if (name == "VEVENT") {
				event = true;
			}
		}
		// A collection asked to hold both is a calendar: CalDAV has no separate
		// task collection, and a room with VIEW_TASKS is offered to clients
		// asking for VTODOs only.
		if (todo && !event) {
			return citadel::VIEW_TASKS;
		}
	}
	return citadel::VIEW_CALENDAR;
}

// Which kind of collection a request is asking to create.
//
// MKCALENDAR (RFC 4791 §5.3.1) says it by being MKCALENDAR. Extended MKCOL
// (RFC 5689) says it in <D:resourcetype>, which is the form a CardDAV client
// has to use because there is no MKADDRESSBOOK.
bool KindForRequest(const std::string &method, const davx::Node &root, bool body_present,
                    DavKind path_kind, DavKind &out) {
	if (method == "MKCALENDAR") {
		// A MKCALENDAR under the addressbook home is a contradiction; the path
		// and the verb have to agree.
		if (path_kind != DavKind::Calendar) {
			return false;
		}
		out = DavKind::Calendar;
		return true;
	}

	// Plain MKCOL with no body: the path decides, which is the only thing left
	// to go on and is unambiguous here because the two homes are separate.
	if (!body_present) {
		out = path_kind;
		return path_kind != DavKind::None;
	}

	const davx::Node *props = nullptr;
	if (const davx::Node *set = root.Child(davx::kNsDav, "set")) {
		props = set->Child(davx::kNsDav, "prop");
	}
	const davx::Node *rt = props ? props->Child(davx::kNsDav, "resourcetype") : nullptr;
	if (!rt) {
		out = path_kind;
		return path_kind != DavKind::None;
	}
	if (rt->Child(davx::kNsCalDav, "calendar")) {
		out = DavKind::Calendar;
	} else if (rt->Child(davx::kNsCardDav, "addressbook")) {
		out = DavKind::AddressBook;
	} else {
		// <D:collection/> alone, or something we do not serve. A plain WebDAV
		// collection is not a thing this server has — every collection is a
		// room with a view — so this is refused rather than silently made into
		// a calendar.
		return false;
	}
	// The body and the path must agree, for the same reason as MKCALENDAR.
	return out == path_kind;
}

// The <D:set><D:prop> a MKCALENDAR or extended MKCOL carries, applied to the
// room that was just made.
//
// Deliberately the *same* properties PROPPATCH understands, applied through the
// same helper: a client that creates a calendar with a colour and a client that
// creates one and then PROPPATCHes a colour onto it must end up with the same
// room. Two code paths here would be two answers to that.
void ApplyCreationProps(Ctx &ctx, const davx::Node &root, DavCollection &c) {
	const davx::Node *set = root.Child(davx::kNsDav, "set");
	const davx::Node *props = set ? set->Child(davx::kNsDav, "prop") : nullptr;
	if (!props) {
		return;
	}
	for (const auto &child : props->children) {
		ApplyCollectionProp(ctx, c, child, false);
	}
}

// The floor a new collection lands on.
//
// MKCOL carries no floor — DAV has no such concept — so it has to be chosen. A
// client's other groupware rooms are the best guess available: a second
// calendar belongs beside the first one rather than on whatever floor happens
// to be numbered zero.
int64_t FloorForNewCollection(Ctx &ctx, DavKind kind) {
	for (const auto &c : ListCollections(ctx, kind)) {
		return c.room.floor_num;
	}
	return 0; // Main Floor
}

} // namespace

// MKCOL (RFC 4918 §9.3, RFC 5689) and MKCALENDAR (RFC 4791 §5.3.1).
//
// A collection here is a *room*, so this is the DAV spelling of the web UI's
// "create a room" — and it goes through exactly the same steps in the same
// order, because the interesting part of creating a room is not the INSERT, it
// is the rights grant afterwards that makes the creator its administrator.
void DavMkcol(Ctx &ctx, const DavPath &p) {
	// A collection is created *at* a URL that does not exist yet, which is
	// precisely the path shape ParseDavPath reports as a collection with no
	// room behind it. Anything else — the root, a home, an object path — is not
	// somewhere a collection can be made.
	if (p.type != DavRes::Collection || p.kind == DavKind::None || p.segment.empty()) {
		DavStatus(ctx, 403);
		return;
	}

	// RFC 4918 §9.3.1: MKCOL on an existing resource is 405, not 409. A client
	// retrying a request whose response it lost depends on telling those apart.
	DavCollection existing;
	if (ResolveCollection(ctx, p, existing)) {
		ctx.resp.SetHeader("Allow", kAllowHeader);
		DavStatus(ctx, 405);
		return;
	}
	// A purely numeric segment names the room-number space, which the server
	// allocates. Letting a client create "42" would either collide with a real
	// room number now or shadow one allocated later.
	if (p.segment.find_first_not_of("0123456789") == std::string::npos) {
		DavError(ctx, 403, davx::kNsDav, "valid-resourcetype");
		return;
	}

	davx::Node root;
	bool empty = true;
	if (!DavBody(ctx, root, empty)) {
		if (!empty) {
			return; // DavBody rendered the 400 itself
		}
	}

	DavKind kind = DavKind::None;
	if (!KindForRequest(ctx.req.method, root, !empty, p.kind, kind)) {
		DavError(ctx, 403, davx::kNsDav, "valid-resourcetype");
		return;
	}

	int64_t floor = FloorForNewCollection(ctx, kind);
	if (!MayCreateRoomOnFloor(ctx, floor)) {
		DavError(ctx, 403, davx::kNsDav, "need-privileges");
		return;
	}

	// The room's display name defaults to the segment the client chose. A
	// <D:displayname> in the body replaces it below, through the same PROPPATCH
	// path that would set it afterwards.
	std::string name = davx::EuidForName(p.segment);
	if (name.empty()) {
		DavStatus(ctx, 400);
		return;
	}
	citadel::Room clash;
	if (citadel::ResolveRoom(ctx.con, ctx.username, name, clash)) {
		// The URL is free but the room name is taken. 409 rather than 405: the
		// conflict is with something the client cannot see at this URL.
		DavError(ctx, 409, davx::kNsDav, "resource-must-be-null");
		return;
	}

	std::string err;
	int64_t room_num = citadel::CreateRoom(ctx.con, name, floor, 0, "", 0, err);
	if (room_num < 0) {
		DavStatus(ctx, 409);
		return;
	}

	citadel::Room room;
	if (!citadel::GetRoomByNum(ctx.con, room_num, room)) {
		DavStatus(ctx, 500);
		return;
	}
	// CreateRoom takes no view; it is set here, as everywhere else in the tree.
	// Without it the collection would resolve as DavKind::None and 404 at the
	// URL the 201 just promised.
	room.default_view = ViewForRequest(kind, root.Child(davx::kNsCalDav, "supported-calendar-component-set"));
	citadel::UpdateRoom(ctx.con, room, err);

	// The creator's grant. Without it a non-aide holds derived rights only, and
	// would not be able to write to the collection they just created.
	if (!citadel::SetRights(ctx.con, room, ctx.username, citadel::kAclRights, err)) {
		DavStatus(ctx, 500);
		return;
	}

	// Bind the client's URL to the room, so the collection is served where the
	// client put it rather than at the number the server happened to allocate.
	Exec(ctx.con,
	     "INSERT OR REPLACE INTO citadel_dav_collections (username, segment, room_num) "
	     "VALUES ($1, $2, $3)",
	     {Value(ctx.username), Value(p.segment), Value::BIGINT(room_num)});

	DavCollection made;
	made.room = room;
	made.kind = kind;
	made.segment = p.segment;
	made.tasks = room.default_view == citadel::VIEW_TASKS;
	if (!empty) {
		ApplyCreationProps(ctx, root, made);
	}

	AideLog(ctx, "Room created: " + room.display_name,
	        "A collection was created over " + std::string(ctx.req.method == "MKCALENDAR" ? "MKCALENDAR" : "MKCOL") +
	            ".\n\nRoom: " + room.display_name + "\nAdministrator: " + ctx.username);

	ctx.resp.status = 201;
	ctx.resp.body.clear();
	ctx.resp.SetHeader("Cache-Control", "no-store");
}

} // namespace qmweb
} // namespace duckdb
