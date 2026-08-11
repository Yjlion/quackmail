#include "quackmail/ical.hpp"

#include "quackmail/contentline.hpp"
#include "quackmail/tz.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace quackmail {
namespace ical {

const size_t kMaxOccurrences = 750;

Property::Property() {
}
DateTime::DateTime() {
}
Component::Component() {
}
Item::Item() {
}
Occurrence::Occurrence() {
}
Period::Period() {
}
Busy::Busy() {
}

namespace {

std::string Upper(const std::string &s) {
	return util::Upper(s);
}

bool IEq(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); i++) {
		if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) {
			return false;
		}
	}
	return true;
}

std::string Trim(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return std::string();
	}
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

// ---- civil calendar ------------------------------------------------------
// The same exact conversions tz.cpp uses. Duplicated rather than shared because
// they are eight lines and exporting them would make two modules' internals
// each other's business.

int64_t DaysFromCivil(int64_t y, int m, int d) {
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

void CivilFromDays(int64_t z, int64_t &y, int &m, int &d) {
	z += 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t yy = yoe + era * 400;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	d = (int)(doy - (153 * mp + 2) / 5 + 1);
	m = (int)(mp + (mp < 10 ? 3 : -9));
	y = yy + (m <= 2);
}

int WeekdayFromDays(int64_t days) {
	int wd = (int)((days + 4) % 7);
	return wd < 0 ? wd + 7 : wd;
}

int DaysInMonth(int64_t y, int m) {
	static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
	if (m == 2 && leap) {
		return 29;
	}
	return kDays[m - 1];
}

// A wall-clock value: seconds since the epoch as if the local time were UTC.
// This is the currency Expand works in, because recurrence is defined on the
// clock ("every Tuesday at 09:00"), not on the instant.
int64_t MakeWall(int64_t y, int mo, int d, int h, int mi, int s) {
	return DaysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

void BreakWall(int64_t wall, int64_t &y, int &mo, int &d, int &h, int &mi, int &s) {
	int64_t days = wall / 86400;
	int64_t rem = wall % 86400;
	if (rem < 0) {
		rem += 86400;
		days -= 1;
	}
	CivilFromDays(days, y, mo, d);
	h = (int)(rem / 3600);
	mi = (int)((rem % 3600) / 60);
	s = (int)(rem % 60);
}

// The line grammar — unfolding, escapes, folding — lives in
// core/src/contentline.cpp, shared with vCard and vNote.
namespace cl = contentline;

// ---- inline VTIMEZONE ----------------------------------------------------

// The UTC offset an inline VTIMEZONE says applies. Only the offsets are read,
// not the RRULEs inside each STANDARD/DAYLIGHT block: picking between them needs
// a date, and the caller has one only for the value being parsed. Taking the
// larger-magnitude DAYLIGHT offset would be wrong half the year, so when the
// bundled database knows the zone it is preferred, and this is the fallback for
// a zone nobody has ever heard of.
bool InlineOffset(const Component *calendar, const std::string &tzid, int &offset) {
	if (!calendar) {
		return false;
	}
	for (auto *vtz : calendar->Children("VTIMEZONE")) {
		if (!IEq(vtz->Get("TZID"), tzid)) {
			continue;
		}
		// Prefer STANDARD: it is what the zone is when nothing special applies.
		for (const char *want : {"STANDARD", "DAYLIGHT"}) {
			const Component *sub = vtz->Child(want);
			if (!sub) {
				continue;
			}
			std::string to = sub->Get("TZOFFSETTO");
			if (to.size() < 5) {
				continue;
			}
			int sign = (to[0] == '-') ? -1 : 1;
			int hh = std::atoi(to.substr(1, 2).c_str());
			int mm = std::atoi(to.substr(3, 2).c_str());
			int ss = to.size() >= 7 ? std::atoi(to.substr(5, 2).c_str()) : 0;
			offset = sign * (hh * 3600 + mm * 60 + ss);
			return true;
		}
	}
	return false;
}

} // namespace

// ---- Property / Component ------------------------------------------------

std::string Property::Param(const std::string &pname) const {
	std::string want = Upper(pname);
	for (auto &kv : params) {
		if (kv.first == want) {
			return kv.second;
		}
	}
	return std::string();
}

const Property *Component::Find(const std::string &pname) const {
	std::string want = Upper(pname);
	for (auto &p : props) {
		if (p.name == want) {
			return &p;
		}
	}
	return nullptr;
}

std::string Component::Get(const std::string &pname) const {
	const Property *p = Find(pname);
	return p ? p->value : std::string();
}

void Component::Set(const std::string &pname, const std::string &value) {
	std::string want = Upper(pname);
	for (auto &p : props) {
		if (p.name == want) {
			p.value = value;
			return;
		}
	}
	Property p;
	p.name = want;
	p.value = value;
	props.push_back(p);
}

void Component::Remove(const std::string &pname) {
	std::string want = Upper(pname);
	props.erase(std::remove_if(props.begin(), props.end(),
	                           [&](const Property &p) { return p.name == want; }),
	            props.end());
}

const Component *Component::Child(const std::string &cname) const {
	std::string want = Upper(cname);
	for (auto &c : children) {
		if (c.name == want) {
			return &c;
		}
	}
	return nullptr;
}

std::vector<const Component *> Component::Children(const std::string &cname) const {
	std::string want = Upper(cname);
	std::vector<const Component *> out;
	for (auto &c : children) {
		if (c.name == want) {
			out.push_back(&c);
		}
	}
	return out;
}

// ---- date-time -----------------------------------------------------------

DateTime ParseDateTime(const std::string &value,
                       const std::vector<std::pair<std::string, std::string>> &params,
                       const Component *calendar) {
	DateTime dt;
	std::string v = Trim(value);
	std::string tzid, vtype;
	for (auto &kv : params) {
		if (kv.first == "TZID") {
			tzid = kv.second;
		} else if (kv.first == "VALUE") {
			vtype = Upper(kv.second);
		}
	}

	// "20260315" — a date, with no time at all.
	if (v.size() == 8 && v.find('T') == std::string::npos) {
		int64_t y = std::atoi(v.substr(0, 4).c_str());
		int mo = std::atoi(v.substr(4, 2).c_str());
		int d = std::atoi(v.substr(6, 2).c_str());
		if (mo < 1 || mo > 12 || d < 1 || d > 31) {
			return dt;
		}
		dt.all_day = true;
		dt.epoch = MakeWall(y, mo, d, 0, 0, 0);
		dt.tzid = tzid;
		dt.valid = true;
		return dt;
	}
	if (vtype == "DATE") {
		return dt; // VALUE=DATE with a non-date value: nothing sensible to do
	}

	// "20260315T090000" with an optional trailing Z.
	if (v.size() < 15 || v[8] != 'T') {
		return dt;
	}
	int64_t y = std::atoi(v.substr(0, 4).c_str());
	int mo = std::atoi(v.substr(4, 2).c_str());
	int d = std::atoi(v.substr(6, 2).c_str());
	int h = std::atoi(v.substr(9, 2).c_str());
	int mi = std::atoi(v.substr(11, 2).c_str());
	int s = std::atoi(v.substr(13, 2).c_str());
	if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) {
		return dt;
	}
	int64_t wall = MakeWall(y, mo, d, h, mi, s);

	bool zulu = v.size() >= 16 && (v[15] == 'Z' || v[15] == 'z');
	if (zulu) {
		dt.epoch = wall;
		dt.utc = true;
		dt.valid = true;
		return dt;
	}
	if (!tzid.empty()) {
		dt.tzid = tzid;
		// The bundled database first — it knows the transition dates, which an
		// inline VTIMEZONE's offsets alone cannot tell us. Fall back to the
		// inline definition for a zone the database has never heard of.
		int64_t utc = 0;
		bool amb = false, non = false;
		if (tz::ToUtc(tzid, wall, utc, amb, non)) {
			dt.epoch = utc;
			dt.valid = true;
			return dt;
		}
		int offset = 0;
		if (InlineOffset(calendar, tzid, offset)) {
			dt.epoch = wall - offset;
			dt.valid = true;
			return dt;
		}
	}
	// Floating: no zone, so it means the same clock time everywhere. Treated as
	// UTC, which is the only choice that does not invent a location.
	dt.epoch = wall;
	dt.valid = true;
	return dt;
}

