#include "web_views.hpp"

#include "quackmail/vnote.hpp"

#include <algorithm>
#include <cstdlib>

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Message;
using quackmail::citadel::Room;
namespace vnote = quackmail::vnote;

namespace {

struct Stored {
	int64_t msgnum = 0;
	vnote::Note note;
};

std::vector<Stored> LoadNotes(Ctx &ctx, const Room &room) {
	std::vector<Stored> out;
	for (int64_t num : quackmail::citadel::RoomMessages(ctx.con, room.room_num, "all", 0, 0)) {
		Message msg;
		if (!quackmail::citadel::LoadMessage(ctx.con, num, msg)) {
			continue;
		}
		std::string body = ObjectBody(msg, "text/vnote");
		if (body.empty()) {
			continue;
		}
		Stored s;
		s.msgnum = num;
		if (!vnote::ParseOne(body, s.note)) {
			continue;
		}
		out.push_back(s);
	}
	// Newest first: a pinboard is a stack, and the note you just wrote is the one
	// you want to see. Message numbers are monotonic, so they are the order.
	std::sort(out.begin(), out.end(),
	          [](const Stored &a, const Stored &b) { return a.msgnum > b.msgnum; });
	return out;
}

bool LoadOne(Ctx &ctx, const Room &room, int64_t msgnum, vnote::Note &out) {
	Message msg;
	if (!LoadMessageIn(ctx, room, msgnum, msg)) {
		return false;
	}
	std::string body = ObjectBody(msg, "text/vnote");
	if (body.empty()) {
		return false;
	}
	return vnote::ParseOne(body, out);
}

std::string ItemHref(const Room &room, int64_t msgnum, const char *suffix = "") {
	return RoomHref(room, "/item/" + std::to_string(msgnum) + suffix);
}

// The note's own colour, if it set one. Validated rather than interpolated
// blindly: this ends up in a style attribute, and the value came out of a file
// somebody else's client wrote.
std::string ColorStyle(const std::string &color) {
	if (color.size() != 7 || color[0] != '#') {
		return std::string();
	}
	for (size_t i = 1; i < color.size(); i++) {
		if (std::isxdigit((unsigned char)color[i]) == 0) {
			return std::string();
		}
	}
	return " style=\"--note:" + color + "\"";
}

void Index(Ctx &ctx, const Room &room) {
	auto notes = LoadNotes(ctx, room);
	bool may_post = quackmail::citadel::CanPost(ctx.con, ctx.username, room);

	std::string toolbar = "<div class=\"actions\">";
	if (may_post) {
		toolbar += Link(RoomHref(room, "/item/new"), "New note", "btn");
	}
	toolbar += Link(RoomHref(room) + "?view=raw", "View as messages", "btn sec");
	toolbar += "</div>";

	std::string body;
	if (notes.empty()) {
		body += "<p class=\"muted\">No notes here yet.</p>";
	} else {
		body += "<div class=\"notegrid\">";
		for (auto &s : notes) {
			body += "<div class=\"note\"" + RawHtml(ColorStyle(s.note.color)) + ">";
			body += "<h3>" + Link(ItemHref(room, s.msgnum), vnote::TitleOf(s.note)) + "</h3>";
			// A preview, not the whole note: a pinboard of full essays is not a
			// pinboard. The detail page has the rest.
			std::string preview = s.note.body;
			if (preview.size() > 240) {
				preview = preview.substr(0, 237) + "...";
			}
			body += "<pre>" + T(preview) + "</pre>";
			body += "</div>";
		}
		body += "</div>";
	}

	PageOpts opts;
	opts.active = "notes";
	opts.view = (int)room.default_view;
	opts.wide = true;
	opts.toolbar = toolbar;
	Render(ctx, room.display_name, body, opts);
}

void Item(Ctx &ctx, const Room &room, int64_t msgnum) {
	vnote::Note note;
	if (!LoadOne(ctx, room, msgnum, note)) {
		NotFound(ctx);
		return;
	}
	std::string body = "<pre class=\"body\">" + T(note.body) + "</pre>";

	std::string toolbar = "<div class=\"actions\">";
	toolbar += Link(RoomHref(room), "Back to notes", "btn sec");
	if (quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		toolbar += Link(ItemHref(room, msgnum, "/edit"), "Edit", "btn sec");
		toolbar += FormStart(ctx, RoomHref(room, "/item/delete"), "inline") +
		           Hidden("msgnum", std::to_string(msgnum)) +
		           "<button class=\"btn danger\" type=\"submit\" data-confirm=\"Delete this note?\">"
		           "Delete</button>" +
		           FormEnd();
	}
	toolbar += "</div>";

	PageOpts opts;
	opts.active = "notes";
	opts.view = (int)room.default_view;
	opts.toolbar = toolbar;
	Render(ctx, vnote::TitleOf(note), body, opts);
}

void Edit(Ctx &ctx, const Room &room, int64_t msgnum) {
	if (!quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "You cannot add anything to this room.");
		return;
	}
	bool editing = msgnum >= 0;
	vnote::Note note;
	if (editing && !LoadOne(ctx, room, msgnum, note)) {
		NotFound(ctx);
		return;
	}

