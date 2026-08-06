#include "web.hpp"

#include "quackmail/citadel_msg.hpp"
#include "quackmail/tz.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>

// Message search.
//
// The web front-end could read rooms but never find anything in them: a room
// list offers "new / old / all" and a pager, which stops being an answer some
// hundreds of messages in. IMAP has SEARCH; this is the same question asked
// from a browser.
//
// Two things decide the shape of this file.
//
// The first is that the room set *is* the access control. Search must not widen
// what a user can already reach, so it never queries citadel_room_msgs against
// a room number out of the query string — it enumerates the rooms through the
// same helpers the pages use and searches inside that set. Everything else here
// is a detail; this is the part that must not be got wrong.
//
// The second is that `subject` and `author` are columns and a body is not.
// Matching a header is one prepared statement over the whole store. Matching a
// body means decoding each candidate message, because a format_type 4 message
// keeps its text base64-encoded inside `raw` — so that path is bounded by
// qm_web_search_scan and the page says when it hit the bound rather than
// reporting a partial answer as a complete one.

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Message;
using quackmail::citadel::Room;

namespace {

constexpr int64_t kDefaultPageSize = 30;
constexpr int64_t kDefaultScan = 2000;

// Where a term is looked for. `any` is the default because it is what somebody
// typing a name into a box means.
struct Field {
	const char *value;
	const char *label;
};

const Field kFields[] = {
    {"any", "Subject or sender"},
    {"sub", "Subject"},
    {"from", "Sender"},
    {"body", "Message text"},
};

std::string FieldOrDefault(const std::string &want) {
	for (auto &f : kFields) {
		if (want == f.value) {
			return f.value;
		}
	}
	return "any";
}

std::vector<std::pair<std::string, std::string>> FieldOptions() {
	std::vector<std::pair<std::string, std::string>> out;
	for (auto &f : kFields) {
		out.push_back({f.value, f.label});
	}
	return out;
}

// Every room this user may be shown the contents of.
//
// ListRooms applies the private-room visibility rules and drops zapped rooms —
// a forgotten room is one the user asked not to see, and search is not a way
// around that. RoomUnlocked is RequireUnlocked's rule applied in bulk: without
// it, search would report the subject lines of a passworded room to somebody
// who has not given the password.
std::vector<Room> SearchableRooms(Ctx &ctx) {
	std::vector<Room> out;
	for (auto &room : quackmail::citadel::ListRooms(ctx.con, ctx.username, -1, "all")) {
		if (!quackmail::citadel::RoomUnlocked(ctx.con, ctx.username, room)) {
			continue;
		}
		out.push_back(room);
	}
	return out;
}

// A date from an <input type=date>, as the UTC instant local midnight falls on.
// `end_of_day` pushes it to the last second of that day, so `until` is
// inclusive — a bound that excluded the day it names would be a trap.
bool ParseBound(const std::string &date, const std::string &zone, bool end_of_day, int64_t &out) {
	if (date.size() != 10 || date[4] != '-' || date[7] != '-') {
		return false;
	}
	struct tm tm {};
	tm.tm_year = std::atoi(date.substr(0, 4).c_str()) - 1900;
	tm.tm_mon = std::atoi(date.substr(5, 2).c_str()) - 1;
	tm.tm_mday = std::atoi(date.substr(8, 2).c_str());
	if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31) {
		return false;
	}
	int64_t wall = (int64_t)timegm(&tm) + (end_of_day ? 86399 : 0);
	int64_t utc = wall;
	bool ambiguous = false, nonexistent = false;
	quackmail::tz::ToUtc(zone, wall, utc, ambiguous, nonexistent);
	out = utc;
	return true;
}

struct Hit {
	int64_t msgnum = 0;
	int64_t room_num = 0;
	std::string subject;
	std::string author;
	int64_t msgtime = 0;
};

// The room-number IN list. Inlined rather than bound: these are numbers this
// request just read out of the store, never anything the client supplied.
std::string RoomInList(const std::vector<Room> &rooms) {
	std::string out;
	for (auto &room : rooms) {
		if (!out.empty()) {
			out += ",";
		}
		out += std::to_string(room.room_num);
	}
	return out;
}

