#include "dav.hpp"
#include "web_views.hpp"

#include "quackmail/util.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <cstdlib>

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Room;

namespace {

std::vector<std::string> SplitSegments(const std::string &tail) {
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= tail.size()) {
		size_t slash = tail.find('/', pos);
		std::string seg = tail.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
		if (!seg.empty()) {
			out.push_back(seg);
		}
		if (slash == std::string::npos) {
			break;
		}
		pos = slash + 1;
	}
	return out;
}

// Strip ".ics" or ".vcf" off a resource name. A name without one is still a
// valid resource name — some clients invent their own — so the extension is
// optional rather than required.
std::string StripExt(const std::string &name) {
	if (name.size() > 4) {
		std::string ext = quackmail::util::Lower(name.substr(name.size() - 4));
		if (ext == ".ics" || ext == ".vcf" || ext == ".vcs") {
			return name.substr(0, name.size() - 4);
		}
	}
	return name;
}

} // namespace

// ---- path parsing --------------------------------------------------------

DavPath ParseDavPath(const std::string &tail) {
	DavPath p;
	auto seg = SplitSegments(tail);

	if (seg.empty()) {
		p.type = DavRes::Root;
		return p;
	}

	if (seg[0] == "principals") {
		// /dav/principals/users/<user>/ — the "users" level exists because
		// RFC 3744 principal collections are conventionally nested, and clients
		// echo back the href we gave them rather than reconstructing it.
		if (seg.size() == 3 && seg[1] == "users") {
			p.type = DavRes::Principal;
			p.user = seg[2];
		}
		return p;
	}

	if (seg[0] == "calendars") {
		p.kind = DavKind::Calendar;
	} else if (seg[0] == "addressbooks") {
		p.kind = DavKind::AddressBook;
	} else {
		return p;
	}

	if (seg.size() == 1) {
		// /dav/calendars/ with no user is not addressable: a home set belongs to
		// a principal, and we do not publish a directory of accounts.
		return p;
	}
	p.user = seg[1];

	if (seg.size() == 2) {
		p.type = DavRes::Home;
		return p;
	}

	// The room number. strtoll would read "12abc" as 12, so the whole segment
	// has to be digits or this is not a collection path at all.
	const std::string &num = seg[2];
	if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) {
		return p;
	}
	p.room_num = (int64_t)std::strtoll(num.c_str(), nullptr, 10);

	if (seg.size() == 3) {
		p.type = DavRes::Collection;
		return p;
	}
	if (seg.size() == 4) {
		p.type = DavRes::Object;
		p.name = seg[3];
		p.euid = davx::EuidForName(StripExt(seg[3]));
		return p;
	}
	return p; // deeper than the tree goes
}

// ---- hrefs ---------------------------------------------------------------

namespace {

// Path segments are percent-encoded on the way out. A username may contain a
// space, and a room number never needs it, but encoding both keeps one rule.
std::string Seg(const std::string &s) {
	return http::PercentEncode(s);
}

const char *HomeSeg(DavKind kind) {
	return kind == DavKind::AddressBook ? "addressbooks" : "calendars";
}

} // namespace

std::string PrincipalHref(const std::string &user) {
	return "/dav/principals/users/" + Seg(user) + "/";
}

std::string HomeHref(DavKind kind, const std::string &user) {
	return std::string("/dav/") + HomeSeg(kind) + "/" + Seg(user) + "/";
}

std::string CollectionHref(DavKind kind, const std::string &user, int64_t room_num) {
	return HomeHref(kind, user) + std::to_string(room_num) + "/";
}

std::string ObjectHref(DavKind kind, const std::string &user, int64_t room_num, const std::string &name) {
	return CollectionHref(kind, user, room_num) + Seg(name);
}

const char *ObjectExt(DavKind kind) {
	return kind == DavKind::AddressBook ? ".vcf" : ".ics";
}

const char *ObjectMediaType(DavKind kind) {
	return kind == DavKind::AddressBook ? "text/vcard" : "text/calendar";
}

// ---- collections ---------------------------------------------------------

DavKind KindForView(int64_t default_view) {
	switch (default_view) {
	case quackmail::citadel::VIEW_CALENDAR:
	case quackmail::citadel::VIEW_CALBRIEF:
	case quackmail::citadel::VIEW_TASKS:
		return DavKind::Calendar;
	case quackmail::citadel::VIEW_ADDRESSBOOK:
		return DavKind::AddressBook;
	default:
		// Notes are deliberately absent. They are stored as vNote for parity
		// with WebCit and the Citadel clients, and vNote is not a DAV resource
		// type — exposing them as VJOURNAL would mean rewriting them on the way
		// out and losing that parity. Wikis, blogs, journals, mail folders and
		// the spool queue are not DAV collections either.
		return DavKind::None;
	}
}