std::string FormatDateTime(const DateTime &dt) {
	int64_t y;
	int mo, d, h, mi, s;
	// An all-day or zoned value is stored as the wall clock; a UTC one as the
	// instant. Either way the digits come from breaking down what we hold.
	int64_t basis = dt.epoch;
	if (!dt.all_day && !dt.tzid.empty()) {
		basis = tz::FromUtc(dt.tzid, dt.epoch);
	}
	BreakWall(basis, y, mo, d, h, mi, s);
	char buf[32];
	if (dt.all_day) {
		std::snprintf(buf, sizeof(buf), "%04lld%02d%02d", (long long)y, mo, d);
		return buf;
	}
	std::snprintf(buf, sizeof(buf), "%04lld%02d%02dT%02d%02d%02d%s", (long long)y, mo, d, h, mi, s,
	              (dt.tzid.empty() && dt.utc) ? "Z" : "");
	return buf;
}

// ---- parsing -------------------------------------------------------------

bool Parse(const std::string &text, Component &out) {
	out = Component();
	auto lines = cl::Unfold(text);

	// A stack of open components, so nested VALARM / VTIMEZONE sub-components
	// land in the right place.
	std::vector<Component> stack;
	bool have_root = false;

	for (auto &raw : lines) {
		std::string line = Trim(raw);
		if (line.empty()) {
			continue;
		}
		size_t colon = std::string::npos;
		bool quoted = false;
		for (size_t i = 0; i < line.size(); i++) {
			if (line[i] == '"') {
				quoted = !quoted;
			} else if (line[i] == ':' && !quoted) {
				colon = i;
				break;
			}
		}
		if (colon == std::string::npos) {
			continue;
		}
		std::string head = line.substr(0, colon);
		std::string value = line.substr(colon + 1);

		// Name and parameters.
		Property prop;
		{
			std::vector<std::string> parts;
			std::string cur;
			bool q = false;
			for (char c : head) {
				if (c == '"') {
					q = !q;
					continue;
				}
				if (c == ';' && !q) {
					parts.push_back(cur);
					cur.clear();
					continue;
				}
				cur += c;
			}
			parts.push_back(cur);
			if (parts.empty() || Trim(parts[0]).empty()) {
				continue;
			}
			prop.name = Upper(Trim(parts[0]));
			for (size_t i = 1; i < parts.size(); i++) {
				std::string kv = Trim(parts[i]);
				size_t eq = kv.find('=');
				if (eq == std::string::npos) {
					continue;
				}
				prop.params.emplace_back(Upper(Trim(kv.substr(0, eq))), Trim(kv.substr(eq + 1)));
			}
		}

		if (prop.name == "BEGIN") {
			Component c;
			c.name = Upper(Trim(value));
			stack.push_back(c);
			continue;
		}
		if (prop.name == "END") {
			if (stack.empty()) {
				continue;
			}
			Component done = stack.back();
			stack.pop_back();
			if (stack.empty()) {
				if (done.name == "VCALENDAR") {
					out = done;
					have_root = true;
				}
			} else {
				stack.back().children.push_back(done);
			}
			continue;
		}
		if (stack.empty()) {
			continue; // a property outside any component
		}
		// RRULE and the like keep their raw value: the semicolons in
		// "FREQ=WEEKLY;BYDAY=TU" are structure, not escaped text.
		prop.value = (prop.name == "RRULE" || prop.name == "EXRULE") ? value : cl::Unescape(value);
		stack.back().props.push_back(prop);
	}

	return have_root;
}

