#include "dav.hpp"

#include "quackmail/caldav_filter.hpp"
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

// ---- expand --------------------------------------------------------------

// An instant, as the iCalendar text an expanded instance carries. All-day
// occurrences keep the DATE form: rewriting one as a UTC datetime would move a
// birthday by whatever the reader's offset happens to be.
std::string IcalUtc(int64_t epoch, bool all_day) {
	ical::DateTime dt;
	dt.epoch = epoch;
	dt.all_day = all_day;
	dt.utc = !all_day;
	dt.valid = true;
	return ical::FormatDateTime(dt);
}

// Drop TZID (and VALUE, for a DATE that is now a datetime or the reverse) off a
// property whose value has just been rewritten as an absolute instant. Leaving
// the parameter would have it say something the value contradicts.
void StripTzid(ical::Component &c, const char *name, bool all_day) {
	for (auto &p : c.props) {
		if (p.name != name) {
			continue;
		}
		std::vector<std::pair<std::string, std::string>> keep;
		for (auto &kv : p.params) {
			std::string up = quackmail::util::Upper(kv.first);
			if (up == "TZID" || up == "VALUE") {
				continue;
			}
			keep.push_back(kv);
		}
		if (all_day) {
			keep.emplace_back("VALUE", "DATE");
		}
		p.params = keep;
	}
}


// The <C:expand start= end=/> a client put inside the <C:calendar-data> it
// asked for. Read off the request root rather than off PropRequest, which keeps
// only (namespace, name) pairs and has thrown the children away by this point.
bool ExpandRange(const davx::Node &root, int64_t &from, int64_t &to) {
	const davx::Node *prop = root.Child(davx::kNsDav, "prop");
	if (!prop) {
		return false;
	}
	const davx::Node *data = prop->Child(davx::kNsCalDav, "calendar-data");
	if (!data) {
		return false;
	}
	const davx::Node *exp = data->Child(davx::kNsCalDav, "expand");
	if (!exp) {
		return false;
	}
	// Unlike a time-range filter, both bounds are required here (RFC 4791
	// §9.6.5) — an unbounded expansion of an infinite rule has no answer.
	// A malformed one is treated as absent rather than as an error, which
	// returns the master event: more than asked for, but nothing lost.
	return ParseIcalUtc(exp->Attr("start"), from) && ParseIcalUtc(exp->Attr("end"), to) && from < to;
}

// Rewrite one calendar object as one component per occurrence in [from, to).
//
// Recurrence expansion is ical::Expand — the same engine behind the web
// calendar grid, free/busy and the time-range filter, with its own
// kMaxOccurrences cap. What this adds is turning each instant back into a real
// VEVENT: RECURRENCE-ID naming the instance, DTSTART/DTEND moved onto it, and
// the rule properties stripped so a client cannot expand the expansion.
//
// Returns "" when nothing falls in the window, which the caller treats as "this
// object is not in the result set" rather than emitting an empty VCALENDAR.
std::string ExpandObject(const std::string &body, int64_t from, int64_t to) {
	ical::Component root;
	if (!ical::Parse(body, root)) {
		return std::string();
	}

	ical::Component out;
	out.name = root.name;
	out.props = root.props;
	// VTIMEZONEs are carried over: an expanded DTSTART may still name a TZID,
	// and dropping the definition would leave the client unable to place it.
	// Everything else is rebuilt from the occurrences.
	for (const auto &child : root.children) {
		if (child.name == "VTIMEZONE") {
			out.children.push_back(child);
		}
	}

	bool any = false;
	for (const auto &child : root.children) {
		ical::Item item;
		if (!ical::ItemFromComponent(child, root, item)) {
			continue;
		}
		for (const auto &occ : ical::Expand(item, from, to)) {
			if (!(occ.start < to && (occ.end > from || occ.end == occ.start))) {
				continue;
			}
			ical::Component inst = child;
			// A recurring instance is identified by the *original* start of that
			// occurrence, which for an unmodified instance is the start itself.
			inst.Set("RECURRENCE-ID", IcalUtc(occ.start, occ.all_day));
			inst.Set("DTSTART", IcalUtc(occ.start, occ.all_day));
			if (child.Find("DTEND") || child.Find("DURATION")) {
				inst.Set("DTEND", IcalUtc(occ.end, occ.all_day));
			}
			// The instance carries no rule of its own: it *is* one expansion of
			// the rule, and leaving these would have the client expand it again.
			inst.Remove("RRULE");
			inst.Remove("EXRULE");
			inst.Remove("EXDATE");
			inst.Remove("RDATE");
			// DTSTART/DTEND are now plain UTC instants, so a TZID parameter left
			// on them would say something contradictory.
			StripTzid(inst, "DTSTART", occ.all_day);
			StripTzid(inst, "DTEND", occ.all_day);
			StripTzid(inst, "RECURRENCE-ID", occ.all_day);
			out.children.push_back(std::move(inst));
			any = true;
		}
	}
	if (!any) {
		return std::string();
	}
	return ical::Emit(out);
}

