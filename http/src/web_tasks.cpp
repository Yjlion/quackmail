#include "web_views.hpp"

#include "quackmail/html_sanitize.hpp"
#include "quackmail/ical.hpp"
#include "quackmail/tz.hpp"

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

struct Stored {
	int64_t msgnum = 0;
	std::string body;
	ical::Item item;
};

// Every VTODO in the room. A task is a VTODO in a text/calendar part — the same
// storage as an event, which is why VIEW_TASKS and VIEW_CALENDAR rooms are
// interchangeable as far as the store is concerned.
std::vector<Stored> LoadTasks(Ctx &ctx, const Room &room) {
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
		std::vector<ical::Item> items;
		if (!ical::ParseItems(body, items)) {
			continue;
		}
		for (auto &it : items) {
			if (it.kind != ical::Item::Todo) {
				continue;
			}
			Stored s;
			s.msgnum = num;
			s.body = body;
			s.item = it;
			out.push_back(s);
			break;
		}
	}

	// Unfinished first, then by due date — a task list is a work queue, so
	// something overdue belongs at the top and something done belongs out of the
	// way. A task with no due date sorts after those that have one.
	std::sort(out.begin(), out.end(), [](const Stored &a, const Stored &b) {
		bool a_done = a.item.percent_complete >= 100 || a.item.status == "COMPLETED";
		bool b_done = b.item.percent_complete >= 100 || b.item.status == "COMPLETED";
		if (a_done != b_done) {
			return !a_done;
		}
		int64_t ad = a.item.due.valid ? a.item.due.epoch : INT64_MAX;
		int64_t bd = b.item.due.valid ? b.item.due.epoch : INT64_MAX;
		if (ad != bd) {
			return ad < bd;
		}
		return a.item.summary < b.item.summary;
	});
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
	std::vector<ical::Item> items;
	if (!ical::ParseItems(out.body, items)) {
		return false;
	}
	for (auto &it : items) {
		if (it.kind == ical::Item::Todo) {
			out.item = it;
			return true;
		}
	}
	return false;
}

bool IsDone(const ical::Item &item) {
	return item.percent_complete >= 100 || item.status == "COMPLETED";
}

// The date part of a stored value, as "2026-04-01".
//
// An all-day value already holds a wall-clock date (see ical.hpp), so it must not
// be shifted through the zone a second time — doing so moved a 1 April due date
// to 31 March for any viewer west of Greenwich, which is exactly what a task list
// must not do.
std::string DateOnly(const std::string &zone, const ical::DateTime &dt) {
	int64_t wall = (dt.all_day || zone.empty()) ? dt.epoch : quackmail::tz::FromUtc(zone, dt.epoch);
	int64_t days = wall / 86400;
	if (wall % 86400 < 0) {
		days--;
	}
	std::time_t midnight = (std::time_t)(days * 86400);
	struct tm tm {};
	gmtime_r(&midnight, &tm);
	char buf[16];
	std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
	return buf;
}

std::string ItemHref(const Room &room, int64_t msgnum, const char *suffix = "") {
	return RoomHref(room, "/item/" + std::to_string(msgnum) + suffix);
}

void Index(Ctx &ctx, const Room &room) {
	std::string zone = EffectiveTz(ctx);
	auto tasks = LoadTasks(ctx, room);
	bool may_post = quackmail::citadel::CanPost(ctx.con, ctx.username, room);
	bool show_done = ctx.req.Param("done") == "1";
	int64_t now = (int64_t)std::time(nullptr);

	std::string toolbar = "<div class=\"actions\">";
	if (may_post) {
		toolbar += Link(RoomHref(room, "/item/new"), "New task", "btn");
	}
	toolbar += Link(RoomHref(room) + (show_done ? "" : "?done=1"),
	                show_done ? "Hide completed" : "Show completed", "btn sec");
	toolbar += Link(RoomHref(room) + "?view=raw", "View as messages", "btn sec");
	toolbar += "</div>";

	std::string body;
	int64_t shown = 0, done_count = 0;
	std::string rows;
	for (auto &s : tasks) {
		bool done = IsDone(s.item);
		if (done) {
			done_count++;
			if (!show_done) {
				continue;
			}
		}
		shown++;
		bool overdue = !done && s.item.due.valid && s.item.due.epoch < now;
		rows += "<tr class=\"" + std::string(done ? "taskdone" : (overdue ? "unread" : "")) + "\">";
		// The checkbox is a one-field form rather than a link: completing a task
		// changes state, and a GET that changes state is prefetchable.
		rows += "<td>";
		if (may_post) {
			rows += FormStart(ctx, RoomHref(room, "/item/complete"), "inline") +
			        Hidden("msgnum", std::to_string(s.msgnum)) + Hidden("done", done ? "0" : "1") +
			        Button(done ? "Reopen" : "Done", "sec") + FormEnd();
		}
		rows += "</td>";
		rows += "<td>" + Link(ItemHref(room, s.msgnum), s.item.summary.empty() ? "(no title)"
		                                                                      : s.item.summary) +
		        "</td>";
		rows += Cell(s.item.due.valid ? DateOnly(zone, s.item.due) : "");
		rows += "<td class=\"num\">" + T(s.item.priority > 0 ? std::to_string(s.item.priority) : "") +
		        "</td>";
		rows += "<td class=\"num\">" +
		        T(s.item.percent_complete > 0 ? std::to_string(s.item.percent_complete) + "%" : "") +
		        "</td>";
		rows += "</tr>";
	}

	if (shown == 0) {
		body += "<p class=\"muted\">Nothing to do here.</p>";
	} else {
		body += "<div class=\"wrap\"><table class=\"longlist\"><tr>" + Head("") + Head("Task") +
		        Head("Due") + "<th class=\"num\">Priority</th>" + "<th class=\"num\">Done</th></tr>";
		body += RawHtml(rows);
		body += "</table></div>";
	}
	if (done_count > 0 && !show_done) {
		body += "<p class=\"muted\">" + T(std::to_string(done_count)) +
		        (done_count == 1 ? " completed task hidden." : " completed tasks hidden.") + "</p>";
	}

	PageOpts opts;
	opts.active = "tasks";
	opts.view = (int)room.default_view;
	opts.wide = true;
	opts.toolbar = toolbar;
	Render(ctx, room.display_name, body, opts);
}