bool ParseItems(const std::string &text, std::vector<Item> &out) {
	out.clear();
	Component root;
	if (!Parse(text, root)) {
		return false;
	}

	for (auto &c : root.children) {
		Item it;
		if (c.name == "VEVENT") {
			it.kind = Item::Event;
		} else if (c.name == "VTODO") {
			it.kind = Item::Todo;
		} else if (c.name == "VJOURNAL") {
			it.kind = Item::Journal;
		} else {
			continue; // VTIMEZONE, VFREEBUSY: not items
		}

		it.uid = c.Get("UID");
		it.summary = c.Get("SUMMARY");
		it.description = c.Get("DESCRIPTION");
		it.location = c.Get("LOCATION");
		it.organizer = c.Get("ORGANIZER");
		it.status = c.Get("STATUS");
		it.transp = c.Get("TRANSP");
		it.rrule = c.Get("RRULE");
		it.sequence = std::atoll(c.Get("SEQUENCE").c_str());
		it.priority = std::atoi(c.Get("PRIORITY").c_str());
		it.percent_complete = std::atoi(c.Get("PERCENT-COMPLETE").c_str());

		for (auto &p : c.props) {
			if (p.name == "ATTENDEE") {
				it.attendees.push_back(p.value);
			} else if (p.name == "EXDATE") {
				// EXDATE may carry several instants on one line.
				for (auto &one : cl::SplitRaw(p.value, ',')) {
					DateTime d = ParseDateTime(one, p.params, &root);
					if (d.valid) {
						it.exdates.push_back(d.epoch);
					}
				}
			}
		}

		if (const Property *p = c.Find("DTSTART")) {
			it.start = ParseDateTime(p->value, p->params, &root);
		}
		if (const Property *p = c.Find("DTEND")) {
			it.end = ParseDateTime(p->value, p->params, &root);
		}
		if (const Property *p = c.Find("DUE")) {
			it.due = ParseDateTime(p->value, p->params, &root);
		}
		if (const Property *p = c.Find("COMPLETED")) {
			it.completed = ParseDateTime(p->value, p->params, &root);
		}

		// DURATION instead of DTEND, which is the form most phones emit.
		if (!it.end.valid && it.start.valid) {
			std::string dur = c.Get("DURATION");
			if (!dur.empty()) {
				// "PT1H30M", "P2D", "-PT15M".
				int64_t total = 0;
				int sign = 1;
				size_t i = 0;
				if (i < dur.size() && (dur[i] == '-' || dur[i] == '+')) {
					sign = dur[i] == '-' ? -1 : 1;
					i++;
				}
				if (i < dur.size() && dur[i] == 'P') {
					i++;
				}
				bool in_time = false;
				int64_t num = 0;
				bool have_num = false;
				for (; i < dur.size(); i++) {
					char ch = dur[i];
					if (ch == 'T') {
						in_time = true;
						continue;
					}
					if (std::isdigit((unsigned char)ch)) {
						num = num * 10 + (ch - '0');
						have_num = true;
						continue;
					}
					if (!have_num) {
						continue;
					}
					switch (ch) {
					case 'W':
						total += num * 604800;
						break;
					case 'D':
						total += num * 86400;
						break;
					case 'H':
						total += num * 3600;
						break;
					case 'M':
						total += in_time ? num * 60 : num * 86400 * 30;
						break;
					case 'S':
						total += num;
						break;
					default:
						break;
					}
					num = 0;
					have_num = false;
				}
				it.end = it.start;
				it.end.epoch = it.start.epoch + sign * total;
			}
		}

		out.push_back(it);
	}
	return true;
}

