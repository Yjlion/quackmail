#include "web_views.hpp"

#include "quackmail/ical.hpp"
#include "quackmail/tz.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Message;
using quackmail::citadel::Room;
namespace ical = quackmail::ical;

namespace {

// ---- civil dates ---------------------------------------------------------
// Wall-clock arithmetic, in the viewer's zone. The calendar draws a grid of
// local days, so every boundary here is a local midnight rather than a UTC one.

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
	return (m == 2 && leap) ? 29 : kDays[m - 1];
}

const char *kMonthNames[] = {"January",   "February", "March",    "April",
                             "May",       "June",     "July",     "August",
                             "September", "October",  "November", "December"};
const char *kDayNames[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

// A local wall-clock instant -> the UTC instant it names, in `zone`.
int64_t LocalToUtc(const std::string &zone, int64_t wall) {
	int64_t utc = 0;
	bool amb = false, non = false;
	if (zone.empty() || !quackmail::tz::ToUtc(zone, wall, utc, amb, non)) {
		return wall;
	}
	return utc;
}

// The local day a value falls on, as days-since-epoch.
//
// `all_day` is not a formatting hint: an all-day DateTime already holds a
// wall-clock value (see ical.hpp), so shifting it through the zone a second time
// would move 1 April to 31 March for any viewer west of Greenwich.
int64_t LocalDay(const std::string &zone, int64_t utc, bool all_day) {
	int64_t wall = (all_day || zone.empty()) ? utc : quackmail::tz::FromUtc(zone, utc);
	int64_t days = wall / 86400;
	if (wall % 86400 < 0) {
		days--;
	}
	return days;
}

std::string LocalHm(const std::string &zone, int64_t utc, bool all_day = false) {
	int64_t wall = (all_day || zone.empty()) ? utc : quackmail::tz::FromUtc(zone, utc);
	int64_t rem = wall % 86400;
	if (rem < 0) {
		rem += 86400;
	}
	char buf[8];
	std::snprintf(buf, sizeof buf, "%02d:%02d", (int)(rem / 3600), (int)((rem % 3600) / 60));
	return buf;
}

// ---- loading -------------------------------------------------------------

// One stored calendar object: the message that holds it, its parsed items, and
// the raw body so an edit can start from what is actually on disk.
struct Stored {
	int64_t msgnum = 0;
	std::string body;
	std::vector<ical::Item> items;
};

std::vector<Stored> LoadCalendar(Ctx &ctx, const Room &room) {
	std::vector<Stored> out;
	for (int64_t num : quackmail::citadel::RoomMessages(ctx.con, room.room_num, "all", 0, 0)) {
		Message msg;
		if (!quackmail::citadel::LoadMessage(ctx.con, num, msg)) {
			continue;
		}
		std::string body = ObjectBody(msg, "text/calendar");
		if (body.empty()) {
			continue;
		}
		Stored s;
		s.msgnum = num;
		s.body = body;
		if (!ical::ParseItems(body, s.items) || s.items.empty()) {
			continue;
		}
		out.push_back(s);
	}
	return out;
}

bool LoadOne(Ctx &ctx, const Room &room, int64_t msgnum, Stored &out) {
	Message msg;
	if (!LoadMessageIn(ctx, room, msgnum, msg)) {
		return false;
	}
	out.msgnum = msgnum;
	out.body = ObjectBody(msg, "text/calendar");
	if (out.body.empty()) {
		return false;
	}
	return ical::ParseItems(out.body, out.items) && !out.items.empty();
}

// An occurrence placed on the grid, with the message it came from.
struct Placed {
	int64_t start = 0;
	int64_t end = 0;
	bool all_day = false;
	int64_t msgnum = 0;
	std::string summary;
	std::string location;
};

// Every occurrence in [from, to), sorted. Recurrence expansion happens per
// object, so an object with 200 instances costs one parse rather than 200.
std::vector<Placed> Occurrences(const std::vector<Stored> &stored, int64_t from, int64_t to,
                                bool events_only) {
	std::vector<Placed> out;
	for (auto &s : stored) {
		for (auto &item : s.items) {
			if (events_only && item.kind != ical::Item::Event) {
				continue;
			}
			for (auto &o : ical::Expand(item, from, to)) {
				Placed p;
				p.start = o.start;
				p.end = o.end;
				p.all_day = o.all_day;
				p.msgnum = s.msgnum;
				p.summary = item.summary.empty() ? "(no title)" : item.summary;
				p.location = item.location;
				out.push_back(p);
			}
		}
	}
	std::sort(out.begin(), out.end(),
	          [](const Placed &a, const Placed &b) { return a.start < b.start; });
	return out;
}

std::string ItemHref(const Room &room, int64_t msgnum, const char *suffix = "") {
	return RoomHref(room, "/item/" + std::to_string(msgnum) + suffix);
}

// ---- the month grid ------------------------------------------------------

void RenderMonth(Ctx &ctx, const Room &room, const std::string &zone, int64_t year, int month,
                 const std::vector<Stored> &stored, std::string &body) {
	// The grid runs Monday to Sunday and always shows whole weeks, so it spills
	// into the neighbouring months — which is what a month view is for.
	int64_t first = DaysFromCivil(year, month, 1);
	int lead = (WeekdayFromDays(first) + 6) % 7; // 0 = Monday
	int64_t grid_start = first - lead;
	int64_t last = DaysFromCivil(year, month, DaysInMonth(year, month));
	int trail = (7 - ((WeekdayFromDays(last) + 6) % 7 + 1)) % 7;
	int64_t grid_end = last + trail; // inclusive

	int64_t from = LocalToUtc(zone, grid_start * 86400);
	int64_t to = LocalToUtc(zone, (grid_end + 1) * 86400);
	auto occ = Occurrences(stored, from, to, true);

	int64_t today = LocalDay(zone, (int64_t)std::time(nullptr), false);

	body += "<table class=\"calmonth\"><thead><tr>";
	for (auto &d : kDayNames) {
		body += "<th>" + T(d) + "</th>";
	}
	body += "</tr></thead><tbody>";

	for (int64_t day = grid_start; day <= grid_end; day++) {
		if ((day - grid_start) % 7 == 0) {
			body += "<tr>";
		}
		int64_t y;
		int m, d;
		CivilFromDays(day, y, m, d);
		bool other_month = (m != month);

		std::string cls = "calday";
		if (other_month) {
			cls += " other";
		}
		if (day == today) {
			cls += " today";
		}
		body += "<td class=\"" + A(cls) + "\">";
		// The number is a link that creates an event on that day, which is the
		// gesture a calendar is expected to support.
		char iso[16];
		std::snprintf(iso, sizeof iso, "%04lld-%02d-%02d", (long long)y, m, d);
		body += "<div class=\"num\">" +
		        Link(RoomHref(room, "/item/new") + "?date=" + iso, std::to_string(d)) + "</div>";

		for (auto &p : occ) {
			if (LocalDay(zone, p.start, p.all_day) != day) {
				continue;
			}
			std::string label = p.all_day ? p.summary : LocalHm(zone, p.start, p.all_day) + " " + p.summary;
			body += "<div class=\"ev" + std::string(p.all_day ? " allday" : "") + "\">" +
			        Link(ItemHref(room, p.msgnum), label) + "</div>";
		}
		body += "</td>";
		if ((day - grid_start) % 7 == 6) {
			body += "</tr>";
		}
	}
	body += "</tbody></table>";
}

void RenderAgenda(Ctx &ctx, const Room &room, const std::string &zone, int64_t year, int month,
                  const std::vector<Stored> &stored, std::string &body) {
	int64_t first = DaysFromCivil(year, month, 1);
	int64_t after = DaysFromCivil(year, month, DaysInMonth(year, month)) + 1;
	auto occ = Occurrences(stored, LocalToUtc(zone, first * 86400),
	                       LocalToUtc(zone, after * 86400), true);
	if (occ.empty()) {
		body += "<p class=\"muted\">Nothing this month.</p>";
		return;
	}
	body += "<div class=\"wrap\"><table class=\"longlist\"><tr>" + Head("When") + Head("Event") +
	        Head("Where") + "</tr>";
	int64_t prev_day = -1;
	for (auto &p : occ) {
		int64_t day = LocalDay(zone, p.start, p.all_day);
		std::string when;
		if (day != prev_day) {
			int64_t y;
			int m, d;
			CivilFromDays(day, y, m, d);
			char buf[32];
			std::snprintf(buf, sizeof buf, "%s %d", kDayNames[(WeekdayFromDays(day) + 6) % 7], d);
			when = buf;
			prev_day = day;
		}
		body += "<tr>";
		body += "<td>" + T(when) + (p.all_day ? "" : " <span class=\"muted\">" +
		                                                 T(LocalHm(zone, p.start, p.all_day)) + "</span>") +
		        "</td>";
		body += "<td>" + Link(ItemHref(room, p.msgnum), p.summary) + "</td>";
		body += Cell(p.location);
		body += "</tr>";
	}
	body += "</table></div>";
}

// ---- the index -----------------------------------------------------------

void Index(Ctx &ctx, const Room &room) {
	std::string zone = EffectiveTz(ctx);
	auto stored = LoadCalendar(ctx, room);

	// Which month. Defaults to the one containing today, in the viewer's zone.
	int64_t today = LocalDay(zone, (int64_t)std::time(nullptr), false);
	int64_t y0;
	int m0, d0;
	CivilFromDays(today, y0, m0, d0);
	int64_t year = ctx.ParamInt("y", y0);
	int month = (int)ctx.ParamInt("m", m0);
	// Clamp rather than reject: a hand-edited URL should land somewhere sane.
	if (month < 1 || month > 12) {
		month = m0;
	}
	if (year < 1970 || year > 2200) {
		year = y0;
	}

	bool agenda = ctx.req.Param("v") == "list" ||
	              (ctx.req.Param("v").empty() &&
	               room.default_view == quackmail::citadel::VIEW_CALBRIEF);

	auto nav = [&](int64_t dy, int dm) {
		int64_t ny = year + dy;
		int nm = month + dm;
		while (nm > 12) {
			nm -= 12;
			ny++;
		}
		while (nm < 1) {
			nm += 12;
			ny--;
		}
		return RoomHref(room) + "?y=" + std::to_string(ny) + "&m=" + std::to_string(nm) +
		       (agenda ? "&v=list" : "&v=grid");
	};

	std::string toolbar = "<div class=\"actions\">";
	toolbar += Link(nav(0, -1), "← Previous", "btn sec");
	toolbar += "<strong>" + T(std::string(kMonthNames[month - 1]) + " " + std::to_string(year)) +
	           "</strong>";
	toolbar += Link(nav(0, 1), "Next →", "btn sec");
	toolbar += Link(RoomHref(room), "Today", "btn sec");
	toolbar += Link(RoomHref(room) + "?y=" + std::to_string(year) + "&m=" + std::to_string(month) +
	                    (agenda ? "&v=grid" : "&v=list"),
	                agenda ? "Month grid" : "Agenda", "btn sec");
	if (quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		toolbar += Link(RoomHref(room, "/item/new"), "New event", "btn");
	}
	toolbar += Link(RoomHref(room) + "?view=raw", "View as messages", "btn sec");
	toolbar += "</div>";

	std::string body;
	if (agenda) {
		RenderAgenda(ctx, room, zone, year, month, stored, body);
	} else {
		RenderMonth(ctx, room, zone, year, month, stored, body);
	}
	body += "<p class=\"muted\">Times shown in " + T(zone) + ". " +
	        RawHtml(Link("/prefs", "Change your time zone")) + ".</p>";

	PageOpts opts;
	opts.active = "calendar";
	opts.view = (int)room.default_view;
	opts.wide = true;
	opts.toolbar = toolbar;
	Render(ctx, room.display_name, body, opts);
}

// ---- one event -----------------------------------------------------------

void Row(std::string &out, const char *label, const std::string &value) {
	if (value.empty()) {
		return;
	}
	out += "<dt>" + T(label) + "</dt><dd>" + T(value) + "</dd>";
}

std::string WhenText(const std::string &zone, const ical::Item &item) {
	if (!item.start.valid) {
		return std::string();
	}
	int64_t day = LocalDay(zone, item.start.epoch, item.start.all_day);
	int64_t y;
	int m, d;
	CivilFromDays(day, y, m, d);
	char date[40];
	std::snprintf(date, sizeof date, "%s %d %s %lld", kDayNames[(WeekdayFromDays(day) + 6) % 7], d,
	              kMonthNames[m - 1], (long long)y);
	if (item.start.all_day) {
		return std::string(date) + " (all day)";
	}
	std::string out = std::string(date) + ", " + LocalHm(zone, item.start.epoch, item.start.all_day);
	if (item.end.valid && item.end.epoch > item.start.epoch) {
		out += "–" + LocalHm(zone, item.end.epoch, item.end.all_day);
	}
	return out;
}

void Item(Ctx &ctx, const Room &room, int64_t msgnum) {
	Stored s;
	if (!LoadOne(ctx, room, msgnum, s)) {
		NotFound(ctx);
		return;
	}
	std::string zone = EffectiveTz(ctx);
	const ical::Item &item = s.items[0];

	std::string body = "<div class=\"msghead\"><dl>";
	Row(body, "When", WhenText(zone, item));
	Row(body, "Where", item.location);
	Row(body, "Status", item.status);
	if (!item.rrule.empty()) {
		// Shown verbatim rather than prettified: an RRULE this code cannot fully
		// expand still has to be visible, and a wrong plain-English rendering
		// would be worse than the rule itself.
		Row(body, "Repeats", item.rrule);
	}
	std::string organizer = item.organizer;
	if (organizer.rfind("mailto:", 0) == 0) {
		organizer = organizer.substr(7);
	}
	Row(body, "Organiser", organizer);
	for (auto &a : item.attendees) {
		std::string who = a.rfind("mailto:", 0) == 0 ? a.substr(7) : a;
		body += "<dt>Attendee</dt><dd>" + T(who) + "</dd>";
	}
	body += "</dl></div>";

	if (!item.description.empty()) {
		body += "<pre class=\"body\">" + T(item.description) + "</pre>";
	}

	std::string toolbar = "<div class=\"actions\">";
	toolbar += Link(RoomHref(room), "Back to the calendar", "btn sec");
	if (quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		toolbar += Link(ItemHref(room, msgnum, "/edit"), "Edit", "btn sec");
		toolbar += FormStart(ctx, RoomHref(room, "/item/delete"), "inline") +
		           Hidden("msgnum", std::to_string(msgnum)) +
		           "<button class=\"btn danger\" type=\"submit\" data-confirm=\"Delete this event?\">"
		           "Delete</button>" +
		           FormEnd();
	}
	toolbar += Link(RoomHref(room, "/msg/" + std::to_string(msgnum) + "/source"), "iCalendar source",
	                "btn sec");
	toolbar += "</div>";

	PageOpts opts;
	opts.active = "calendar";
	opts.view = (int)room.default_view;
	opts.toolbar = toolbar;
	Render(ctx, item.summary.empty() ? "(no title)" : item.summary, body, opts);
}

// ---- the form ------------------------------------------------------------

// "2026-03-15" and "09:00" from a stored instant, in the viewer's zone — the
// shapes <input type=date> and <input type=time> want.
void SplitLocal(const std::string &zone, int64_t utc, bool all_day, std::string &date,
                std::string &time) {
	int64_t wall = (all_day || zone.empty()) ? utc : quackmail::tz::FromUtc(zone, utc);
	int64_t days = wall / 86400;
	int64_t rem = wall % 86400;
	if (rem < 0) {
		rem += 86400;
		days--;
	}
	int64_t y;
	int m, d;
	CivilFromDays(days, y, m, d);
	char db[16], tb[8];
	std::snprintf(db, sizeof db, "%04lld-%02d-%02d", (long long)y, m, d);
	std::snprintf(tb, sizeof tb, "%02d:%02d", (int)(rem / 3600), (int)((rem % 3600) / 60));
	date = db;
	time = tb;
}

// The inverse, returning the **wall-clock** value. Returns false when the date
// does not parse, so a malformed form is rejected rather than silently saving an
// event in 1970.
//
// Deliberately not the instant: an all-day event wants the wall value as-is
// (ical.hpp), and a timed one wants it converted. Returning the instant and
// asking the all-day caller to convert back would round-trip through a DST gap
// for no reason.
bool JoinLocal(const std::string &date, const std::string &time, int64_t &wall) {
	if (date.size() != 10 || date[4] != '-' || date[7] != '-') {
		return false;
	}
	int64_t y = std::atoll(date.substr(0, 4).c_str());
	int m = std::atoi(date.substr(5, 2).c_str());
	int d = std::atoi(date.substr(8, 2).c_str());
	if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1970 || y > 2200) {
		return false;
	}
	int hh = 0, mm = 0;
	if (time.size() >= 5 && time[2] == ':') {
		hh = std::atoi(time.substr(0, 2).c_str());
		mm = std::atoi(time.substr(3, 2).c_str());
	}
	if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
		return false;
	}
	wall = DaysFromCivil(y, m, d) * 86400 + hh * 3600 + mm * 60;
	return true;
}