void Item(Ctx &ctx, const Room &room, int64_t msgnum) {
	Stored s;
	if (!LoadOne(ctx, room, msgnum, s)) {
		NotFound(ctx);
		return;
	}
	std::string zone = EffectiveTz(ctx);

	std::string body = "<div class=\"msghead\"><dl>";
	auto row = [&](const char *label, const std::string &value) {
		if (!value.empty()) {
			body += "<dt>" + T(label) + "</dt><dd>" + T(value) + "</dd>";
		}
	};
	row("Due", s.item.due.valid ? DateOnly(zone, s.item.due) : "");
	row("Status", IsDone(s.item) ? "Completed" : (s.item.status.empty() ? "Open" : s.item.status));
	row("Priority", s.item.priority > 0 ? std::to_string(s.item.priority) : "");
	row("Progress", s.item.percent_complete > 0 ? std::to_string(s.item.percent_complete) + "%" : "");
	row("Completed", s.item.completed.valid ? DateOnly(zone, s.item.completed) : "");
	body += "</dl></div>";
	if (!s.item.description.empty()) {
		body += s.item.desc_format.empty()
		            ? "<pre class=\"body\">" + T(s.item.description) + "</pre>"
		            : "<div class=\"richnote\">" +
		                  RawHtml(RenderFormattedBody(s.item.description, s.item.desc_format)) + "</div>";
	}

	std::string toolbar = "<div class=\"actions\">";
	toolbar += Link(RoomHref(room), "Back to tasks", "btn sec");
	if (quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		toolbar += Link(ItemHref(room, msgnum, "/edit"), "Edit", "btn sec");
		toolbar += FormStart(ctx, RoomHref(room, "/item/delete"), "inline") +
		           Hidden("msgnum", std::to_string(msgnum)) +
		           "<button class=\"btn danger\" type=\"submit\" data-confirm=\"Delete this task?\">"
		           "Delete</button>" +
		           FormEnd();
	}
	toolbar += "</div>";

	PageOpts opts;
	opts.active = "tasks";
	opts.view = (int)room.default_view;
	opts.toolbar = toolbar;
	Render(ctx, s.item.summary.empty() ? "(no title)" : s.item.summary, body, opts);
}

// A due date is an all-day value, so this returns the wall clock and the caller
// stores it unshifted.
bool ParseDate(const std::string &date, int64_t &wall) {
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
	wall = timegm(&tm);
	return true;
}

void Edit(Ctx &ctx, const Room &room, int64_t msgnum) {
	if (!quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "You cannot add anything to this room.");
		return;
	}
	std::string zone = EffectiveTz(ctx);
	bool editing = msgnum >= 0;
	Stored s;
	if (editing && !LoadOne(ctx, room, msgnum, s)) {
		NotFound(ctx);
		return;
	}
	const ical::Item &item = s.item;

	std::string body = FormStart(ctx, RoomHref(room, "/item/save"));
	if (editing) {
		body += Hidden("msgnum", std::to_string(msgnum));
	}
	body += "<label class=\"field\"><span>Task</span>" + TextInput("summary", item.summary) +
	        "</label>";
	body += "<label class=\"field\"><span>Due</span>" +
	        TextInput("due", item.due.valid ? DateOnly(zone, item.due) : "", "date") +
	        "</label>";
	body += "<label class=\"field\"><span>Priority (1 highest, 9 lowest)</span>" +
	        TextInput("priority", item.priority > 0 ? std::to_string(item.priority) : "", "number") +
	        "</label>";
	body += "<label class=\"field\"><span>Percent complete</span>" +
	        TextInput("percent", std::to_string(item.percent_complete), "number") + "</label>";
	body += "<label class=\"field\"><span>Notes</span>" + TextArea("description", item.description, 6) +
	        "</label>";
	body += "<label class=\"field\"><span>Notes format</span>" +
	        Select("desc_format", {{"", "Plain text"},
	                               {kHtmlContentType, "Formatted text (HTML)"},
	                               {kMarkdownContentType, "Markdown"}},
	               item.desc_format) +
	        "</label>";
	body += "<p>" + Button(editing ? "Save" : "Add task") + " " +
	        Link(editing ? ItemHref(room, msgnum) : RoomHref(room), "Cancel") + "</p>";
	body += FormEnd();

	PageOpts opts;
	opts.active = "tasks";
	opts.view = (int)room.default_view;
	Render(ctx, editing ? "Edit task" : "New task", body, opts);
}