std::vector<DavCollection> ListCollections(Ctx &ctx, DavKind kind) {
	std::vector<DavCollection> out;
	for (auto &room : quackmail::citadel::ListRooms(ctx.con, ctx.username, -1, "all")) {
		if (KindForView(room.default_view) != kind) {
			continue;
		}
		// A passworded room the user has not unlocked stays out of the listing.
		// DAV has no way to prompt for a room password, so publishing it would
		// only produce a collection whose every object 403s.
		if (!quackmail::citadel::RoomUnlocked(ctx.con, ctx.username, room)) {
			continue;
		}
		DavCollection c;
		c.room = room;
		c.kind = kind;
		c.tasks = room.default_view == quackmail::citadel::VIEW_TASKS;
		out.push_back(std::move(c));
	}
	return out;
}

bool ResolveCollection(Ctx &ctx, const DavPath &p, DavCollection &out) {
	if (p.room_num < 0) {
		return false;
	}
	Room room;
	// ResolveRoomNumFor applies the private-room visibility rules; GetRoomByNum
	// does not, and using it here would be a direct IDOR.
	if (!ResolveRoomNumFor(ctx, p.room_num, room)) {
		return false;
	}
	if (KindForView(room.default_view) != p.kind || p.kind == DavKind::None) {
		return false;
	}
	if (!quackmail::citadel::RoomUnlocked(ctx.con, ctx.username, room)) {
		return false;
	}
	out.room = room;
	out.kind = p.kind;
	out.tasks = room.default_view == quackmail::citadel::VIEW_TASKS;
	return true;
}

// ---- resource naming -----------------------------------------------------

std::string ResourceNameFor(Ctx &ctx, const DavCollection &c, const std::string &euid) {
	auto r = Exec(ctx.con, "SELECT name FROM citadel_dav_names WHERE room_num = $1 AND euid = $2 LIMIT 1",
	              {Value::BIGINT(c.room.room_num), Value(euid)});
	if (r) {
		auto &mat = r->Cast<MaterializedQueryResult>();
		if (mat.RowCount() > 0) {
			return mat.GetValue(0, 0).ToString();
		}
	}
	// The default: what an object created through the web UI or by the native
	// protocol is served as, with no binding row needed.
	return davx::NameForEuid(euid) + ObjectExt(c.kind);
}

std::string EuidForResource(Ctx &ctx, const DavCollection &c, const std::string &name) {
	if (name.empty()) {
		return std::string();
	}
	auto r = Exec(ctx.con, "SELECT euid FROM citadel_dav_names WHERE room_num = $1 AND name = $2",
	              {Value::BIGINT(c.room.room_num), Value(name)});
	if (r) {
		auto &mat = r->Cast<MaterializedQueryResult>();
		if (mat.RowCount() > 0) {
			return mat.GetValue(0, 0).ToString();
		}
	}
	// No binding: read the name as an encoded euid, which is what a client that
	// does use the UID as the resource name produces.
	return davx::EuidForName(StripExt(name));
}

void BindResourceName(Ctx &ctx, const DavCollection &c, const std::string &name,
                      const std::string &euid) {
	if (name.empty() || euid.empty()) {
		return;
	}
	// Only where it differs from what the fallback would already produce. A row
	// per object would be a second thing to keep correct for no gain.
	if (davx::NameForEuid(euid) + ObjectExt(c.kind) == name) {
		UnbindResourceName(ctx, c, name);
		return;
	}
	Exec(ctx.con,
	     "INSERT OR REPLACE INTO citadel_dav_names (room_num, name, euid) VALUES ($1, $2, $3)",
	     {Value::BIGINT(c.room.room_num), Value(name), Value(euid)});
}

void UnbindResourceName(Ctx &ctx, const DavCollection &c, const std::string &name) {
	Exec(ctx.con, "DELETE FROM citadel_dav_names WHERE room_num = $1 AND name = $2",
	     {Value::BIGINT(c.room.room_num), Value(name)});
}

// ---- objects -------------------------------------------------------------

