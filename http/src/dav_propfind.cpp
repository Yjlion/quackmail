#include "dav.hpp"

#include "quackmail/util.hpp"

#include <cstdio>
#include <ctime>

namespace duckdb {
namespace qmweb {

namespace {

// An HTTP-date, which is RFC 1123 in GMT and nothing else — not the RFC 5322
// form util::RfcDate produces, whose "+0000" a strict client will reject.
std::string HttpDate(int64_t epoch) {
	static const char *const kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char *const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	time_t t = (time_t)epoch;
	struct tm tm_utc;
#ifdef _WIN32
	gmtime_s(&tm_utc, &t);
#else
	gmtime_r(&t, &tm_utc);
#endif
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT", kDays[tm_utc.tm_wday],
	              tm_utc.tm_mday, kMonths[tm_utc.tm_mon], tm_utc.tm_year + 1900, tm_utc.tm_hour,
	              tm_utc.tm_min, tm_utc.tm_sec);
	return buf;
}

// The user preference a per-collection decoration is stored under. These are
// client chrome — a colour, a sort position — so they belong to the viewer
// rather than to the room, and citadel_user_prefs is exactly that table.
std::string DecorKey(const char *prop, int64_t room_num) {
	return std::string("dav_") + prop + "_" + std::to_string(room_num);
}

// The RFC 3744 privileges implied by a room's RFC 4314 rights letters.
void WritePrivileges(davx::Writer &w, const std::string &rights, bool is_collection) {
	auto has = [&](char c) { return rights.find(c) != std::string::npos; };
	auto priv = [&](const char *name) {
		w.Open(davx::kNsDav, "privilege");
		w.Empty(davx::kNsDav, name);
		w.Close();
	};
	if (has('r')) {
		priv("read");
	}
	// Reading a property is reading; there is no separate grant for it in
	// Citadel's model.
	if (has('r')) {
		priv("read-current-user-privilege-set");
	}
	if (has('p') || has('i')) {
		priv("write-content");
		if (is_collection) {
			priv("bind");
		}
	}
	if (has('t') || has('e')) {
		priv("unbind");
	}
	if (has('a')) {
		priv("write-properties");
		priv("write");
	}
}

// Emit one property with its value. Returns false — writing nothing — when the
// resource does not have it, which is what puts it in the 404 propstat.
bool EmitProp(Ctx &ctx, davx::Writer &w, const PropSource &src, const std::string &ns,
              const std::string &name) {
	const bool is_dav = ns == davx::kNsDav;
	const bool is_cal = ns == davx::kNsCalDav;
	const bool is_card = ns == davx::kNsCardDav;
	const bool is_cs = ns == davx::kNsCalServer;
	const bool is_apple = ns == davx::kNsApple;

	const bool collection = src.type != DavRes::Object;
	const bool is_calendar_coll = src.type == DavRes::Collection && src.kind == DavKind::Calendar;
	const bool is_card_coll = src.type == DavRes::Collection && src.kind == DavKind::AddressBook;

	// ---- DAV: ------------------------------------------------------------
	if (is_dav && name == "resourcetype") {
		w.Open(ns, name);
		if (collection) {
			w.Empty(davx::kNsDav, "collection");
		}
		if (src.type == DavRes::Principal) {
			w.Empty(davx::kNsDav, "principal");
		}
		if (is_calendar_coll) {
			w.Empty(davx::kNsCalDav, "calendar");
		}
		if (is_card_coll) {
			w.Empty(davx::kNsCardDav, "addressbook");
		}
		w.Close();
		return true;
	}
	if (is_dav && name == "displayname") {
		std::string label;
		switch (src.type) {
		case DavRes::Root:
			label = ConfigStr(ctx.con, "c_humannode", "QuackCit");
			break;
		case DavRes::Principal:
			label = src.user;
			break;
		case DavRes::Home:
			label = src.kind == DavKind::AddressBook ? "Address Books" : "Calendars";
			break;
		case DavRes::Collection:
			label = src.coll ? src.coll->room.display_name : std::string();
			break;
		default:
			return false; // an object has no display name of its own
		}
		w.TextElem(ns, name, label);
		return true;
	}
	if (is_dav && (name == "current-user-principal" || name == "principal-URL" || name == "owner")) {
		if (name == "principal-URL" && src.type != DavRes::Principal) {
			return false;
		}
		w.Open(ns, name);
		w.TextElem(davx::kNsDav, "href", PrincipalHref(ctx.username));
		w.Close();
		return true;
	}
	if (is_dav && name == "principal-collection-set") {
		w.Open(ns, name);
		w.TextElem(davx::kNsDav, "href", "/dav/principals/users/");
		w.Close();
		return true;
	}
	if (is_dav && name == "current-user-privilege-set") {
		std::string rights = src.coll ? quackmail::citadel::EffectiveRights(ctx.con, ctx.username, src.coll->room)
		                              : std::string("lr");
		w.Open(ns, name);
		WritePrivileges(w, rights, collection);
		w.Close();
		return true;
	}
	if (is_dav && name == "supported-report-set") {
		w.Open(ns, name);
		auto report = [&](const std::string &rns, const char *rname) {
			w.Open(davx::kNsDav, "supported-report");
			w.Open(davx::kNsDav, "report");
			w.Empty(rns, rname);
			w.Close();
			w.Close();
		};
		if (src.type == DavRes::Collection) {
			report(davx::kNsDav, "sync-collection");
			if (is_calendar_coll) {
				report(davx::kNsCalDav, "calendar-query");
				report(davx::kNsCalDav, "calendar-multiget");
			}
			if (is_card_coll) {
				report(davx::kNsCardDav, "addressbook-query");
				report(davx::kNsCardDav, "addressbook-multiget");
			}
		}
		w.Close();
		return true;
	}
	if (is_dav && name == "sync-token") {
		if (src.type != DavRes::Collection || !src.coll) {
			return false;
		}
		w.TextElem(ns, name, SyncToken(ctx, src.coll->room.room_num));
		return true;
	}
	if (is_dav && name == "getetag") {
		if (!src.obj) {
			return false;
		}
		w.TextElem(ns, name, ETagFor(src.obj->msgnum));
		return true;
	}
	if (is_dav && name == "getcontenttype") {
		if (!src.obj) {
			return false;
		}
		std::string type = std::string(ObjectMediaType(src.kind)) + "; charset=utf-8";
		if (src.kind == DavKind::Calendar && src.coll) {
			// Apple's clients use this to tell a task list from a calendar
			// without opening the object.
			type += src.coll->tasks ? "; component=vtodo" : "; component=vevent";
		}
		w.TextElem(ns, name, type);
		return true;
	}
	if (is_dav && name == "getcontentlength") {
		if (!src.obj) {
			return false;
		}
		w.TextElem(ns, name, std::to_string(src.obj->body.size()));
		return true;
	}
	if (is_dav && name == "getlastmodified") {
		if (!src.obj) {
			return false;
		}
		w.TextElem(ns, name, HttpDate(src.obj->msgtime));
		return true;
	}

	// ---- CalDAV ----------------------------------------------------------
	if (is_cal && name == "calendar-home-set") {
		if (src.type != DavRes::Principal) {
			return false;
		}
		w.Open(ns, name);
		w.TextElem(davx::kNsDav, "href", HomeHref(DavKind::Calendar, ctx.username));
		w.Close();
		return true;
	}
	if (is_cal && name == "calendar-user-address-set") {
		if (src.type != DavRes::Principal) {
			return false;
		}
		w.Open(ns, name);
		w.TextElem(davx::kNsDav, "href",
		           "mailto:" + ctx.username + "@" + ConfigStr(ctx.con, "c_fqdn", "localhost"));
		w.TextElem(davx::kNsDav, "href", PrincipalHref(ctx.username));
		w.Close();
		return true;
	}
	if (is_cal && name == "supported-calendar-component-set") {
		if (!is_calendar_coll || !src.coll) {
			return false;
		}
		w.Open(ns, name);
		w.Open(davx::kNsCalDav, "comp");
		w.Attr("name", src.coll->tasks ? "VTODO" : "VEVENT");
		w.Close();
		w.Close();
		return true;
	}
	if (is_cal && name == "supported-calendar-data") {
		if (!is_calendar_coll) {
			return false;
		}
		w.Open(ns, name);
		w.Open(davx::kNsCalDav, "calendar-data");
		w.Attr("content-type", "text/calendar");
		w.Attr("version", "2.0");
		w.Close();
		w.Close();
		return true;
	}
	if (is_cal && name == "calendar-description") {
		if (!is_calendar_coll || !src.coll) {
			return false;
		}
		w.TextElem(ns, name, src.coll->room.info);
		return true;
	}
	if (is_cal && name == "max-resource-size") {
		if (!is_calendar_coll) {
			return false;
		}
		// The codec's body ceiling. Saying so lets a client refuse an oversized
		// object itself rather than discovering it through a 413.
		w.TextElem(ns, name, std::to_string(http::Limits().max_body));
		return true;
	}
	if (is_cal && name == "calendar-data") {
		if (!src.obj || !src.want_data) {
			return false;
		}
		w.TextElem(ns, name, src.obj->body);
		return true;
	}

	// ---- CardDAV ---------------------------------------------------------
	if (is_card && name == "addressbook-home-set") {
		if (src.type != DavRes::Principal) {
			return false;
		}
		w.Open(ns, name);
		w.TextElem(davx::kNsDav, "href", HomeHref(DavKind::AddressBook, ctx.username));
		w.Close();
		return true;
	}
	if (is_card && name == "supported-address-data") {
		if (!is_card_coll) {
			return false;
		}
		w.Open(ns, name);
		for (const char *v : {"3.0", "4.0"}) {
			w.Open(davx::kNsCardDav, "address-data-type");
			w.Attr("content-type", "text/vcard");
			w.Attr("version", v);
			w.Close();
		}
		w.Close();
		return true;
	}
	if (is_card && name == "addressbook-description") {
		if (!is_card_coll || !src.coll) {
			return false;
		}
		w.TextElem(ns, name, src.coll->room.info);
		return true;
	}
	if (is_card && name == "max-resource-size") {
		if (!is_card_coll) {
			return false;
		}
		w.TextElem(ns, name, std::to_string(http::Limits().max_body));
		return true;
	}
	if (is_card && name == "address-data") {
		if (!src.obj || !src.want_data) {
			return false;
		}
		w.TextElem(ns, name, src.obj->body);
		return true;
	}

	// ---- calendarserver.org ----------------------------------------------
	if (is_cs && name == "getctag") {
		if (src.type != DavRes::Collection || !src.coll) {
			return false;
		}
		// The same number as sync-token. A ctag is the older, coarser mechanism
		// — "has anything changed" rather than "what changed" — and a client
		// that polls it re-reads the whole collection when it moves.
		w.TextElem(ns, name, SyncToken(ctx, src.coll->room.room_num));
		return true;
	}

	// ---- Apple decoration -------------------------------------------------
	if (is_apple && (name == "calendar-color" || name == "calendar-order")) {
		if (src.type != DavRes::Collection || !src.coll) {
			return false;
		}
		std::string v = quackmail::citadel::GetUserPref(ctx.con, ctx.username,
		                                                DecorKey(name.c_str(), src.coll->room.room_num), "");
		if (v.empty()) {
			return false;
		}
		w.TextElem(ns, name, v);
		return true;
	}

	return false;
}

// What allprop returns. Deliberately not everything: RFC 4918 says allprop
// covers the live DAV: properties, and RFC 4791 says the CalDAV ones are only
// returned when named. Clients that want them ask.
std::vector<std::pair<std::string, std::string>> AllPropNames(const PropSource &src) {
	std::vector<std::pair<std::string, std::string>> out;
	auto add = [&](const char *ns, const char *name) { out.emplace_back(ns, name); };
	add(davx::kNsDav, "resourcetype");
	add(davx::kNsDav, "displayname");
	add(davx::kNsDav, "current-user-principal");
	if (src.type == DavRes::Principal) {
		add(davx::kNsDav, "principal-URL");
		add(davx::kNsCalDav, "calendar-home-set");
		add(davx::kNsCardDav, "addressbook-home-set");
	}
	if (src.type == DavRes::Collection) {
		add(davx::kNsDav, "sync-token");
		add(davx::kNsDav, "supported-report-set");
		add(davx::kNsDav, "current-user-privilege-set");
		add(davx::kNsCalServer, "getctag");
		if (src.kind == DavKind::Calendar) {
			add(davx::kNsCalDav, "supported-calendar-component-set");
		}
	}
	if (src.type == DavRes::Object) {
		add(davx::kNsDav, "getetag");
		add(davx::kNsDav, "getcontenttype");
		add(davx::kNsDav, "getcontentlength");
		add(davx::kNsDav, "getlastmodified");
	}
	return out;
}

} // namespace

bool PropRequest::Wants(const char *ns, const char *name) const {
	if (allprop) {
		return true;
	}
	for (const auto &p : props) {
		if (p.first == ns && p.second == name) {
			return true;
		}
	}
	return false;
}

PropRequest ParsePropRequest(const davx::Node &root, bool body_present) {
	PropRequest pr;
	if (!body_present) {
		// RFC 4918: a PROPFIND with no body means allprop.
		pr.allprop = true;
		return pr;
	}
	if (root.Child(davx::kNsDav, "propname")) {
		pr.propname = true;
		return pr;
	}
	const davx::Node *prop = root.Child(davx::kNsDav, "prop");
	if (!prop) {
		pr.allprop = true;
		return pr;
	}
	for (const auto &child : prop->children) {
		pr.props.emplace_back(child.ns, child.name);
	}
	if (pr.props.empty()) {
		pr.allprop = true;
	}
	return pr;
}

void WriteResponse(Ctx &ctx, davx::Writer &w, const PropSource &src, const PropRequest &pr) {
	w.Open(davx::kNsDav, "response");
	w.TextElem(davx::kNsDav, "href", src.href);

	auto wanted = pr.allprop || pr.propname ? AllPropNames(src) : pr.props;

	// Each property is rendered into a scratch writer so that "does this
	// resource have it" and "what is its value" are decided by one function
	// rather than by two that can drift apart.
	std::string found;
	std::vector<std::pair<std::string, std::string>> missing;
	for (const auto &p : wanted) {
		davx::Writer scratch;
		if (pr.propname) {
			// propname asks for the names alone, with no values.
			scratch.Empty(p.first, p.second);
			found += scratch.Str();
			continue;
		}
		if (EmitProp(ctx, scratch, src, p.first, p.second)) {
			found += scratch.Str();
		} else {
			missing.push_back(p);
		}
	}

	if (!found.empty()) {
		w.Open(davx::kNsDav, "propstat");
		w.Open(davx::kNsDav, "prop");
		w.RawFragment(found);
		w.Close();
		w.TextElem(davx::kNsDav, "status", davx::StatusLine(200));
		w.Close();
	}
	if (!missing.empty()) {
		w.Open(davx::kNsDav, "propstat");
		w.Open(davx::kNsDav, "prop");
		for (const auto &p : missing) {
			w.Empty(p.first, p.second);
		}
		w.Close();
		w.TextElem(davx::kNsDav, "status", davx::StatusLine(404));
		w.Close();
	}
	w.Close();
}

void WriteGoneResponse(davx::Writer &w, const std::string &href) {
	w.Open(davx::kNsDav, "response");
	w.TextElem(davx::kNsDav, "href", href);
	w.TextElem(davx::kNsDav, "status", davx::StatusLine(404));
	w.Close();
}

// ---- PROPFIND ------------------------------------------------------------

void DavPropfind(Ctx &ctx, const DavPath &p) {
	bool empty = false;
	davx::Node root;
	if (!DavBody(ctx, root, empty) && !empty) {
		return; // DavBody rendered the 400
	}
	PropRequest pr = ParsePropRequest(root, !empty);
	int depth = DavDepth(ctx, 0);

	davx::Writer w;
	w.StartDoc(davx::kNsDav, "multistatus");

	switch (p.type) {
	case DavRes::Root: {
		PropSource src;
		src.href = "/dav/";
		src.type = DavRes::Root;
		src.user = ctx.username;
		WriteResponse(ctx, w, src, pr);
		if (depth >= 1) {
			PropSource principal;
			principal.href = PrincipalHref(ctx.username);
			principal.type = DavRes::Principal;
			principal.user = ctx.username;
			WriteResponse(ctx, w, principal, pr);
		}
		break;
	}
	case DavRes::Principal: {
		PropSource src;
		src.href = PrincipalHref(ctx.username);
		src.type = DavRes::Principal;
		src.user = ctx.username;
		WriteResponse(ctx, w, src, pr);
		break;
	}
	case DavRes::Home: {
		PropSource src;
		src.href = HomeHref(p.kind, ctx.username);
		src.type = DavRes::Home;
		src.kind = p.kind;
		src.user = ctx.username;
		WriteResponse(ctx, w, src, pr);
		if (depth >= 1) {
			// The listing a client walks on first connection: every calendar or
			// address book this account can reach, including shared rooms it has
			// been granted rights to rather than only its own.
			for (const auto &c : ListCollections(ctx, p.kind)) {
				PropSource cs;
				cs.href = CollectionHref(p.kind, ctx.username, c.room.room_num);
				cs.type = DavRes::Collection;
				cs.kind = p.kind;
				cs.user = ctx.username;
				cs.coll = &c;
				WriteResponse(ctx, w, cs, pr);
			}
		}
		break;
	}
	case DavRes::Collection: {
		DavCollection c;
		if (!ResolveCollection(ctx, p, c)) {
			DavStatus(ctx, 404);
			return;
		}
		PropSource src;
		src.href = CollectionHref(p.kind, ctx.username, c.room.room_num);
		src.type = DavRes::Collection;
		src.kind = p.kind;
		src.user = ctx.username;
		src.coll = &c;
		WriteResponse(ctx, w, src, pr);
		if (depth >= 1) {
			for (const auto &o : ListObjects(ctx, c)) {
				PropSource os;
				os.href = ObjectHref(p.kind, ctx.username, c.room.room_num, o.euid);
				os.type = DavRes::Object;
				os.kind = p.kind;
				os.user = ctx.username;
				os.coll = &c;
				os.obj = &o;
				// Never here: RFC 4791 is explicit that PROPFIND does not carry
				// calendar data. A client that wants bodies sends a REPORT.
				os.want_data = false;
				WriteResponse(ctx, w, os, pr);
			}
		}
		break;
	}
	case DavRes::Object: {
		DavCollection c;
		DavObject o;
		if (!ResolveCollection(ctx, p, c) || !LoadObject(ctx, c, p.euid, o)) {
			DavStatus(ctx, 404);
			return;
		}
		PropSource src;
		src.href = ObjectHref(p.kind, ctx.username, c.room.room_num, o.euid);
		src.type = DavRes::Object;
		src.kind = p.kind;
		src.user = ctx.username;
		src.coll = &c;
		src.obj = &o;
		WriteResponse(ctx, w, src, pr);
		break;
	}
	default:
		DavStatus(ctx, 404);
		return;
	}

	w.Close();
	SendMultiStatus(ctx, w.Str());
}

// ---- PROPPATCH -----------------------------------------------------------

void DavProppatch(Ctx &ctx, const DavPath &p) {
	if (p.type != DavRes::Collection) {
		DavStatus(ctx, 403);
		return;
	}
	DavCollection c;
	if (!ResolveCollection(ctx, p, c)) {
		DavStatus(ctx, 404);
		return;
	}

	bool empty = false;
	davx::Node root;
	if (!DavBody(ctx, root, empty)) {
		if (empty) {
			DavError(ctx, 400, davx::kNsDav, "");
		}
		return;
	}

	// Every property the request touched, with the status it got. A client
	// learns what it may set from the per-property statuses, so a property we
	// quietly ignored would be one it goes on sending forever.
	std::vector<std::pair<std::pair<std::string, std::string>, int>> results;

	auto apply = [&](const davx::Node &prop, bool removing) {
		for (const auto &item : prop.children) {
			int status = 403;
			if (item.ns == davx::kNsDav && item.name == "displayname") {
				// Renaming a collection renames the room, so it takes the same
				// right renaming it through the web UI takes.
				if (quackmail::citadel::CanAdminister(ctx.con, ctx.username, c.room)) {
					quackmail::citadel::Room updated = c.room;
					updated.display_name = removing ? c.room.display_name : item.text;
					std::string err;
					status = quackmail::citadel::UpdateRoom(ctx.con, updated, err) ? 200 : 409;
				}
			} else if (item.ns == davx::kNsApple &&
			           (item.name == "calendar-color" || item.name == "calendar-order")) {
				// Viewer chrome, stored per user: two people sharing a calendar
				// are each entitled to their own colour for it.
				quackmail::citadel::SetUserPref(ctx.con, ctx.username,
				                                DecorKey(item.name.c_str(), c.room.room_num),
				                                removing ? std::string() : item.text);
				status = 200;
			}
			results.push_back({{item.ns, item.name}, status});
		}
	};

	for (const auto *set : root.Children(davx::kNsDav, "set")) {
		if (const davx::Node *prop = set->Child(davx::kNsDav, "prop")) {
			apply(*prop, false);
		}
	}
	for (const auto *rm : root.Children(davx::kNsDav, "remove")) {
		if (const davx::Node *prop = rm->Child(davx::kNsDav, "prop")) {
			apply(*prop, true);
		}
	}

	davx::Writer w;
	w.StartDoc(davx::kNsDav, "multistatus");
	w.Open(davx::kNsDav, "response");
	w.TextElem(davx::kNsDav, "href", CollectionHref(p.kind, ctx.username, c.room.room_num));
	for (int want : {200, 403, 409}) {
		bool any = false;
		for (const auto &r : results) {
			if (r.second == want) {
				any = true;
				break;
			}
		}
		if (!any) {
			continue;
		}
		w.Open(davx::kNsDav, "propstat");
		w.Open(davx::kNsDav, "prop");
		for (const auto &r : results) {
			if (r.second == want) {
				w.Empty(r.first.first, r.first.second);
			}
		}
		w.Close();
		w.TextElem(davx::kNsDav, "status", davx::StatusLine(want));
		w.Close();
	}
	w.Close();
	w.Close();
	SendMultiStatus(ctx, w.Str());
}

} // namespace qmweb
} // namespace duckdb