	std::string body = FormStart(ctx, RoomHref(room, "/item/save"));
	if (editing) {
		body += Hidden("msgnum", std::to_string(msgnum));
	}
	body += "<label class=\"field\"><span>Title</span>" + TextInput("summary", note.summary) +
	        "</label>";
	body += "<label class=\"field\"><span>Note</span>" + TextArea("body", note.body, 14) + "</label>";
	body += "<label class=\"field\"><span>Colour</span>" +
	        Select("color",
	                {{"", "Default"},
	                 {"#ffff88", "Yellow"},
	                 {"#aaffaa", "Green"},
	                 {"#aaccff", "Blue"},
	                 {"#ffccaa", "Orange"},
	                 {"#ffaacc", "Pink"}},
	                note.color) +
	        "</label>";
	body += "<p>" + Button(editing ? "Save" : "Add note") + " " +
	        Link(editing ? ItemHref(room, msgnum) : RoomHref(room), "Cancel") + "</p>";
	body += FormEnd();

	PageOpts opts;
	opts.active = "notes";
	opts.view = (int)room.default_view;
	Render(ctx, editing ? "Edit note" : "New note", body, opts);
}

void Save(Ctx &ctx, const Room &room) {
	int64_t msgnum = ctx.FormInt("msgnum", -1);
	bool editing = msgnum >= 0;

	// Start from the stored note so the geometry another client set — where the
	// note sits on its pinboard — survives an edit here.
	vnote::Note note;
	if (editing && !LoadOne(ctx, room, msgnum, note)) {
		NotFound(ctx);
		return;
	}

	note.summary = ctx.req.Form("summary");
	note.body = ctx.req.Form("body");
	if (note.summary.empty() && note.body.empty()) {
		BadRequest(ctx, "A note needs a title or some text.");
		return;
	}
	std::string color = ctx.req.Form("color");
	// Only a colour we offer; anything else clears it rather than being stored.
	note.color = ColorStyle(color).empty() ? "" : color;

	if (note.uid.empty()) {
		note.uid = vnote::NewUid(ConfigStr(ctx.con, "c_fqdn", "localhost"));
	}
	if (!SaveObject(ctx, room, vnote::EuidFor(note), vnote::TitleOf(note), "text/vnote",
	                vnote::Emit(note))) {
		return;
	}
	RedirectTo(ctx, RoomHref(room), editing ? "saved" : "created");
}

void Remove(Ctx &ctx, const Room &room) {
	int64_t msgnum = ctx.FormInt("msgnum", -1);
	vnote::Note note;
	if (!LoadOne(ctx, room, msgnum, note)) {
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

const RoomViewHandler kNotes = {quackmail::citadel::VIEW_NOTES, "Notes", "note",
                                Index,                          Item,    Edit,
                                Save,                           Remove};

} // namespace

const RoomViewHandler &NotesView() {
	return kNotes;
}

} // namespace qmweb
} // namespace duckdb
