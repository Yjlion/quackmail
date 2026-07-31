#include "dav.hpp"

#include "quackmail/ical.hpp"
#include "quackmail/util.hpp"
#include "quackmail/vcard.hpp"

#include <cstdlib>
#include <ctime>

namespace duckdb {
namespace qmweb {

namespace {

namespace ical = quackmail::ical;
namespace vcard = quackmail::vcard;

// Parse an iCalendar UTC timestamp of the form a time-range filter carries:
// "20260315T090000Z". Returns false for anything else, which the caller treats
// as "no bound" rather than as an error — a half-specified range is still a
// range.
bool ParseIcalUtc(const std::string &v, int64_t &out) {
	if (v.size() < 15 || v[8] != 'T') {
		return false;
	}
	auto num = [&](size_t at, size_t len) {
		return (int)std::strtol(v.substr(at, len).c_str(), nullptr, 10);
	};
	struct tm tm_v {};
	tm_v.tm_year = num(0, 4) - 1900;
	tm_v.tm_mon = num(4, 2) - 1;
	tm_v.tm_mday = num(6, 2);
	tm_v.tm_hour = num(9, 2);
	tm_v.tm_min = num(11, 2);
	tm_v.tm_sec = num(13, 2);
	if (tm_v.tm_mon < 0 || tm_v.tm_mon > 11 || tm_v.tm_mday < 1 || tm_v.tm_mday > 31) {
		return false;
	}
#ifdef _WIN32
	out = (int64_t)_mkgmtime(&tm_v);
#else
	out = (int64_t)timegm(&tm_v);
#endif
	return true;
}

// The component name a filter selects, e.g. VCALENDAR > VEVENT.
std::string FilterComponent(const davx::Node &filter) {
	for (const auto *outer : filter.Children(davx::kNsCalDav, "comp-filter")) {
		if (quackmail::util::Upper(outer->Attr("name")) != "VCALENDAR") {
			continue;
		}
		for (const auto *inner : outer->Children(davx::kNsCalDav, "comp-filter")) {
			return quackmail::util::Upper(inner->Attr("name"));
		}
	}
	return std::string();
}

// The time-range a filter carries, if any.
bool FilterTimeRange(const davx::Node &filter, int64_t &from, int64_t &to) {
	for (const auto *outer : filter.Children(davx::kNsCalDav, "comp-filter")) {
		for (const auto *inner : outer->Children(davx::kNsCalDav, "comp-filter")) {
			const davx::Node *tr = inner->Child(davx::kNsCalDav, "time-range");
			if (!tr) {
				continue;
			}
			// An absent bound is open-ended, which is what the spec says and what
			// a client asking only for "everything after today" sends.
			from = 0;
			to = 0;
			bool any = false;
			int64_t v = 0;
			if (ParseIcalUtc(tr->Attr("start"), v)) {
				from = v;
				any = true;
			}
			if (ParseIcalUtc(tr->Attr("end"), v)) {
				to = v;
				any = true;
			}
			if (any) {
				if (to == 0) {
					to = from + 3650LL * 86400; // ten years is "open ended" enough
				}
				return true;
			}
		}
	}
	return false;
}

// Does this object fall inside [from, to)?
//
// Expansion goes through ical::Expand, which resolves a recurrence in the
// event's own zone — so a weekly 09:00 meeting still matches after a DST change
// rather than sliding an hour out of the window.
bool InTimeRange(const std::string &body, int64_t from, int64_t to) {
	std::vector<ical::Item> items;
	if (!ical::ParseItems(body, items)) {
		return false;
	}
	for (const auto &item : items) {
		for (const auto &occ : ical::Expand(item, from, to)) {
			if (occ.start < to && (occ.end > from || occ.end == occ.start)) {
				return true;
			}
		}
	}
	return false;
}

// Whether a calendar object is of the component kind a filter asked for.
bool MatchesComponent(const std::string &body, const std::string &comp) {
	if (comp.empty() || comp == "VCALENDAR") {
		return true;
	}
	std::vector<ical::Item> items;
	if (!ical::ParseItems(body, items) || items.empty()) {
		return false;
	}
	for (const auto &item : items) {
		const char *kind = item.kind == ical::Item::Todo    ? "VTODO"
		                   : item.kind == ical::Item::Journal ? "VJOURNAL"
		                                                      : "VEVENT";
		if (comp == kind) {
			return true;
		}
	}
	return false;
}

// A CardDAV prop-filter: a property whose value contains (or equals) some text.
// Only the two match types clients send are implemented; anything else is
// treated as "matches", because returning too much is recoverable and returning
// too little looks to the user like data loss.
bool MatchesCardFilter(const std::string &body, const davx::Node &filter) {
	auto props = filter.Children(davx::kNsCardDav, "prop-filter");
	if (props.empty()) {
		return true;
	}
	bool require_all = quackmail::util::Lower(filter.Attr("test")) != "anyof";
	vcard::Card card;
	if (!vcard::ParseOne(body, card)) {
		return false;
	}

	bool any_matched = false;
	for (const auto *pf : props) {
		std::string name = quackmail::util::Upper(pf->Attr("name"));
		const davx::Node *text = pf->Child(davx::kNsCardDav, "text-match");
		bool matched = false;
		for (const auto *prop : card.FindAll(name)) {
			if (!text) {
				matched = true; // is-present
				break;
			}
			std::string hay = quackmail::util::Lower(prop->Value());
			std::string needle = quackmail::util::Lower(text->text);
			std::string type = quackmail::util::Lower(text->Attr("match-type"));
			bool hit = type == "equals" ? hay == needle : hay.find(needle) != std::string::npos;
			if (quackmail::util::Lower(text->Attr("negate-condition")) == "yes") {
				hit = !hit;
			}
			if (hit) {
				matched = true;
				break;
			}
		}
		if (matched) {
			any_matched = true;
		} else if (require_all) {
			return false;
		}
	}
	return require_all ? true : any_matched;
}

// Assemble a response for one object.
void EmitObject(Ctx &ctx, davx::Writer &w, const DavCollection &c, const DavObject &o,
                const PropRequest &pr) {
	PropSource src;
	src.href = ObjectHref(c.kind, ctx.username, c.room.room_num, o.euid);
	src.type = DavRes::Object;
	src.kind = c.kind;
	src.user = ctx.username;
	src.coll = &c;
	src.obj = &o;
	// The whole point of a REPORT over a PROPFIND: the body comes back inline,
	// so a client syncing fifty events makes one request rather than fifty-one.
	src.want_data = true;
	WriteResponse(ctx, w, src, pr);
}

// The hrefs a multiget listed, reduced to the euids they name. An href that is
// not inside this collection is skipped rather than resolved — a client must not
// be able to read another room by naming it in a multiget aimed here.
std::vector<std::string> MultigetEuids(Ctx &ctx, const DavCollection &c, const davx::Node &root) {
	std::vector<std::string> out;
	std::string prefix = CollectionHref(c.kind, ctx.username, c.room.room_num);
	for (const auto *href : root.Children(davx::kNsDav, "href")) {
		std::string path = href->text;
		// Clients sometimes send an absolute URL. Take everything from the
		// third slash, which is where the path starts in "scheme://host/path".
		size_t scheme = path.find("://");
		if (scheme != std::string::npos) {
			size_t slash = path.find('/', scheme + 3);
			path = slash == std::string::npos ? std::string() : path.substr(slash);
		}
		std::string decoded;
		if (!http::NormalizePath(http::PercentDecode(path, false), decoded)) {
			continue;
		}
		if (decoded.rfind(prefix, 0) != 0) {
			continue;
		}
		// Decoded through ParseDavPath rather than by a second copy of the
		// name-and-extension rules, so the two can never drift apart.
		DavPath parsed = ParseDavPath(decoded.substr(std::string("/dav/").size()));
		if (parsed.type == DavRes::Object && parsed.room_num == c.room.room_num && !parsed.euid.empty()) {
			out.push_back(parsed.euid);
		}
	}
	return out;
}

} // namespace

void DavReport(Ctx &ctx, const DavPath &p) {
	bool empty = false;
	davx::Node root;
	if (!DavBody(ctx, root, empty)) {
		if (empty) {
			DavError(ctx, 400, davx::kNsDav, "");
		}
		return;
	}

	// Every report we implement is against a collection. A report aimed at a
	// principal or the root is legal in the abstract but names nothing here.
	if (p.type != DavRes::Collection) {
		DavStatus(ctx, 403);
		return;
	}
	DavCollection c;
	if (!ResolveCollection(ctx, p, c)) {
		DavStatus(ctx, 404);
		return;
	}

	const bool is_cal = c.kind == DavKind::Calendar;
	PropRequest pr = ParsePropRequest(root, true);

	// ---- sync-collection ------------------------------------------------
	if (root.Is(davx::kNsDav, "sync-collection")) {
		int64_t now = quackmail::citadel::RoomChangeToken(ctx.con, c.room.room_num);
		const davx::Node *token_node = root.Child(davx::kNsDav, "sync-token");
		std::string token = token_node ? token_node->text : std::string();
		int64_t since = token.empty() ? 0 : SyncTokenValue(ctx, token);

		davx::Writer w;
		w.StartDoc(davx::kNsDav, "multistatus");

		if (since < 0) {
			// A token we did not mint, or one that has aged past the tombstone
			// retention. RFC 6578 says to answer an invalid token with the whole
			// collection rather than an empty diff, so the client resynchronizes
			// instead of quietly missing everything that happened.
			for (const auto &o : ListObjects(ctx, c)) {
				EmitObject(ctx, w, c, o, pr);
			}
		} else {
			// A replaced object shows up twice — new msgnum, plus the tombstone
			// for the one it displaced — so resolve each euid once and let the
			// store say whether it still exists.
			std::vector<std::string> seen;
			for (const auto &ch : quackmail::citadel::RoomChangesSince(ctx.con, c.room.room_num, since)) {
				if (ch.euid.empty()) {
					continue; // an ordinary message, not one of our resources
				}
				bool already = false;
				for (const auto &s : seen) {
					if (s == ch.euid) {
						already = true;
						break;
					}
				}
				if (already) {
					continue;
				}
				seen.push_back(ch.euid);

				DavObject o;
				if (LoadObject(ctx, c, ch.euid, o)) {
					EmitObject(ctx, w, c, o, pr);
				} else {
					WriteGoneResponse(w, ObjectHref(c.kind, ctx.username, c.room.room_num, ch.euid));
				}
			}
		}

		// The token goes last and names the state the client has now caught up
		// to — the one read *before* the diff, so a change landing mid-request is
		// reported next time rather than skipped.
		w.TextElem(davx::kNsDav, "sync-token", "urn:quackcit:sync:" + std::to_string(now));
		w.Close();
		SendMultiStatus(ctx, w.Str());
		return;
	}

	// ---- multiget --------------------------------------------------------
	const bool cal_multiget = root.Is(davx::kNsCalDav, "calendar-multiget");
	const bool card_multiget = root.Is(davx::kNsCardDav, "addressbook-multiget");
	if (cal_multiget || card_multiget) {
		if (cal_multiget != is_cal) {
			DavStatus(ctx, 403); // a calendar report aimed at an address book
			return;
		}
		davx::Writer w;
		w.StartDoc(davx::kNsDav, "multistatus");
		for (const auto &euid : MultigetEuids(ctx, c, root)) {
			DavObject o;
			if (LoadObject(ctx, c, euid, o)) {
				EmitObject(ctx, w, c, o, pr);
			} else {
				// Named but gone. Reporting it as 404 inside the multistatus is
				// how a client learns to drop it, rather than retrying forever.
				WriteGoneResponse(w, ObjectHref(c.kind, ctx.username, c.room.room_num, euid));
			}
		}
		w.Close();
		SendMultiStatus(ctx, w.Str());
		return;
	}

	// ---- query -----------------------------------------------------------
	const bool cal_query = root.Is(davx::kNsCalDav, "calendar-query");
	const bool card_query = root.Is(davx::kNsCardDav, "addressbook-query");
	if (cal_query || card_query) {
		if (cal_query != is_cal) {
			DavStatus(ctx, 403);
			return;
		}
		const davx::Node *filter =
		    root.Child(cal_query ? davx::kNsCalDav : davx::kNsCardDav, "filter");

		std::string comp;
		int64_t from = 0;
		int64_t to = 0;
		bool ranged = false;
		if (cal_query && filter) {
			comp = FilterComponent(*filter);
			ranged = FilterTimeRange(*filter, from, to);
		}

		davx::Writer w;
		w.StartDoc(davx::kNsDav, "multistatus");
		for (const auto &o : ListObjects(ctx, c)) {
			if (cal_query) {
				if (!MatchesComponent(o.body, comp)) {
					continue;
				}
				if (ranged && !InTimeRange(o.body, from, to)) {
					continue;
				}
			} else if (filter && !MatchesCardFilter(o.body, *filter)) {
				continue;
			}
			EmitObject(ctx, w, c, o, pr);
		}
		w.Close();
		SendMultiStatus(ctx, w.Str());
		return;
	}

	// A report we do not implement. The named precondition is what tells the
	// client to stop asking for it.
	DavError(ctx, 403, davx::kNsDav, "supported-report");
}

} // namespace qmweb
} // namespace duckdb