// Build the object from a form, preserving whatever the stored one carried.
bool Persist(Ctx &ctx, const Room &room, bool editing, Stored &s, ical::Item item) {
	item.kind = ical::Item::Todo;
	if (item.uid.empty()) {
		item.uid = ical::NewUid(ConfigStr(ctx.con, "c_fqdn", "localhost"));
	}
	item.sequence++;

	std::string emitted;
	ical::Component root;
	if (editing && ical::Parse(s.body, root) && ical::ApplyItem(root, item)) {
		emitted = ical::Emit(root);
	} else {
		emitted = ical::EmitItem(item, "-//QuackCit//web//EN");
	}
	return SaveObject(ctx, room, ical::EuidFor(item), item.summary, "text/calendar", emitted);
}

void Save(Ctx &ctx, const Room &room) {
	std::string zone = EffectiveTz(ctx);
	int64_t msgnum = ctx.FormInt("msgnum", -1);
	bool editing = msgnum >= 0;
	Stored s;
	if (editing && !LoadOne(ctx, room, msgnum, s)) {
		NotFound(ctx);
		return;
	}
	ical::Item item = s.item;

	std::string summary = ctx.req.Form("summary");
	if (summary.empty()) {
		BadRequest(ctx, "A task needs a description.");
		return;
	}
	item.summary = summary;
	item.description = ctx.req.Form("description");
	// A third state ResolveFormat() does not model: "" means plain text, kept
	// on the wire as an ordinary DESCRIPTION every other client already reads.
	std::string desc_format = ctx.req.Form("desc_format");
	if (desc_format == kMarkdownContentType) {
		item.desc_format = kMarkdownContentType;
	} else if (desc_format == kHtmlContentType) {
		item.desc_format = kHtmlContentType;
		// Sanitized *before* storage, same as every other rich-text view.
		item.description = quackmail::html::SanitizeForCompose(item.description);
	} else {
		item.desc_format = "";
	}
	item.priority = (int)ctx.FormInt("priority", 0);
	if (item.priority < 0 || item.priority > 9) {
		item.priority = 0;
	}
	item.percent_complete = (int)ctx.FormInt("percent", 0);
	item.percent_complete = std::max(0, std::min(100, item.percent_complete));

	std::string due = ctx.req.Form("due");
	if (due.empty()) {
		item.due = ical::DateTime();
	} else {
		int64_t when = 0;
		if (!ParseDate(due, when)) {
			BadRequest(ctx, "That due date is not a date.");
			return;
		}
		item.due = ical::DateTime();
		item.due.epoch = when;
		item.due.all_day = true;
		item.due.valid = true;
	}
	// A task at 100% is completed, and one below it is not — otherwise the
	// percentage and the status could disagree.
	if (item.percent_complete >= 100) {
		item.status = "COMPLETED";
		if (!item.completed.valid) {
			item.completed = ical::DateTime();
			item.completed.epoch = (int64_t)std::time(nullptr);
			item.completed.utc = true;
			item.completed.valid = true;
		}
	} else if (item.status == "COMPLETED") {
		item.status = "IN-PROCESS";
		item.completed = ical::DateTime();
	}

	// VTODO has no DTSTART requirement, but Expand needs one to place the object
	// and the store needs a subject; the due date is the honest choice.
	if (!item.start.valid && item.due.valid) {
		item.start = item.due;
	}

	if (!Persist(ctx, room, editing, s, item)) {
		return;
	}
	RedirectTo(ctx, RoomHref(room), editing ? "saved" : "created");
}

// The one-click complete toggle from the list.
void Complete(Ctx &ctx, const Room &room) {
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
	ical::Item item = s.item;
	if (ctx.req.Form("done") == "1") {
		item.percent_complete = 100;
		item.status = "COMPLETED";
		item.completed = ical::DateTime();
		item.completed.epoch = (int64_t)std::time(nullptr);
		item.completed.utc = true;
		item.completed.valid = true;
	} else {
		item.percent_complete = 0;
		item.status = "NEEDS-ACTION";
		item.completed = ical::DateTime();
	}
	if (!Persist(ctx, room, true, s, item)) {
		return;
	}
	RedirectTo(ctx, RoomHref(room), "saved");
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

const RoomViewHandler kTasks = {quackmail::citadel::VIEW_TASKS, "Tasks", "task",
                                Index,                          Item,    Edit,
                                Save,                           Remove};

} // namespace

const RoomViewHandler &TasksView() {
	return kTasks;
}

void TasksComplete(Ctx &ctx, const Room &room) {
	Complete(ctx, room);
}

} // namespace qmweb
} // namespace duckdb