// The date half of the WHERE clause. Both bounds are integers computed above,
// so they are inlined for the same reason the room numbers are.
std::string TimeClause(int64_t since, int64_t until) {
	std::string out;
	if (since > 0) {
		out += " AND m.msgtime >= " + std::to_string(since);
	}
	if (until > 0) {
		out += " AND m.msgtime <= " + std::to_string(until);
	}
	return out;
}

// Newest first, msgnum breaking the tie so the order is total and a paged
// result cannot show one message twice.
const char *kOrder = " ORDER BY m.msgtime DESC, m.msgnum DESC";

// Subject and sender: one statement, the whole store, no cap needed.
//
// contains() rather than LIKE, so a term holding '%' or '_' needs no escaping
// and means what it looks like.
std::vector<Hit> SearchHeaders(Ctx &ctx, const std::string &rooms, const std::string &field,
                               const std::string &term, int64_t since, int64_t until) {
	std::vector<Hit> out;
	// CAST rather than a bare $1: contains() is overloaded for lists and maps as
	// well as strings, and an untyped parameter leaves DuckDB with no best
	// candidate to choose.
	std::string pred;
	if (field == "sub") {
		pred = "contains(lower(coalesce(m.subject, '')), CAST($1 AS VARCHAR))";
	} else if (field == "from") {
		pred = "contains(lower(coalesce(m.author, '')), CAST($1 AS VARCHAR))";
	} else {
		pred = "(contains(lower(coalesce(m.subject, '')), CAST($1 AS VARCHAR)) OR "
		       "contains(lower(coalesce(m.author, '')), CAST($1 AS VARCHAR)))";
	}
	auto r = Exec(ctx.con,
	              "SELECT m.msgnum, rm.room_num, coalesce(m.subject, ''), coalesce(m.author, ''), "
	              "coalesce(m.msgtime, 0) "
	              "FROM citadel_room_msgs rm JOIN citadel_messages m ON m.msgnum = rm.msgnum "
	              "WHERE rm.room_num IN (" +
	                  rooms + ")" + TimeClause(since, until) + " AND " + pred + kOrder,
	              {Value(term)});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		Hit hit;
		hit.msgnum = mat.GetValue(0, i).GetValue<int64_t>();
		hit.room_num = mat.GetValue(1, i).GetValue<int64_t>();
		hit.subject = mat.GetValue(2, i).GetValue<std::string>();
		hit.author = mat.GetValue(3, i).GetValue<std::string>();
		hit.msgtime = mat.GetValue(4, i).GetValue<int64_t>();
		out.push_back(hit);
	}
	return out;
}

// Message text. The store cannot answer this one: `raw` holds RFC822 bytes for
// a format_type 4 message, so its body is very often base64 and a contains()
// over the column would match transfer encoding and header noise while missing
// the words actually in the message. citadel::BodyText is what turns either
// storage form into text, and it needs the message in hand.
//
// So SQL narrows to the room set and the date range, newest first, and the
// scan happens here — capped, with `capped` reported so the page can say so.
std::vector<Hit> SearchBodies(Ctx &ctx, const std::string &rooms, const std::string &term,
                              int64_t since, int64_t until, int64_t scan, bool &capped) {
	std::vector<Hit> out;
	capped = false;
	auto r = Exec(ctx.con,
	              "SELECT m.msgnum, rm.room_num "
	              "FROM citadel_room_msgs rm JOIN citadel_messages m ON m.msgnum = rm.msgnum "
	              "WHERE rm.room_num IN (" +
	                  rooms + ")" + TimeClause(since, until) + kOrder + " LIMIT " +
	                  std::to_string(scan + 1),
	              {});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	idx_t rows = mat.RowCount();
	if ((int64_t)rows > scan) {
		capped = true;
		rows = (idx_t)scan;
	}
	// One message at a time, deliberately. Selecting `raw` for the whole
	// candidate set in the query above would be one scan instead of `scan`
	// point lookups, but it would also materialize every one of those bodies at
	// once — and a body is where the attachments are, so the peak is unbounded
	// in a way the row count does not show. LoadMessage is a primary-key lookup;
	// the MIME parse below costs more than the query does either way.
	for (idx_t i = 0; i < rows; i++) {
		int64_t msgnum = mat.GetValue(0, i).GetValue<int64_t>();
		Message msg;
		if (!quackmail::citadel::LoadMessage(ctx.con, msgnum, msg)) {
			continue;
		}
		if (quackmail::util::Lower(quackmail::citadel::BodyText(msg)).find(term) == std::string::npos) {
			continue;
		}
		Hit hit;
		hit.msgnum = msgnum;
		hit.room_num = mat.GetValue(1, i).GetValue<int64_t>();
		hit.subject = msg.subject;
		hit.author = msg.author;
		hit.msgtime = msg.msgtime;
		out.push_back(hit);
	}
	return out;
}

