#include "quackmail/tz.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace quackmail {
namespace tz {

namespace {

// Days in the civil calendar. Everything below works in days-since-epoch and
// seconds-in-day rather than through the C library, because localtime_r would
// consult the *host's* zone and gmtime_r is not the operation we need either.

bool LeapYear(int y) {
	return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int DaysInMonth(int y, int m) {
	static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (m == 2 && LeapYear(y)) {
		return 29;
	}
	return kDays[m - 1];
}

// Howard Hinnant's days_from_civil, which is exact for the whole int64 range and
// has no branch on the epoch. y/m/d are proleptic Gregorian.
int64_t DaysFromCivil(int64_t y, int m, int d) {
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;                                             // [0, 399]
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;            // [0, 365]
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                     // [0, 146096]
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

// Day of week for a days-since-epoch value: 0 = Sunday. 1970-01-01 was a
// Thursday, hence the +4.
int WeekdayFromDays(int64_t days) {
	int wd = (int)((days + 4) % 7);
	return wd < 0 ? wd + 7 : wd;
}

const Zone *FindZone(const std::string &name) {
	// The table is emitted sorted by name, so this is a binary search.
	size_t lo = 0, hi = kZoneCount;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int cmp = name.compare(kZones[mid].name);
		if (cmp == 0) {
			return &kZones[mid];
		}
		if (cmp < 0) {
			hi = mid;
		} else {
			lo = mid + 1;
		}
	}
	return nullptr;
}

std::string AbbrevOf(const Zone &z, const ZoneType &t) {
	// The blob is NUL-separated and indexed directly, so the abbreviation is
	// whatever runs from abbr_idx to the next NUL.
	return std::string(z.abbrs + t.abbr_idx);
}

// ---- POSIX TZ rules ------------------------------------------------------
//
// Everything after a zone's last stored transition comes from its footer:
// "EST5EDT,M3.2.0,M11.1.0". Without this a date in 2050 would be frozen at
// whatever the final recorded transition left in force, which for a
// northern-hemisphere zone means being an hour wrong for half of every year.

struct PosixRule {
	bool valid = false;
	bool has_dst = false;
	std::string std_abbr, dst_abbr;
	int std_offset = 0; // seconds east of UTC
	int dst_offset = 0;
	// Start and end, in the M<month>.<week>.<day>/<time> form. Julian forms
	// (Jn / n) are rare enough — and confined to zones with no DST — that they
	// are treated as "no DST" rather than parsed.
	int start_mon = 0, start_week = 0, start_dow = 0, start_time = 7200;
	int end_mon = 0, end_week = 0, end_dow = 0, end_time = 7200;
};

// "EST", "<+0530>", "GMT+0" — the name, which is either letters or a quoted
// run in angle brackets.
bool ParseAbbr(const char *&p, std::string &out) {
	if (*p == '<') {
		p++;
		while (*p && *p != '>') {
			out += *p++;
		}
		if (*p != '>') {
			return false;
		}
		p++;
		return !out.empty();
	}
	while (*p && (std::isalpha((unsigned char)*p) != 0)) {
		out += *p++;
	}
	return out.size() >= 3;
}

// "5", "-5:30", "+10:30:00" — hours[:minutes[:seconds]]. POSIX offsets are
// *west* positive, the opposite of everything else here, so the caller negates.
bool ParseOffset(const char *&p, int &secs) {
	int sign = 1;
	if (*p == '+') {
		p++;
	} else if (*p == '-') {
		sign = -1;
		p++;
	}
	if (std::isdigit((unsigned char)*p) == 0) {
		return false;
	}
	int parts[3] = {0, 0, 0};
	for (int i = 0; i < 3; i++) {
		int v = 0;
		while (std::isdigit((unsigned char)*p) != 0) {
			v = v * 10 + (*p++ - '0');
		}
		parts[i] = v;
		if (*p != ':') {
			break;
		}
		p++;
	}
	secs = sign * (parts[0] * 3600 + parts[1] * 60 + parts[2]);
	return true;
}

bool ParseChange(const char *&p, int &mon, int &week, int &dow, int &time_of_day) {
	if (*p != 'M') {
		return false; // Jn / n forms: see the note on PosixRule
	}
	p++;
	mon = (int)std::strtol(p, const_cast<char **>(&p), 10);
	if (*p != '.') {
		return false;
	}
	p++;
	week = (int)std::strtol(p, const_cast<char **>(&p), 10);
	if (*p != '.') {
		return false;
	}
	p++;
	dow = (int)std::strtol(p, const_cast<char **>(&p), 10);
	time_of_day = 7200; // POSIX default: 02:00 local
	if (*p == '/') {
		p++;
		int t = 0;
		if (!ParseOffset(p, t)) {
			return false;
		}
		time_of_day = t;
	}
	return mon >= 1 && mon <= 12 && week >= 1 && week <= 5 && dow >= 0 && dow <= 6;
}

PosixRule ParsePosix(const char *spec) {
	PosixRule r;
	if (!spec || !*spec) {
		return r;
	}
	const char *p = spec;
	if (!ParseAbbr(p, r.std_abbr)) {
		return r;
	}
	int west = 0;
	if (!ParseOffset(p, west)) {
		return r;
	}
	r.std_offset = -west; // POSIX counts west-positive
	r.valid = true;
	if (!*p) {
		return r; // no DST, e.g. "MST7"
	}
	if (!ParseAbbr(p, r.dst_abbr)) {
		return r;
	}
	// The DST offset is optional and defaults to one hour ahead.
	if (*p == '+' || *p == '-' || std::isdigit((unsigned char)*p) != 0) {
		int dwest = 0;
		if (!ParseOffset(p, dwest)) {
			return r;
		}
		r.dst_offset = -dwest;
	} else {
		r.dst_offset = r.std_offset + 3600;
	}
	if (*p != ',') {
		return r;
	}
	p++;
	if (!ParseChange(p, r.start_mon, r.start_week, r.start_dow, r.start_time)) {
		return r;
	}
	if (*p != ',') {
		return r;
	}
	p++;
	if (!ParseChange(p, r.end_mon, r.end_week, r.end_dow, r.end_time)) {
		return r;
	}
	r.has_dst = true;
	return r;
}

// The UTC instant a M<mon>.<week>.<dow>/<time> rule fires in `year`.
int64_t RuleInstant(int64_t year, int mon, int week, int dow, int time_of_day, int utc_offset) {
	int64_t first = DaysFromCivil(year, mon, 1);
	int first_dow = WeekdayFromDays(first);
	int delta = (dow - first_dow + 7) % 7;
	int64_t day = first + delta + (int64_t)(week - 1) * 7;
	// Week 5 means "the last such weekday in the month", which is week 4 or 5
	// depending on the month — step back if we ran over.
	int64_t last = DaysFromCivil(year, mon, DaysInMonth((int)year, mon));
	while (day > last) {
		day -= 7;
	}
	return day * 86400 + time_of_day - utc_offset;
}

// Resolve `utc` against a zone's POSIX footer.
bool ApplyPosix(const PosixRule &r, int64_t utc, int &offset, bool &is_dst, std::string &abbrev) {
	if (!r.valid) {
		return false;
	}
	if (!r.has_dst) {
		offset = r.std_offset;
		is_dst = false;
		abbrev = r.std_abbr;
		return true;
	}
	int64_t days = utc / 86400 - (utc % 86400 < 0 ? 1 : 0);
	int64_t y;
	int m, d;
	CivilFromDays(days, y, m, d);

	// The change instants are given in local time, so each is offset by
	// whichever rule is in force *before* it: DST starts from standard time and
	// ends from daylight time.
	int64_t start = RuleInstant(y, r.start_mon, r.start_week, r.start_dow, r.start_time, r.std_offset);
	int64_t end = RuleInstant(y, r.end_mon, r.end_week, r.end_dow, r.end_time, r.dst_offset);

	bool dst;
	if (start < end) {
		// Northern hemisphere: DST is the middle of the year.
		dst = utc >= start && utc < end;
	} else {
		// Southern: DST wraps the new year.
		dst = utc >= start || utc < end;
	}
	offset = dst ? r.dst_offset : r.std_offset;
	is_dst = dst;
	abbrev = dst ? r.dst_abbr : r.std_abbr;
	return true;
}

// The type in force at `utc`, from the stored table alone. Returns nullptr when
// `utc` is past the last transition, which is the caller's cue to use the POSIX
// rule instead.
const ZoneType *TypeFromTable(const Zone &z, int64_t utc, bool &past_end) {
	past_end = false;
	if (z.transition_count == 0) {
		past_end = z.posix[0] != '\0';
		return &z.types[z.first_type];
	}
	if (utc < z.transitions[0]) {
		return &z.types[z.first_type];
	}
	// The last transition is the boundary the POSIX rule takes over from.
	if (utc >= z.transitions[z.transition_count - 1] && z.posix[0] != '\0') {
		past_end = true;
	}
	// Rightmost transition <= utc.
	size_t lo = 0, hi = z.transition_count;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (z.transitions[mid] <= utc) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return &z.types[z.type_at[lo - 1]];
}

bool Resolve(const Zone &z, int64_t utc, int &offset, bool &is_dst, std::string &abbrev) {
	bool past_end = false;
	const ZoneType *t = TypeFromTable(z, utc, past_end);
	if (past_end) {
		PosixRule r = ParsePosix(z.posix);
		if (ApplyPosix(r, utc, offset, is_dst, abbrev)) {
			return true;
		}
	}
	if (!t) {
		return false;
	}
	offset = t->offset;
	is_dst = t->is_dst != 0;
	abbrev = AbbrevOf(z, *t);
	return true;
}

} // namespace

const char *Version() {
	return kTzVersion;
}

std::string Canonical(const std::string &tzid) {
	if (!FindZone(tzid)) {
		return std::string();
	}
	for (size_t i = 0; i < kLinkCount; i++) {
		if (kLinks[i].from && tzid == kLinks[i].from) {
			return kLinks[i].to;
		}
	}
	return tzid;
}

bool IsKnown(const std::string &tzid) {
	return FindZone(tzid) != nullptr;
}

std::vector<std::string> List() {
	std::vector<std::string> out;
	out.reserve(kZoneCount);
	for (size_t i = 0; i < kZoneCount; i++) {
		if (kZones[i].selectable) {
			out.push_back(kZones[i].name);
		}
	}
	return out;
}

bool OffsetAt(const std::string &tzid, int64_t utc, int &offset_secs, bool &is_dst,
              std::string &abbrev) {
	const Zone *z = FindZone(tzid);
	if (!z) {
		return false;
	}
	return Resolve(*z, utc, offset_secs, is_dst, abbrev);
}

int64_t FromUtc(const std::string &tzid, int64_t utc) {
	int offset = 0;
	bool dst = false;
	std::string abbrev;
	if (!OffsetAt(tzid, utc, offset, dst, abbrev)) {
		return utc;
	}
	return utc + offset;
}

bool ToUtc(const std::string &tzid, int64_t local_wall, int64_t &utc, bool &ambiguous,
           bool &nonexistent) {
	const Zone *z = FindZone(tzid);
	if (!z) {
		return false;
	}
	ambiguous = false;
	nonexistent = false;

	// Going the other way is a search, not a lookup: the offset depends on the
	// instant, and the instant is what is being solved for.
	//
	// The two offsets to try are the ones a day either side, which brackets any
	// transition near this wall time. Probing *at* the wall time instead would
	// converge on one answer and never notice the other, which is how an
	// ambiguous time gets silently resolved rather than reported.
	auto offset_at = [&](int64_t u) {
		int off = 0;
		bool dst = false;
		std::string ab;
		Resolve(*z, u, off, dst, ab);
		return off;
	};

	int before = offset_at(local_wall - 86400);
	int after = offset_at(local_wall + 86400);

	int64_t c1 = local_wall - before;
	int64_t c2 = local_wall - after;
	// A candidate is real only if the offset in force at it is the offset used
	// to compute it. Anything else is a wall time the clock skipped.
	bool ok1 = offset_at(c1) == before;
	bool ok2 = offset_at(c2) == after;

	if (before == after) {
		utc = c1;
		return true;
	}
	if (ok1 && ok2 && c1 != c2) {
		// The clock went back: this wall time happened twice. RFC 5545 wants
		// the first occurrence.
		ambiguous = true;
		utc = std::min(c1, c2);
		return true;
	}
	if (ok1) {
		utc = c1;
		return true;
	}
	if (ok2) {
		utc = c2;
		return true;
	}
	// Neither is self-consistent: the clock jumped forward over this wall time,
	// so it never happened. Using the pre-transition offset lands just past the
	// gap — an 02:30 that was skipped becomes 03:30 — which is what calendar
	// clients do with an event scheduled into one.
	nonexistent = true;
	utc = local_wall - std::min(before, after);
	return true;
}

} // namespace tz
} // namespace quackmail