// Assemble a response for one object.
void EmitObject(Ctx &ctx, davx::Writer &w, const DavCollection &c, const DavObject &o,
                const PropRequest &pr) {
	PropSource src;
	src.href = ObjectHref(c.kind, ctx.username, c.segment, o.name);
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

// The hrefs a multiget listed, reduced to the resource names they name. An href
// that is not inside this collection is skipped rather than resolved — a client
// must not be able to read another room by naming it in a multiget aimed here.
//
// Names, not euids: a client echoes back the href we gave it, and that href is
// whatever the resource is bound to rather than the object's UID.
std::vector<std::string> MultigetNames(Ctx &ctx, const DavCollection &c, const davx::Node &root) {
	std::vector<std::string> out;
	std::string prefix = CollectionHref(c.kind, ctx.username, c.segment);
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
		if (parsed.type == DavRes::Object && parsed.room_num == c.room.room_num && !parsed.name.empty()) {
			out.push_back(parsed.name);
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
					WriteGoneResponse(w, ObjectHref(c.kind, ctx.username, c.segment,
				                                ResourceNameFor(ctx, c, ch.euid)));
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

	// ---- free-busy-query --------------------------------------------------
	//
	// RFC 4791 §7.10, and the odd one out: every other report answers with a
	// multistatus, this one answers with a single text/calendar body. That is
	// the point of it — the reply carries intervals and nothing else, so it
	// says when somebody is unavailable without saying what they are doing.
	if (root.Is(davx::kNsCalDav, "free-busy-query")) {
		if (!is_cal) {
			DavStatus(ctx, 403);
			return;
		}
		const davx::Node *tr = root.Child(davx::kNsCalDav, "time-range");
		int64_t from = 0;
		int64_t to = 0;
		bool have_from = tr && ParseIcalUtc(tr->Attr("start"), from);
		bool have_to = tr && ParseIcalUtc(tr->Attr("end"), to);
		if (!have_from && !have_to) {
			// Unbounded means "read the whole calendar and expand every
			// recurrence in it", which is a request to hang a thread rather than
			// a question. The spec allows refusing it and names the condition.
			DavError(ctx, 400, davx::kNsCalDav, "valid-filter");
			return;
		}
		if (!have_to) {
			to = from + 3650LL * 86400; // the same "open ended" the query filter uses
		}
		if (!have_from) {
			from = to - 3650LL * 86400;
		}
		if (to <= from) {
			DavError(ctx, 400, davx::kNsCalDav, "valid-filter");
			return;
		}

		ical::Busy busy;
		for (const auto &o : ListObjects(ctx, c)) {
			ical::CollectBusy(o.body, from, to, busy);
		}
		std::string body = ical::EmitFreeBusy(from, to, busy, "", "", "");
		ctx.resp.Bytes(body, "text/calendar; charset=utf-8");
		ctx.resp.status = 200;
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
		for (const auto &name : MultigetNames(ctx, c, root)) {
			DavObject o;
			if (LoadObjectByName(ctx, c, name, o)) {
				EmitObject(ctx, w, c, o, pr);
			} else {
				// Named but gone. Reporting it as 404 inside the multistatus is
				// how a client learns to drop it, rather than retrying forever —
				// and at the href it asked about, not at some other spelling.
				WriteGoneResponse(w, ObjectHref(c.kind, ctx.username, c.segment, name));
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

		// An <expand> inside the requested <calendar-data>, if the client asked
		// for one. Read once, outside the loop: it is a property of the request,
		// not of any object.
		int64_t exp_from = 0;
		int64_t exp_to = 0;
		const bool expanding = cal_query && ExpandRange(root, exp_from, exp_to);

		davx::Writer w;
		w.StartDoc(davx::kNsDav, "multistatus");
		for (const auto &o : ListObjects(ctx, c)) {
			// One evaluator for both, and one parse of the body inside it. The
			// two flat extractors this replaced looked exactly two comp-filter
			// levels deep and parsed each object twice to do it.
			if (filter) {
				const bool hit = cal_query ? quackmail::caldav::MatchCalendar(*filter, o.body)
				                           : quackmail::caldav::MatchAddressBook(*filter, o.body);
				if (!hit) {
					continue;
				}
			}
			if (expanding) {
				// RFC 4791 §9.6.5: the client gets one component per occurrence,
				// each with its own RECURRENCE-ID, instead of a master plus a
				// rule it has to expand itself. An object with nothing in the
				// window drops out rather than being returned empty.
				std::string expanded = ExpandObject(o.body, exp_from, exp_to);
				if (expanded.empty()) {
					continue;
				}
				DavObject copy = o;
				copy.body = expanded;
				EmitObject(ctx, w, c, copy, pr);
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