// ---- the page ------------------------------------------------------------

// The query string for a link back to this same search on another page. Every
// value is percent-encoded: `q` is whatever somebody typed.
std::string QueryString(const std::string &q, const std::string &field, int64_t room,
                        const std::string &since, const std::string &until, int64_t per, int64_t page) {
	std::string out = "?q=" + http::PercentEncode(q) + "&in=" + http::PercentEncode(field);
	if (room >= 0) {
		out += "&room=" + std::to_string(room);
	}
	if (!since.empty()) {
		out += "&since=" + http::PercentEncode(since);
	}
	if (!until.empty()) {
		out += "&until=" + http::PercentEncode(until);
	}
	out += "&n=" + std::to_string(per) + "&p=" + std::to_string(page);
	return out;
}

std::string SearchForm(const std::string &q, const std::string &field, int64_t room,
                       const std::string &since, const std::string &until,
                       const std::vector<Room> &rooms) {
	// A GET form, so there is no CSRF token here and none is wanted: the router
	// gates POST, and a search that could not be linked to or bookmarked would
	// be the worse thing.
	std::string out = "<form method=\"get\" action=\"/search\" class=\"searchform\">";
	out += "<label class=\"field\"><span>Find</span>" +
	       TextInput("q", q, "search", "words to look for") + "</label>";
	out += "<label class=\"field\"><span>In</span>" + Select("in", FieldOptions(), field) + "</label>";

	std::vector<std::pair<std::string, std::string>> room_opts;
	room_opts.push_back({"", "Every room"});
	for (auto &r : rooms) {
		room_opts.push_back({std::to_string(r.room_num), r.display_name});
	}
	out += "<label class=\"field\"><span>Room</span>" +
	       Select("room", room_opts, room >= 0 ? std::to_string(room) : "") + "</label>";

	out += "<label class=\"field\"><span>From</span>" + TextInput("since", since, "date") + "</label>";
	out += "<label class=\"field\"><span>To</span>" + TextInput("until", until, "date") + "</label>";
	out += "<div class=\"actions\"><button class=\"btn\">Search</button></div>";
	out += "</form>";
	return out;
}

