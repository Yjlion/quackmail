#include "quackmail/caldav_filter.hpp"

#include "quackmail/util.hpp"

#include <cstdlib>
#include <ctime>

namespace quackmail {
namespace caldav {

namespace {

namespace davx = quackmail::dav;

// An iCalendar UTC timestamp, the form a time-range attribute carries:
// "20260315T090000Z". False for anything else, which a caller treats as "no
// bound" rather than as an error — a half-specified range is still a range.
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

// "anyof" means one child is enough; anything else — including the attribute
// being absent, which is the common case — means all of them must match.
bool AnyOf(const davx::Node &n) {
	return util::Lower(n.Attr("test")) == "anyof";
}

// Combine child results under the node's own test attribute. `any` is whether
// at least one child matched, `all` whether every one did, and `count` how many
// there were — with no children at all, a level imposes no condition and the
// answer is "matches", which is what an empty <comp-filter name="VEVENT"/>
// means.
bool Combine(const davx::Node &n, size_t count, bool any, bool all) {
	if (count == 0) {
		return true;
	}
	return AnyOf(n) ? any : all;
}

// ---- time-range ----------------------------------------------------------

// Read start/end off a <time-range>. An absent bound is open-ended; ten years
// is "open ended" enough for the far end, matching what the DAV report has
// always done rather than introducing a second convention.
void RangeOf(const davx::Node &tr, int64_t &from, int64_t &to) {
	from = 0;
	to = 0;
	int64_t v = 0;
	if (ParseIcalUtc(tr.Attr("start"), v)) {
		from = v;
	}
	if (ParseIcalUtc(tr.Attr("end"), v)) {
		to = v;
	}
	if (to == 0) {
		to = from + 3650LL * 86400;
	}
}

// Does this component have an occurrence inside [from, to)?
//
// Expansion goes through ical::Expand, which resolves a recurrence in the
// event's own zone — so a weekly 09:00 meeting still matches after a DST change
// rather than sliding an hour out of the window. It is the same expander the
// web calendar grid and free/busy use; there is deliberately not a second one.
bool ComponentInRange(const ical::Component &c, const ical::Component &root, int64_t from,
                      int64_t to) {
	ical::Item item;
	if (!ical::ItemFromComponent(c, root, item)) {
		// A VALARM or VFREEBUSY: RFC 4791 defines a time-range for those too,
		// but expanding them means a second model. Not matching is the safe
		// answer — over-returning would be worse than under-returning only if
		// the client could not tell, and here it can.
		return false;
	}
	for (const auto &occ : ical::Expand(item, from, to)) {
		// A zero-length instant at the boundary still counts as inside; an event
		// that ends exactly when the window opens does not.
		if (occ.start < to && (occ.end > from || occ.end == occ.start)) {
			return true;
		}
	}
	return false;
}

// ---- param-filter --------------------------------------------------------

bool MatchParamFilter(const davx::Node &pf, const char *ns, const ical::Property *prop,
                      const vcard::Property *vprop) {
	std::string name = util::Upper(pf.Attr("name"));
	bool present = false;
	std::string value;
	if (prop) {
		for (const auto &kv : prop->params) {
			if (util::Upper(kv.first) == name) {
				present = true;
				value = kv.second;
				break;
			}
		}
	} else if (vprop) {
		for (const auto &kv : vprop->params) {
			if (util::Upper(kv.first) == name) {
				present = true;
				value = kv.second;
				break;
			}
		}
	}

	if (pf.Child(ns, "is-not-defined")) {
		return !present;
	}
	if (!present) {
		return false;
	}
	if (const davx::Node *tm = pf.Child(ns, "text-match")) {
		return TextMatches(*tm, value);
	}
	return true; // is-present
}

// ---- prop-filter ---------------------------------------------------------

// One prop-filter against every property of that name on the component. RFC
// 4791 §9.7.2: the filter matches if *any* instance of the property matches,
// which matters for repeated properties like ATTENDEE and CATEGORIES.
bool MatchPropFilter(const davx::Node &pf, const char *ns, const ical::Component &c) {
	std::string name = util::Upper(pf.Attr("name"));

	std::vector<const ical::Property *> hits;
	for (const auto &p : c.props) {
		if (p.name == name) {
			hits.push_back(&p);
		}
	}

	if (pf.Child(ns, "is-not-defined")) {
		return hits.empty();
	}
	if (hits.empty()) {
		return false;
	}

	auto params = pf.Children(ns, "param-filter");
	const davx::Node *tm = pf.Child(ns, "text-match");
	if (params.empty() && !tm) {
		return true; // is-present
	}

	for (const ical::Property *p : hits) {
		bool ok = true;
		if (tm && !TextMatches(*tm, p->value)) {
			ok = false;
		}
		if (ok) {
			size_t n = 0;
			bool any = false, all = true;
			for (const auto *sub : params) {
				n++;
				if (MatchParamFilter(*sub, ns, p, nullptr)) {
					any = true;
				} else {
					all = false;
				}
			}
			if (!Combine(pf, n, any, all)) {
				ok = false;
			}
		}
		if (ok) {
			return true;
		}
	}
	return false;
}

bool MatchVCardPropFilter(const davx::Node &pf, const char *ns, const vcard::Card &card) {
	std::string name = util::Upper(pf.Attr("name"));
	auto hits = card.FindAll(name);

	if (pf.Child(ns, "is-not-defined")) {
		return hits.empty();
	}
	if (hits.empty()) {
		return false;
	}

	auto params = pf.Children(ns, "param-filter");
	const davx::Node *tm = pf.Child(ns, "text-match");
	if (params.empty() && !tm) {
		return true;
	}

	for (const auto *p : hits) {
		bool ok = true;
		if (tm && !TextMatches(*tm, p->Value())) {
			ok = false;
		}
		if (ok) {
			size_t n = 0;
			bool any = false, all = true;
			for (const auto *sub : params) {
				n++;
				if (MatchParamFilter(*sub, ns, nullptr, p)) {
					any = true;
				} else {
					all = false;
				}
			}
			if (!Combine(pf, n, any, all)) {
				ok = false;
			}
		}
		if (ok) {
			return true;
		}
	}
	return false;
}

// ---- comp-filter ---------------------------------------------------------

bool MatchCompFilter(const davx::Node &cf, const ical::Component &c, const ical::Component &root);

// Every child component of `c` named by `cf`, tested. A comp-filter on a name
// that appears more than once matches if any instance does.
bool MatchChildComp(const davx::Node &cf, const ical::Component &c, const ical::Component &root) {
	std::string name = util::Upper(cf.Attr("name"));
	std::vector<const ical::Component *> hits;
	for (const auto &child : c.children) {
		if (child.name == name) {
			hits.push_back(&child);
		}
	}

	if (cf.Child(dav::kNsCalDav, "is-not-defined")) {
		return hits.empty();
	}
	for (const ical::Component *child : hits) {
		if (MatchCompFilter(cf, *child, root)) {
			return true;
		}
	}
	return false;
}

// One comp-filter, applied to the component it selected. Its own name has
// already been matched by the caller; what is evaluated here are the conditions
// *inside* it.
bool MatchCompFilter(const davx::Node &cf, const ical::Component &c, const ical::Component &root) {
	size_t n = 0;
	bool any = false, all = true;
	auto note = [&](bool hit) {
		n++;
		if (hit) {
			any = true;
		} else {
			all = false;
		}
	};

	if (const davx::Node *tr = cf.Child(dav::kNsCalDav, "time-range")) {
		int64_t from = 0, to = 0;
		RangeOf(*tr, from, to);
		note(ComponentInRange(c, root, from, to));
	}
	for (const auto *sub : cf.Children(dav::kNsCalDav, "comp-filter")) {
		note(MatchChildComp(*sub, c, root));
	}
	for (const auto *pf : cf.Children(dav::kNsCalDav, "prop-filter")) {
		note(MatchPropFilter(*pf, dav::kNsCalDav, c));
	}
	return Combine(cf, n, any, all);
}

} // namespace

// ---- text-match ----------------------------------------------------------

bool TextMatches(const davx::Node &tm, const std::string &value) {
	// Collation is not honoured beyond case folding. RFC 6352's default is
	// i;unicode-casemap; this does an ASCII fold, which is what the rest of the
	// tree does everywhere it compares user text. Claiming otherwise in the
	// supported-collation-set would be the actual bug.
	std::string hay = util::Lower(value);
	std::string needle = util::Lower(tm.text);
	std::string type = util::Lower(tm.Attr("match-type"));

	bool hit;
	if (type == "equals") {
		hit = hay == needle;
	} else if (type == "starts-with") {
		hit = hay.rfind(needle, 0) == 0;
	} else if (type == "ends-with") {
		hit = needle.size() <= hay.size() &&
		      hay.compare(hay.size() - needle.size(), needle.size(), needle) == 0;
	} else {
		// "contains", and anything unrecognised. Returning too much is
		// recoverable; returning too little looks to the user like data loss.
		hit = hay.find(needle) != std::string::npos;
	}

	// CalDAV spells it `negate-condition`, CardDAV `negate-condition` too, and
	// both use "yes".
	if (util::Lower(tm.Attr("negate-condition")) == "yes") {
		hit = !hit;
	}
	return hit;
}

bool MatchCalendar(const davx::Node &filter, const std::string &ical_body) {
	auto tops = filter.Children(dav::kNsCalDav, "comp-filter");
	if (tops.empty()) {
		return true; // no conditions: the whole collection
	}
	ical::Component root;
	if (!ical::Parse(ical_body, root)) {
		return false;
	}
	// The outermost comp-filter names VCALENDAR, which is the tree's own root
	// rather than one of its children — so it is matched here and its contents
	// are evaluated against `root` directly.
	size_t n = 0;
	bool any = false, all = true;
	for (const auto *cf : tops) {
		std::string name = util::Upper(cf->Attr("name"));
		bool hit;
		if (cf->Child(dav::kNsCalDav, "is-not-defined")) {
			hit = name != root.name;
		} else if (name != root.name) {
			hit = false;
		} else {
			hit = MatchCompFilter(*cf, root, root);
		}
		n++;
		if (hit) {
			any = true;
		} else {
			all = false;
		}
	}
	return Combine(filter, n, any, all);
}

bool MatchAddressBook(const davx::Node &filter, const std::string &vcard_body) {
	auto props = filter.Children(dav::kNsCardDav, "prop-filter");
	if (props.empty()) {
		return true;
	}
	vcard::Card card;
	if (!vcard::ParseOne(vcard_body, card)) {
		return false;
	}
	size_t n = 0;
	bool any = false, all = true;
	for (const auto *pf : props) {
		n++;
		if (MatchVCardPropFilter(*pf, dav::kNsCardDav, card)) {
			any = true;
		} else {
			all = false;
		}
	}
	return Combine(filter, n, any, all);
}

} // namespace caldav
} // namespace quackmail