std::string Field(const char *name, const char *label, const std::string &value,
                  const char *type = "text") {
	return "<label class=\"field\"><span>" + T(label) + "</span>" + TextInput(name, value, type) +
	       "</label>";
}

void Edit(Ctx &ctx, const Room &room, int64_t msgnum) {
	if (!quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "You cannot add anything to this room.");
		return;
	}
	std::string zone = EffectiveTz(ctx);
	bool editing = msgnum >= 0;

	ical::Item item;
	Stored s;
	if (editing) {
		if (!LoadOne(ctx, room, msgnum, s)) {
			NotFound(ctx);
			return;
		}
		item = s.items[0];
	}

	std::string date, time, end_date, end_time;
	if (item.start.valid) {
		SplitLocal(zone, item.start.epoch, item.start.all_day, date, time);
	} else {
		// A new event defaults to the day the grid was clicked on, or today.
		std::string wanted = ctx.req.Param("date");
		int64_t now = (int64_t)std::time(nullptr);
		SplitLocal(zone, now, false, date, time);
		if (wanted.size() == 10) {
			date = wanted;
		}
		time = "09:00";
	}
	if (item.end.valid) {
		SplitLocal(zone, item.end.epoch, item.end.all_day, end_date, end_time);
	} else {
		end_date = date;
		end_time = "10:00";
	}

	std::string body = FormStart(ctx, RoomHref(room, "/item/save"));
	if (editing) {
		body += Hidden("msgnum", std::to_string(msgnum));
	}
	body += Field("summary", "Title", item.summary);
	body += Field("date", "Date", date, "date");
	body += Field("time", "From", time, "time");
	body += Field("end_date", "End date", end_date, "date");
	body += Field("end_time", "To", end_time, "time");
	body += Checkbox("all_day", item.start.all_day, "All day") + "<br>";
	body += Field("location", "Where", item.location);
	body += "<label class=\"field\"><span>Repeats</span>" +
	        Select("rrule",
	                {{"", "Does not repeat"},
	                 {"FREQ=DAILY", "Every day"},
	                 {"FREQ=WEEKLY", "Every week"},
	                 {"FREQ=MONTHLY", "Every month"},
	                 {"FREQ=YEARLY", "Every year"}},
	                item.rrule) +
	        "</label>";
	if (!item.rrule.empty() &&
	    item.rrule != "FREQ=DAILY" && item.rrule != "FREQ=WEEKLY" &&
	    item.rrule != "FREQ=MONTHLY" && item.rrule != "FREQ=YEARLY") {
		// The rule is more specific than this form can express. Say so rather
		// than offering a picker that would quietly simplify it on save.
		body += "<p class=\"warnbar\">This event repeats by a rule this form cannot show "
		        "(<code>" + T(item.rrule) + "</code>). Saving here would replace it with the "
		        "simpler rule selected above.</p>";
	}
	body += "<label class=\"field\"><span>Description</span>" +
	        TextArea("description", item.description, 6) + "</label>";
	body += "<p>" + Button(editing ? "Save" : "Create event") + " " +
	        Link(editing ? ItemHref(room, msgnum) : RoomHref(room), "Cancel") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">Times are in " + T(zone) + ".</p>";

	PageOpts opts;
	opts.active = "calendar";
	opts.view = (int)room.default_view;
	Render(ctx, editing ? "Edit event" : "New event", body, opts);
}