void GetSearch(Ctx &ctx) {
	std::string q = ctx.req.Param("q");
	// Trim: a trailing space from a phone keyboard should not change the answer.
	while (!q.empty() && (q.front() == ' ' || q.front() == '\t')) {
		q.erase(0, 1);
	}
	while (!q.empty() && (q.back() == ' ' || q.back() == '\t')) {
		q.pop_back();
	}
	std::string field = FieldOrDefault(ctx.req.Param("in"));
	std::string since_raw = ctx.req.Param("since");
	std::string until_raw = ctx.req.Param("until");
	int64_t want_room = ctx.ParamInt("room", -1);
	int64_t per = std::min<int64_t>(std::max<int64_t>(ctx.ParamInt("n", kDefaultPageSize), 5), 200);
	int64_t page = std::max<int64_t>(ctx.ParamInt("p", 1), 1);

	std::vector<Room> rooms = SearchableRooms(ctx);
	// A room number that is not in the searchable set is simply not a scope —
	// it never becomes a query against that room.
	bool scoped = false;
	for (auto &r : rooms) {
		scoped = scoped || r.room_num == want_room;
	}
	if (!scoped) {
		want_room = -1;
	}

	// Parsed before the form is built, and the raw value dropped when it does
	// not parse, so the field, the pager links and the query that actually ran
	// all agree about what the bounds were.
	std::string zone = EffectiveTz(ctx);
	int64_t since = 0, until = 0;
	if (since_raw.empty() || !ParseBound(since_raw, zone, false, since)) {
		since_raw.clear();
		since = 0;
	}
	if (until_raw.empty() || !ParseBound(until_raw, zone, true, until)) {
		until_raw.clear();
		until = 0;
	}

	PageOpts opts;
	opts.active = "search";
	opts.wide = true;

	std::string body = SearchForm(q, field, want_room, since_raw, until_raw, rooms);
	if (q.empty()) {
		body += "<p class=\"muted\">Type something to look for. Subject and sender are matched across "
		        "every room you can read; message text is matched over the most recent messages.</p>";
		Render(ctx, "Search", body, opts);
		return;
	}

	std::vector<Room> scope;
	for (auto &r : rooms) {
		if (want_room < 0 || r.room_num == want_room) {
			scope.push_back(r);
		}
	}
	if (scope.empty()) {
		body += "<p class=\"muted\">There are no rooms to search.</p>";
		Render(ctx, "Search", body, opts);
		return;
	}

	std::string term = quackmail::util::Lower(q);
	std::string in_list = RoomInList(scope);
	bool capped = false;
	int64_t scan = (int64_t)std::strtoll(ConfigStr(ctx.con, "qm_web_search_scan",
	                                               std::to_string(kDefaultScan))
	                                         .c_str(),
	                                     nullptr, 10);
	if (scan <= 0) {
		scan = kDefaultScan;
	}
	std::vector<Hit> hits = field == "body"
	                            ? SearchBodies(ctx, in_list, term, since, until, scan, capped)
	                            : SearchHeaders(ctx, in_list, field, term, since, until);

	if (hits.empty()) {
		body += "<p class=\"muted\">Nothing matched.</p>";
		if (capped) {
			body += "<p class=\"muted\">Only the newest " + std::to_string(scan) +
			        " messages in these rooms were read. Narrow the dates or pick one room to look "
			        "further back.</p>";
		}
		Render(ctx, "Search", body, opts);
		return;
	}

	int64_t total_pages = (int64_t)((hits.size() + (size_t)per - 1) / (size_t)per);
	if (page > total_pages) {
		page = total_pages;
	}
	size_t begin = (size_t)((page - 1) * per);
	size_t end = std::min(hits.size(), begin + (size_t)per);

	body += "<p class=\"muted\">" + std::to_string(hits.size()) +
	        (hits.size() == 1 ? " match" : " matches") + ".</p>";
	if (capped) {
		body += "<p class=\"muted\">Message text was matched over the newest " + std::to_string(scan) +
		        " messages in these rooms; there are older ones this did not read.</p>";
	}

	// Room numbers to rooms, so a result row can name the room it came from
	// without a lookup per row.
	auto room_named = [&](int64_t num) -> const Room * {
		for (auto &r : scope) {
			if (r.room_num == num) {
				return &r;
			}
		}
		return nullptr;
	};

	body += "<div class=\"wrap\"><table><tr>" + Head("Subject") + Head("From") + Head("Room") +
	        Head("Date") + "</tr>";
	for (size_t i = begin; i < end; i++) {
		const Room *room = room_named(hits[i].room_num);
		if (!room) {
			continue; // impossible: every hit came out of `scope`
		}
		std::string subject = DecodeHeader(hits[i].subject);
		if (subject.empty()) {
			subject = "(no subject)";
		}
		body += "<tr>";
		body += "<td>" +
		        Link(RoomHref(*room, "/msg/" + std::to_string(hits[i].msgnum)), subject) + "</td>";
		body += Cell(DecodeHeader(hits[i].author));
		body += "<td>" + Link(RoomHref(*room), room->display_name) + "</td>";
		body += Cell(FormatTime(ctx, hits[i].msgtime));
		body += "</tr>";
	}
	body += "</table></div>";

	if (total_pages > 1) {
		body += "<div class=\"pager\">";
		if (page > 1) {
			body += Link("/search" + QueryString(q, field, want_room, since_raw, until_raw, per, page - 1),
			             "Newer");
		}
		body += "<span class=\"muted\">Page " + std::to_string(page) + " of " +
		        std::to_string(total_pages) + "</span>";
		if (page < total_pages) {
			body += Link("/search" + QueryString(q, field, want_room, since_raw, until_raw, per, page + 1),
			             "Older");
		}
		body += "</div>";
	}

	Render(ctx, "Search", body, opts);
}

} // namespace

void RegisterSearchRoutes(std::vector<Route> &out) {
	out.push_back({"GET", "/search", Role::User, GetSearch});
}

} // namespace qmweb
} // namespace duckdb