// ---- emitting ------------------------------------------------------------

namespace {

void EmitComponent(std::string &out, const Component &c) {
	cl::AppendFolded(out, "BEGIN:" + c.name);
	for (auto &p : c.props) {
		std::string line = p.name;
		for (auto &kv : p.params) {
			line += ";" + kv.first + "=";
			if (kv.second.find_first_of(":;,") != std::string::npos) {
				line += "\"" + kv.second + "\"";
			} else {
				line += kv.second;
			}
		}
		// RRULE's semicolons are structure; a text value's are content.
		line += ":" + ((p.name == "RRULE" || p.name == "EXRULE") ? p.value : cl::Escape(p.value));
		cl::AppendFolded(out, line);
	}
	for (auto &child : c.children) {
		EmitComponent(out, child);
	}
	cl::AppendFolded(out, "END:" + c.name);
}

} // namespace

std::string Emit(const Component &root) {
	std::string out;
	EmitComponent(out, root);
	return out;
}

std::string EmitVtimezone(const std::string &tzid, int from_year, int to_year) {
	std::string canonical = tz::Canonical(tzid);
	if (canonical.empty()) {
		return std::string();
	}
	// Sample the offset at the start of each month across the range and record
	// where it changes. That is enough to describe the zone for the years an
	// event actually spans, and it avoids reimplementing the RRULE forms real
	// zones use — which a client only needs in order to recompute what we are
	// about to state outright.
	struct Change {
		int64_t utc;
		int from_offset;
		int to_offset;
		bool to_dst;
		std::string abbrev;
	};
	std::vector<Change> changes;
	int prev_offset = 0;
	bool have_prev = false;
	std::string first_abbrev;
	int first_offset = 0;
	bool first_dst = false;

	for (int y = from_year; y <= to_year; y++) {
		for (int m = 1; m <= 12; m++) {
			int64_t probe = DaysFromCivil(y, m, 1) * 86400;
			int off = 0;
			bool dst = false;
			std::string ab;
			if (!tz::OffsetAt(canonical, probe, off, dst, ab)) {
				continue;
			}
			if (!have_prev) {
				prev_offset = off;
				first_offset = off;
				first_dst = dst;
				first_abbrev = ab;
				have_prev = true;
				continue;
			}
			if (off != prev_offset) {
				// Narrow to the exact second. Stopping at the hour would put
				// DTSTART at 02:19:41 instead of 02:00:00, and a client that
				// recomputes the transition from what we wrote would then
				// disagree with us by minutes.
				int64_t lo = DaysFromCivil(y, m == 1 ? 12 : m - 1, 1) * 86400;
				if (m == 1) {
					lo = DaysFromCivil(y - 1, 12, 1) * 86400;
				}
				int64_t hi = probe;
				while (hi - lo > 1) {
					int64_t mid = lo + (hi - lo) / 2;
					int o2 = 0;
					bool d2 = false;
					std::string a2;
					tz::OffsetAt(canonical, mid, o2, d2, a2);
					if (o2 == prev_offset) {
						lo = mid;
					} else {
						hi = mid;
					}
				}
				int o2 = 0;
				bool d2 = false;
				std::string a2;
				tz::OffsetAt(canonical, hi, o2, d2, a2);
				changes.push_back({hi, prev_offset, o2, d2, a2});
				prev_offset = off;
			}
		}
	}
	if (!have_prev) {
		return std::string();
	}

	auto offset_str = [](int secs) {
		char buf[16];
		int sign = secs < 0 ? -1 : 1;
		int a = secs * sign;
		std::snprintf(buf, sizeof(buf), "%c%02d%02d", sign < 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
		return std::string(buf);
	};
	auto stamp = [](int64_t wall) {
		int64_t y;
		int mo, d, h, mi, s;
		BreakWall(wall, y, mo, d, h, mi, s);
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%04lld%02d%02dT%02d%02d%02d", (long long)y, mo, d, h, mi, s);
		return std::string(buf);
	};

	std::string out;
	cl::AppendFolded(out, "BEGIN:VTIMEZONE");
	cl::AppendFolded(out, "TZID:" + tzid);
	if (changes.empty()) {
		// A zone with no transitions in range: one block stating the offset.
		cl::AppendFolded(out, first_dst ? "BEGIN:DAYLIGHT" : "BEGIN:STANDARD");
		cl::AppendFolded(out, "DTSTART:" + stamp(MakeWall(from_year, 1, 1, 0, 0, 0)));
		cl::AppendFolded(out, "TZOFFSETFROM:" + offset_str(first_offset));
		cl::AppendFolded(out, "TZOFFSETTO:" + offset_str(first_offset));
		cl::AppendFolded(out, "TZNAME:" + first_abbrev);
		cl::AppendFolded(out, first_dst ? "END:DAYLIGHT" : "END:STANDARD");
	}
	for (auto &ch : changes) {
		cl::AppendFolded(out, ch.to_dst ? "BEGIN:DAYLIGHT" : "BEGIN:STANDARD");
		// DTSTART inside a VTIMEZONE is local to the offset being left.
		cl::AppendFolded(out, "DTSTART:" + stamp(ch.utc + ch.from_offset));
		cl::AppendFolded(out, "TZOFFSETFROM:" + offset_str(ch.from_offset));
		cl::AppendFolded(out, "TZOFFSETTO:" + offset_str(ch.to_offset));
		if (!ch.abbrev.empty()) {
			cl::AppendFolded(out, "TZNAME:" + ch.abbrev);
		}
		cl::AppendFolded(out, ch.to_dst ? "END:DAYLIGHT" : "END:STANDARD");
	}
	cl::AppendFolded(out, "END:VTIMEZONE");
	return out;
}

namespace {

void SetDate(Component &c, const char *name, const DateTime &dt) {
	if (!dt.valid) {
		c.Remove(name);
		return;
	}
	Property p;
	p.name = name;
	p.value = FormatDateTime(dt);
	if (dt.all_day) {
		p.params.emplace_back("VALUE", "DATE");
	} else if (!dt.tzid.empty()) {
		p.params.emplace_back("TZID", dt.tzid);
	}
	// Replace in place so property order survives an edit.
	for (auto &existing : c.props) {
		if (existing.name == p.name) {
			existing = p;
			return;
		}
	}
	c.props.push_back(p);
}

const char *KindName(Item::Kind k) {
	switch (k) {
	case Item::Todo:
		return "VTODO";
	case Item::Journal:
		return "VJOURNAL";
	default:
		return "VEVENT";
	}
}

void FoldItemInto(Component &c, const Item &item) {
	c.Set("UID", item.uid);
	c.Set("SUMMARY", item.summary);
	if (item.description.empty()) {
		c.Remove("DESCRIPTION");
	} else {
		c.Set("DESCRIPTION", item.description);
	}
	if (item.location.empty()) {
		c.Remove("LOCATION");
	} else {
		c.Set("LOCATION", item.location);
	}
	if (item.status.empty()) {
		c.Remove("STATUS");
	} else {
		c.Set("STATUS", item.status);
	}
	if (item.rrule.empty()) {
		c.Remove("RRULE");
	} else {
		c.Set("RRULE", item.rrule);
	}
	// Zero means "not set", so the property is *removed* rather than left alone.
	// Leaving it was a real bug: reopening a completed task set
	// percent_complete = 0 but the stored PERCENT-COMPLETE:100 survived, so the
	// task read as done again the moment it was reloaded.
	if (item.priority > 0) {
		c.Set("PRIORITY", std::to_string(item.priority));
	} else {
		c.Remove("PRIORITY");
	}
	if (item.percent_complete > 0) {
		c.Set("PERCENT-COMPLETE", std::to_string(item.percent_complete));
	} else {
		c.Remove("PERCENT-COMPLETE");
	}
	// A changed object has to say so, or a client that already has a copy will
	// keep showing the old one.
	c.Set("SEQUENCE", std::to_string(item.sequence));
	// DTSTAMP is mandatory (RFC 5545 §3.6.1) and means "when this
	// representation was made", so it is set on every write rather than
	// preserved.
	if (c.Get("DTSTAMP").empty()) {
		DateTime now;
		now.epoch = (int64_t)std::time(nullptr);
		now.utc = true;
		now.valid = true;
		c.Set("DTSTAMP", FormatDateTime(now));
	}

	SetDate(c, "DTSTART", item.start);
	SetDate(c, "DTEND", item.end);
	SetDate(c, "DUE", item.due);
	SetDate(c, "COMPLETED", item.completed);
	// DTEND and DURATION are mutually exclusive; having just written DTEND, a
	// DURATION left over from the original would make the object contradictory.
	if (item.end.valid) {
		c.Remove("DURATION");
	}
}

} // namespace

bool ApplyItem(Component &root, const Item &item) {
	const char *want = KindName(item.kind);
	for (auto &c : root.children) {
		if (c.name == want && c.Get("UID") == item.uid) {
			FoldItemInto(c, item);
			return true;
		}
	}
	return false;
}

std::string EmitItem(const Item &item, const std::string &prodid) {
	Component root;
	root.name = "VCALENDAR";
	root.Set("VERSION", "2.0");
	root.Set("PRODID", prodid.empty() ? "-//QuackCit//EN" : prodid);

	// A TZID with no VTIMEZONE beside it leaves the client guessing what the
	// zone means, so emit one for every zone the item names.
	std::vector<std::string> zones;
	for (const DateTime *dt : {&item.start, &item.end, &item.due, &item.completed}) {
		if (dt->valid && !dt->tzid.empty() &&
		    std::find(zones.begin(), zones.end(), dt->tzid) == zones.end()) {
			zones.push_back(dt->tzid);
		}
	}

	Component body;
	body.name = KindName(item.kind);
	FoldItemInto(body, item);
	for (auto &a : item.attendees) {
		Property p;
		p.name = "ATTENDEE";
		p.value = a;
		body.props.push_back(p);
	}
	root.children.push_back(body);

	std::string out;
	cl::AppendFolded(out, "BEGIN:VCALENDAR");
	for (auto &p : root.props) {
		cl::AppendFolded(out, p.name + ":" + cl::Escape(p.value));
	}
	for (auto &z : zones) {
		int64_t y = 1970;
		int mo, d, h, mi, s;
		BreakWall(item.start.valid ? item.start.epoch : 0, y, mo, d, h, mi, s);
		// A year either side covers an event and any recurrence a client will
		// render without asking us again.
		out += EmitVtimezone(z, (int)y - 1, (int)y + 2);
	}
	EmitComponent(out, body);
	cl::AppendFolded(out, "END:VCALENDAR");
	return out;
}

// ---- recurrence ----------------------------------------------------------

namespace {

struct Rule {
	std::string freq;
	int64_t interval = 1;
	int64_t count = 0;   // 0 = unlimited
	int64_t until = 0;   // 0 = none; a UTC instant
	bool until_set = false;
	std::vector<int> bydays; // 0 = Sunday
	bool understood = false;
};

Rule ParseRule(const std::string &rrule) {
	Rule r;
	if (rrule.empty()) {
		return r;
	}
	bool unknown_part = false;
	for (auto &part : cl::SplitRaw(rrule, ';')) {
		size_t eq = part.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		std::string k = Upper(Trim(part.substr(0, eq)));
		std::string v = Trim(part.substr(eq + 1));
		if (k == "FREQ") {
			r.freq = Upper(v);
		} else if (k == "INTERVAL") {
			r.interval = std::max<int64_t>(1, std::atoll(v.c_str()));
		} else if (k == "COUNT") {
			r.count = std::atoll(v.c_str());
		} else if (k == "UNTIL") {
			DateTime dt = ParseDateTime(v, {}, nullptr);
			if (dt.valid) {
				r.until = dt.epoch;
				r.until_set = true;
			}
		} else if (k == "BYDAY") {
			static const char *kNames[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
			for (auto &one : cl::SplitRaw(v, ',')) {
				std::string d = Upper(Trim(one));
				// An ordinal prefix ("2TU", "-1FR") is a form this does not
				// expand; note it and let the caller degrade.
				if (d.size() > 2) {
					unknown_part = true;
					continue;
				}
				for (int i = 0; i < 7; i++) {
					if (d == kNames[i]) {
						r.bydays.push_back(i);
					}
				}
			}
		} else if (k == "WKST") {
			// Only affects BYWEEKNO, which is not expanded here.
		} else {
			// BYMONTHDAY, BYSETPOS, BYWEEKNO, BYMONTH, BYYEARDAY...
			unknown_part = true;
		}
	}
	r.understood = !unknown_part && (r.freq == "DAILY" || r.freq == "WEEKLY" ||
	                                 r.freq == "MONTHLY" || r.freq == "YEARLY");
	return r;
}

} // namespace

std::vector<Occurrence> Expand(const Item &item, int64_t from, int64_t to) {
	std::vector<Occurrence> out;
	if (!item.start.valid) {
		return out;
	}

	int64_t duration = 0;
	if (item.end.valid) {
		duration = item.end.epoch - item.start.epoch;
	} else if (item.start.all_day) {
		// An all-day event with no DTEND covers exactly its day.
		duration = 86400;
	}

	auto emit = [&](int64_t start_instant) {
		for (int64_t ex : item.exdates) {
			if (ex == start_instant) {
				return;
			}
		}
		// Does the occurrence intersect [from, to)?
		//
		// An event with a duration spans [start, start+duration) and overlaps
		// when it has not already ended. One *without* a duration is a single
		// instant, and has to be compared as one: "start + 0 <= from" would
		// reject an event starting exactly at the window's first second, which
		// is precisely the occurrence a day view is asking for.
		if (start_instant >= to) {
			return;
		}
		if (duration > 0 ? start_instant + duration <= from : start_instant < from) {
			return;
		}
		Occurrence o;
		o.start = start_instant;
		o.end = start_instant + duration;
		o.all_day = item.start.all_day;
		o.uid = item.uid;
		out.push_back(o);
	};

	Rule r = ParseRule(item.rrule);
	if (item.rrule.empty() || !r.understood) {
		// No rule, or one this cannot expand: the master instance still belongs
		// on the calendar. An event that cannot be expanded should appear once,
		// not disappear.
		emit(item.start.epoch);
		return out;
	}

	// Recurrence is defined on the clock, not the instant: "every Tuesday at
	// 09:00" means 09:00 local even across a DST change. So step in wall-clock
	// terms in the event's own zone and convert each occurrence back.
	const std::string &zone = item.start.tzid;
	auto to_instant = [&](int64_t wall) {
		if (zone.empty() || item.start.all_day) {
			return wall;
		}
		int64_t utc = 0;
		bool amb = false, non = false;
		if (tz::ToUtc(zone, wall, utc, amb, non)) {
			return utc;
		}
		return wall;
	};
	int64_t start_wall = (zone.empty() || item.start.all_day)
	                         ? item.start.epoch
	                         : tz::FromUtc(zone, item.start.epoch);

	int64_t y0;
	int mo0, d0, h0, mi0, s0;
	BreakWall(start_wall, y0, mo0, d0, h0, mi0, s0);
	int64_t emitted = 0;
	size_t guard = 0;

	auto want_more = [&]() {
		if (out.size() >= kMaxOccurrences || guard >= kMaxOccurrences * 8) {
			return false;
		}
		if (r.count > 0 && emitted >= r.count) {
			return false;
		}
		return true;
	};

	if (r.freq == "DAILY" || r.freq == "WEEKLY") {
		int64_t step = (r.freq == "DAILY" ? 1 : 7) * r.interval;
		// A weekly rule with BYDAY fires on each named day of every nth week.
		std::vector<int> days = r.bydays;
		if (r.freq == "WEEKLY" && !days.empty()) {
			int64_t week_start = DaysFromCivil(y0, mo0, d0);
			week_start -= WeekdayFromDays(week_start); // back to Sunday
			int64_t day_time = start_wall - DaysFromCivil(y0, mo0, d0) * 86400;
			for (int64_t w = 0; want_more(); w += r.interval) {
				guard++;
				bool any_future = false;
				for (int dow : days) {
					int64_t day = week_start + w * 7 + dow;
					int64_t wall = day * 86400 + day_time;
					if (wall < start_wall) {
						continue;
					}
					any_future = true;
					int64_t inst = to_instant(wall);
					if (r.until_set && inst > r.until) {
						return out;
					}
					emitted++;
					emit(inst);
					if (!want_more()) {
						break;
					}
				}
				// Past the window and nothing left to find.
				if (any_future && to_instant((week_start + w * 7) * 86400 + day_time) >= to) {
					break;
				}
			}
			return out;
		}
		for (int64_t n = 0; want_more(); n++) {
			guard++;
			int64_t wall = start_wall + n * step * 86400;
			int64_t inst = to_instant(wall);
			if (r.until_set && inst > r.until) {
				break;
			}
			if (inst >= to) {
				break;
			}
			emitted++;
			emit(inst);
		}
		return out;
	}

	if (r.freq == "MONTHLY" || r.freq == "YEARLY") {
		int64_t day_time = start_wall - DaysFromCivil(y0, mo0, d0) * 86400;
		for (int64_t n = 0; want_more(); n++) {
			guard++;
			int64_t y = y0;
			int mo = mo0;
			if (r.freq == "MONTHLY") {
				int64_t total = (int64_t)(mo0 - 1) + n * r.interval;
				y = y0 + total / 12;
				mo = (int)(total % 12) + 1;
			} else {
				y = y0 + n * r.interval;
			}
			// A 31st in a 30-day month simply does not occur, which is what
			// RFC 5545 says: the invalid date is skipped, not clamped.
			if (d0 > DaysInMonth(y, mo)) {
				if (DaysFromCivil(y, mo, 1) * 86400 >= to) {
					break;
				}
				continue;
			}
			int64_t wall = DaysFromCivil(y, mo, d0) * 86400 + day_time;
			int64_t inst = to_instant(wall);
			if (r.until_set && inst > r.until) {
				break;
			}
			if (inst >= to) {
				break;
			}
			emitted++;
			emit(inst);
		}
		return out;
	}

	emit(item.start.epoch);
	return out;
}

// ---- free/busy -----------------------------------------------------------

std::vector<Period> MergePeriods(std::vector<Period> in) {
	std::vector<Period> out;
	if (in.empty()) {
		return out;
	}
	std::sort(in.begin(), in.end(), [](const Period &a, const Period &b) {
		return a.start != b.start ? a.start < b.start : a.end < b.end;
	});
	for (const Period &p : in) {
		if (p.end <= p.start) {
			continue; // an instant is not an interval
		}
		// `<=` rather than `<`: two meetings that merely touch are one
		// unavailable stretch, and reporting them separately invites a client to
		// offer the zero-length gap between them.
		if (!out.empty() && p.start <= out.back().end) {
			if (p.end > out.back().end) {
				out.back().end = p.end;
			}
			continue;
		}
		out.push_back(p);
	}
	return out;
}

void CollectBusy(const std::string &text, int64_t from, int64_t to, Busy &out) {
	std::vector<Item> items;
	if (!ParseItems(text, items)) {
		return;
	}
	for (const Item &it : items) {
		if (it.kind != Item::Event) {
			continue; // RFC 4791 §7.10: only VEVENTs contribute
		}
		std::string status = Upper(it.status);
		if (status == "CANCELLED") {
			continue;
		}
		if (Upper(it.transp) == "TRANSPARENT") {
			// The property exists precisely so an entry can sit on a calendar
			// without claiming the person — an all-day "on holiday" marker is
			// the usual one, and honouring it is what stops it blocking a week.
			continue;
		}
		std::vector<Period> &into = status == "TENTATIVE" ? out.tentative : out.busy;
		for (const Occurrence &occ : Expand(it, from, to)) {
			if (occ.end <= occ.start) {
				continue;
			}
			Period p;
			// Clipped to the window: a client asked about a range and a period
			// running past it is an answer to a question it did not ask.
			p.start = occ.start < from ? from : occ.start;
			p.end = occ.end > to ? to : occ.end;
			if (p.end > p.start) {
				into.push_back(p);
			}
		}
	}
}

std::string EmitFreeBusy(int64_t from, int64_t to, const Busy &busy,
                         const std::string &organizer, const std::string &attendee,
                         const std::string &prodid) {
	auto utc = [](int64_t t) {
		DateTime dt;
		dt.epoch = t;
		dt.utc = true;
		dt.valid = true;
		return FormatDateTime(dt);
	};
	auto periods = [&](const char *fbtype, const std::vector<Period> &in) {
		std::string line;
		for (const Period &p : MergePeriods(in)) {
			line += line.empty() ? "" : ",";
			line += utc(p.start) + "/" + utc(p.end);
		}
		if (line.empty()) {
			return std::string();
		}
		return std::string("FREEBUSY;FBTYPE=") + fbtype + ":" + line;
	};

	std::string out;
	cl::AppendFolded(out, "BEGIN:VCALENDAR");
	cl::AppendFolded(out, "VERSION:2.0");
	cl::AppendFolded(out, "PRODID:" + (prodid.empty() ? std::string("-//QuackCit//EN") : prodid));
	// METHOD belongs to an iTIP reply. A CalDAV free-busy-query REPORT is not
	// one, so it is only written when there is a scheduling identity to write.
	if (!organizer.empty()) {
		cl::AppendFolded(out, "METHOD:REPLY");
	}
	cl::AppendFolded(out, "BEGIN:VFREEBUSY");
	cl::AppendFolded(out, "DTSTAMP:" + utc((int64_t)std::time(nullptr)));
	cl::AppendFolded(out, "DTSTART:" + utc(from));
	cl::AppendFolded(out, "DTEND:" + utc(to));
	if (!organizer.empty()) {
		cl::AppendFolded(out, "ORGANIZER:" + cl::Escape(organizer));
	}
	if (!attendee.empty()) {
		cl::AppendFolded(out, "ATTENDEE:" + cl::Escape(attendee));
	}
	// Absent FREEBUSY lines mean "free for the whole window", which is a real
	// answer and not an empty one — RFC 5545 §3.6.4 says so explicitly.
	std::string line = periods("BUSY", busy.busy);
	if (!line.empty()) {
		cl::AppendFolded(out, line);
	}
	line = periods("BUSY-TENTATIVE", busy.tentative);
	if (!line.empty()) {
		cl::AppendFolded(out, line);
	}
	cl::AppendFolded(out, "END:VFREEBUSY");
	cl::AppendFolded(out, "END:VCALENDAR");
	return out;
}

std::string EuidFor(const Item &item) {
	if (!item.uid.empty()) {
		return item.uid;
	}
	std::string basis = item.summary + "\x1f" + std::to_string(item.start.epoch);
	return "qc-" + util::Sha256Hex(basis).substr(0, 32);
}

std::string NewUid(const std::string &fqdn) {
	return util::RandomHex(16) + "@" + (fqdn.empty() ? "localhost" : fqdn);
}

} // namespace ical
} // namespace quackmail