std::vector<DavObject> ListObjects(Ctx &ctx, const DavCollection &c) {
	std::vector<DavObject> out;
	std::string want = ObjectMediaType(c.kind);
	for (int64_t msgnum : quackmail::citadel::RoomMessages(ctx.con, c.room.room_num, "all", 0, 0)) {
		quackmail::citadel::Message msg;
		if (!quackmail::citadel::LoadMessage(ctx.con, msgnum, msg)) {
			continue;
		}
		// ObjectBody returns "" for anything that is not a groupware object of
		// this type, which is how an ordinary message posted into a Calendar
		// room is skipped instead of served as a broken .ics.
		std::string body = ObjectBody(msg, want);
		if (body.empty() || msg.euid.empty()) {
			continue;
		}
		DavObject o;
		o.msgnum = msg.msgnum;
		o.euid = msg.euid;
		o.name = ResourceNameFor(ctx, c, msg.euid);
		o.body = std::move(body);
		o.msgtime = msg.msgtime;
		out.push_back(std::move(o));
	}
	return out;
}

bool LoadObject(Ctx &ctx, const DavCollection &c, const std::string &euid, DavObject &out) {
	if (euid.empty()) {
		return false;
	}
	int64_t msgnum = quackmail::citadel::FindByEuid(ctx.con, c.room.room_num, euid);
	if (msgnum < 0) {
		return false;
	}
	quackmail::citadel::Message msg;
	if (!quackmail::citadel::LoadMessage(ctx.con, msgnum, msg)) {
		return false;
	}
	std::string body = ObjectBody(msg, ObjectMediaType(c.kind));
	if (body.empty()) {
		return false;
	}
	out.msgnum = msg.msgnum;
	out.euid = msg.euid;
	out.name = ResourceNameFor(ctx, c, msg.euid);
	out.body = std::move(body);
	out.msgtime = msg.msgtime;
	return true;
}

bool LoadObjectByName(Ctx &ctx, const DavCollection &c, const std::string &name, DavObject &out) {
	std::string euid = EuidForResource(ctx, c, name);
	if (euid.empty() || !LoadObject(ctx, c, euid, out)) {
		return false;
	}
	// Serve it under the name it was asked for, not the one the binding table
	// happens to prefer — otherwise a client following its own href would be
	// told the resource lives somewhere else.
	out.name = name;
	return true;
}

std::string ETagFor(int64_t msgnum) {
	return "\"" + std::to_string(msgnum) + "\"";
}

std::string SyncToken(Ctx &ctx, int64_t room_num) {
	return "urn:quackcit:sync:" + std::to_string(quackmail::citadel::RoomChangeToken(ctx.con, room_num));
}

int64_t SyncTokenValue(Ctx &ctx, const std::string &token) {
	(void)ctx;
	const std::string prefix = "urn:quackcit:sync:";
	if (token.rfind(prefix, 0) != 0) {
		return -1;
	}
	std::string digits = token.substr(prefix.size());
	if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
		return -1;
	}
	return (int64_t)std::strtoll(digits.c_str(), nullptr, 10);
}

// ---- request and response plumbing ---------------------------------------

bool DavBody(Ctx &ctx, davx::Node &out, bool &empty) {
	empty = true;
	const std::string &body = ctx.req.body;
	// Whitespace-only counts as absent: several clients send a bare newline
	// where the spec says "no body".
	if (body.find_first_not_of(" \t\r\n") == std::string::npos) {
		return false;
	}
	empty = false;
	if (!davx::ParseDoc(body, out)) {
		DavError(ctx, 400, davx::kNsDav, "");
		return false;
	}
	return true;
}

int DavDepth(Ctx &ctx, int dflt) {
	std::string d = quackmail::util::Lower(ctx.req.Header("Depth"));
	if (d.empty()) {
		return dflt;
	}
	if (d == "0") {
		return 0;
	}
	if (d == "1") {
		return 1;
	}
	// "infinity" against a collection whose depth is at most two is the same
	// answer as Depth 1, and refusing it outright breaks clients that ask for it
	// out of habit.
	return 1;
}

void DavStatus(Ctx &ctx, int status) {
	ctx.resp.status = status;
	ctx.resp.body.clear();
	ctx.resp.SetHeader("Cache-Control", "no-store");
	ctx.resp.SetHeader("X-Content-Type-Options", "nosniff");
}

void DavError(Ctx &ctx, int status, const std::string &ns, const std::string &condition) {
	davx::Writer w;
	w.StartDoc(davx::kNsDav, "error");
	if (!condition.empty()) {
		w.Empty(ns, condition);
	}
	w.Close();
	ctx.resp.Bytes(w.Str(), "application/xml; charset=utf-8");
	ctx.resp.status = status;
	ctx.resp.SetHeader("Cache-Control", "no-store");
	ctx.resp.SetHeader("X-Content-Type-Options", "nosniff");
}

void SendMultiStatus(Ctx &ctx, const std::string &xml) {
	ctx.resp.Bytes(xml, "application/xml; charset=utf-8");
	ctx.resp.status = 207;
	ctx.resp.SetHeader("Cache-Control", "no-store");
	ctx.resp.SetHeader("X-Content-Type-Options", "nosniff");
}