void Save(Ctx &ctx, const Room &room) {
	std::string zone = EffectiveTz(ctx);
	int64_t msgnum = ctx.FormInt("msgnum", -1);
	bool editing = msgnum >= 0;

	// Editing starts from the stored object so alarms, attendees and X-
	// properties survive: ApplyItem writes the modelled fields back into the
	// tree and leaves the rest alone.
	ical::Component root;
	ical::Item item;
	Stored s;
	if (editing) {
		if (!LoadOne(ctx, room, msgnum, s)) {
			NotFound(ctx);
			return;
		}
		if (!ical::Parse(s.body, root)) {
			BadRequest(ctx, "The stored event could not be read.");
			return;
		}
		item = s.items[0];
	}

	std::string summary = ctx.req.Form("summary");
	if (summary.empty()) {
		BadRequest(ctx, "An event needs a title.");
		return;
	}
	bool all_day = !ctx.req.Form("all_day").empty();

	// Wall-clock values; the all_day branch below decides whether to shift them.
	int64_t start = 0, end = 0;
	if (!JoinLocal(ctx.req.Form("date"), all_day ? "00:00" : ctx.req.Form("time"), start)) {
		BadRequest(ctx, "That start date is not a date.");
		return;
	}
	std::string end_date = ctx.req.Form("end_date");
	if (end_date.empty()) {
		end_date = ctx.req.Form("date");
	}
	if (!JoinLocal(end_date, all_day ? "00:00" : ctx.req.Form("end_time"), end)) {
		BadRequest(ctx, "That end date is not a date.");
		return;
	}

	item.kind = ical::Item::Event;
	item.summary = summary;
	item.location = ctx.req.Form("location");
	item.description = ctx.req.Form("description");
	item.rrule = ctx.req.Form("rrule");
	item.start = ical::DateTime();
	item.end = ical::DateTime();
	item.start.valid = true;
	item.end.valid = true;

	if (all_day) {
		// An all-day value is the wall-clock date itself, stored unshifted. DTEND
		// is exclusive, so a one-day event ends on the following day.
		item.start.all_day = true;
		item.end.all_day = true;
		item.start.epoch = start;
		item.end.epoch = std::max(end, start) + 86400;
	} else {
		if (end <= start) {
			BadRequest(ctx, "The event ends before it starts.");
			return;
		}
		// Carry the zone, so the event means the same clock time after a DST
		// change rather than drifting an hour.
		item.start.epoch = LocalToUtc(zone, start);
		item.end.epoch = LocalToUtc(zone, end);
		item.start.tzid = zone;
		item.end.tzid = zone;
	}
	// A changed object has to say so, or a client holding a copy keeps the old.
	item.sequence++;

	std::string emitted;
	if (editing && ical::ApplyItem(root, item)) {
		emitted = ical::Emit(root);
	} else {
		if (item.uid.empty()) {
			item.uid = ical::NewUid(ConfigStr(ctx.con, "c_fqdn", "localhost"));
		}
		emitted = ical::EmitItem(item, "-//QuackCit//web//EN");
	}

	if (!SaveObject(ctx, room, ical::EuidFor(item), item.summary, "text/calendar", emitted)) {
		return;
	}
	RedirectTo(ctx, RoomHref(room), editing ? "saved" : "created");
}

void Remove(Ctx &ctx, const Room &room) {
	int64_t msgnum = ctx.FormInt("msgnum", -1);
	Stored s;
	if (!LoadOne(ctx, room, msgnum, s)) {
		NotFound(ctx);
		return;
	}
	if (!quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "You cannot change this room.");
		return;
	}
	std::string err;
	if (!quackmail::citadel::DeleteMessage(ctx.con, room.room_num, msgnum, err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, RoomHref(room), "deleted");
}

const RoomViewHandler kCalendar = {
    quackmail::citadel::VIEW_CALENDAR, "Calendar", "event", Index, Item, Edit, Save, Remove};

const RoomViewHandler kCalBrief = {
    quackmail::citadel::VIEW_CALBRIEF, "Calendar", "event", Index, Item, Edit, Save, Remove};

} // namespace

const RoomViewHandler &CalendarView() {
	return kCalendar;
}

const RoomViewHandler &CalBriefView() {
	return kCalBrief;
}

} // namespace qmweb
} // namespace duckdb