// ---- the verbs -----------------------------------------------------------

namespace {

// What OPTIONS advertises, and what a 405 lists.
//
// The compliance classes are exactly the ones we implement, and no more: a
// class we claim but do not honour is one a client keeps trying to use.
//
//   1  RFC 4918 core. **Not 2** — that promises LOCK and UNLOCK, and CalDAV's
//      consistency story is ETags and If-Match, which is implemented. The real
//      Citadel's webcit-ng does claim "1, 2" here; we do not, deliberately.
//   3  RFC 4918 rather than its 2518 predecessor, as webcit-ng also says.
//   calendar-access  RFC 4791 §5.1.
//   addressbook      RFC 6352 §6.1. Note the name: it is not
//                    "addressbook-access", however symmetrical that would be.
//                    webcit-ng/server/room_propfind.c agrees.
//
// Absent on purpose: access-control (RFC 3744 — we serve its properties but
// implement no ACL method) and extended-mkcol (we have no MKCOL at all).
const char *const kDavHeader = "1, 3, calendar-access, addressbook";
const char *const kAllowHeader = "OPTIONS, PROPFIND, PROPPATCH, REPORT, GET, HEAD, PUT, DELETE";

void DavOptions(Ctx &ctx) {
	ctx.resp.status = 204;
	ctx.resp.body.clear();
	ctx.resp.SetHeader("DAV", kDavHeader);
	ctx.resp.SetHeader("Allow", kAllowHeader);
	ctx.resp.SetHeader("Cache-Control", "no-store");
}

// The whole /dav/ subtree. Registered as one route with method "*" so the ten
// verbs are one linear-scan entry rather than ten.
void DavHandler(Ctx &ctx) {
	// Advertised on every response, not only on OPTIONS: some clients sniff for
	// the header on the first request they happen to make.
	ctx.resp.SetHeader("DAV", kDavHeader);

	const std::string &method = ctx.req.method;
	if (method == "OPTIONS") {
		DavOptions(ctx);
		return;
	}

	DavPath p = ParseDavPath(ctx.Cap(0));
	if (p.type == DavRes::None) {
		DavStatus(ctx, 404);
		return;
	}
	// Every path below the root names a user, and it is always the signed-in
	// one. Another account's collections are not reachable by editing the URL —
	// sharing is expressed through the room's ACL, which puts the shared room in
	// *this* user's home rather than putting this user in someone else's tree.
	if (!p.user.empty() && p.user != ctx.username) {
		DavStatus(ctx, 403);
		return;
	}

	if (method == "PROPFIND") {
		DavPropfind(ctx, p);
	} else if (method == "PROPPATCH") {
		DavProppatch(ctx, p);
	} else if (method == "REPORT") {
		DavReport(ctx, p);
	} else if (method == "GET" || method == "HEAD") {
		DavGet(ctx, p, method == "HEAD");
	} else if (method == "PUT") {
		DavPut(ctx, p);
	} else if (method == "DELETE") {
		DavDelete(ctx, p);
	} else {
		// MKCOL, MKCALENDAR, COPY and MOVE reach the allowlist but stop here.
		// Creating a calendar means creating a room, which has a floor, a name
		// and an access level to decide; that belongs in the room-admin UI, not
		// in a verb with no way to ask.
		ctx.resp.SetHeader("Allow", kAllowHeader);
		DavStatus(ctx, 405);
	}
}

// Discovery. RFC 6764: a client given only an address resolves it to a
// well-known path, and expects to be sent onward to the real root.
void WellKnown(Ctx &ctx) {
	// 301, so the client remembers and stops asking.
	ctx.resp.Redirect("/dav/", 301);
	ctx.resp.SetHeader("Cache-Control", "no-store");
}

} // namespace

void RegisterDavRoutes(std::vector<Route> &out) {
	// Anonymous, because they resolve to a redirect and nothing else: a client
	// that has to authenticate before it can even find the endpoint cannot
	// perform the bootstrap RFC 6764 describes.
	// "*" rather than GET: a client bootstrapping under RFC 6764 sends whatever
	// verb it intended for the real endpoint — usually PROPFIND — and expects the
	// redirect to come back regardless.
	out.push_back({"*", "/.well-known/caldav", Role::Anon, WellKnown});
	out.push_back({"*", "/.well-known/carddav", Role::Anon, WellKnown});
	out.push_back({"*", "/dav/*", Role::Dav, DavHandler});
}

} // namespace qmweb
} // namespace duckdb
