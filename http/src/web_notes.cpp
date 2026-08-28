#include "web_views.hpp"

#include "quackmail/html_sanitize.hpp"
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

// Is this a colour we are willing to put in a stylesheet? Validated rather than
// interpolated blindly: the value came out of a file somebody else's client
// wrote, and it ends up in CSS rather than in escaped text.
bool ValidColor(const std::string &color) {
	if (color.size() != 7 || color[0] != '#') {
		return false;
	}
	for (size_t i = 1; i < color.size(); i++) {
		if (std::isxdigit((unsigned char)color[i]) == 0) {
			return false;
		}
	}
	return true;
}

// The colours one page turned out to need, gathered as it renders and emitted
// once as a nonced <style> block.
//
// This used to be a style attribute per note, which never worked: style-src
// carries a nonce and no 'unsafe-inline', and a nonce covers <style> elements
// only — never attributes — so every browser dropped the declaration and both
// the swatches and the cards fell back to the card background. White, in the
// light theme, which is the bug.
//
// A class per colour rather than a fixed palette class per swatch, because
// X-OUTLOOK-COLOR is whatever hex the client that wrote the note chose: real
// Outlook's five differ from ours, and a note somebody else coloured should
// still show its own colour rather than being rounded to the nearest we offer.
class ColorSheet {
public:
	// The tint class for `color`, or "" for a note that set none. Records the
	// colour so Css() can declare it.
	std::string ClassFor(const std::string &color) {
		if (!ValidColor(color)) {
			return std::string();
		}
		std::string lower = color;
		for (auto &ch : lower) {
			ch = (char)std::tolower((unsigned char)ch);
		}
		size_t i = 0;
		for (; i < colors_.size(); i++) {
			if (colors_[i] == lower) {
				break;
			}
		}
		if (i == colors_.size()) {
			colors_.push_back(lower);
		}
		return "note-c" + std::to_string(i);
	}

	// The class attribute for an element that is tinted or not. `base` is the
	// element's own class; "tinted" is what qc.css keys the forced dark text off.
	std::string Attr(const std::string &base, const std::string &color) {
		std::string tint = ClassFor(color);
		std::string all = base;
		if (!tint.empty()) {
			if (!all.empty()) {
				all += " ";
			}
			all += "tinted " + tint;
		}
		return all.empty() ? std::string() : " class=\"" + A(all) + "\"";
	}

	std::string Css() const {
		std::string css;
		for (size_t i = 0; i < colors_.size(); i++) {
			css += ".note-c" + std::to_string(i) + "{--note:" + colors_[i] + "}";
		}
		return css;
	}

private:
	std::vector<std::string> colors_;
};

// The Post-it palette. A vector rather than a fixed array because Edit()
// hands it straight to the swatch renderer, which needs to know how many
// there are.
const std::vector<std::pair<std::string, std::string>> &NoteColors() {
	static const std::vector<std::pair<std::string, std::string>> kColors = {
	    {"", "Default"},          {"#ffff88", "Yellow"}, {"#aaffaa", "Green"},
	    {"#aaccff", "Blue"},      {"#ffccaa", "Orange"}, {"#ffaacc", "Pink"},
	    {"#e0c8ff", "Lavender"},  {"#d9d9d9", "Grey"},
	};
	return kColors;
}

// A row of colour swatches rather than a <select>: which colour a note is sits
// on the same "pick one visually" footing as choosing a highlighter, and a
// dropdown of colour *names* makes you read before you can compare. Plain
// radio inputs, each hidden behind a styled <span> sibling — the same
// checked-sibling-selector trick as the mobile nav toggle, so this needs no
// script.
std::string ColorSwatches(ColorSheet &sheet, const std::string &selected) {
	std::string out = "<div class=\"swatches\">";
	for (auto &c : NoteColors()) {
		bool checked = c.first == selected;
		out += "<label class=\"swatch" + std::string(c.first.empty() ? " swatch-default" : "") +
		       "\" title=\"" + A(c.second) + "\">";
		out += "<input type=\"radio\" name=\"color\" value=\"" + A(c.first) + "\"" +
		       (checked ? " checked" : "") + ">";
		// No "tinted" on a swatch dot: that class is the note card's dark-text
		// override, and a dot has no text.
		std::string tint = sheet.ClassFor(c.first);
		out += "<span" + (tint.empty() ? std::string() : " class=\"" + A(tint) + "\"") + "></span>";
		out += "<span class=\"vh\">" + T(c.second) + "</span>";
		out += "</label>";
	}
	return out + "</div>";
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

	ColorSheet sheet;
	std::string body;
	if (notes.empty()) {
		body += "<p class=\"muted\">No notes here yet.</p>";
	} else {
		body += "<div class=\"notegrid\">";
		for (auto &s : notes) {
			body += "<div" + RawHtml(sheet.Attr("note", s.note.color)) + ">";
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
	opts.style = sheet.Css();
	Render(ctx, room.display_name, body, opts);
}

void Item(Ctx &ctx, const Room &room, int64_t msgnum) {
	vnote::Note note;
	if (!LoadOne(ctx, room, msgnum, note)) {
		NotFound(ctx);
		return;
	}
	// Absent content_type is a note no client marked as HTML or Markdown —
	// including every note written before this field existed — and stays the
	// escaped plain text it has always rendered as.
	std::string body = note.content_type.empty()
	                       ? "<pre class=\"body\">" + T(note.body) + "</pre>"
	                       : "<div class=\"richnote\">" +
	                             RawHtml(RenderFormattedBody(note.body, note.content_type)) + "</div>";

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
	body += "<label class=\"field\"><span>Format</span>" +
	        Select("format",
	               {{"", "Plain text"},
	                {kHtmlContentType, "Formatted text (HTML)"},
	                {kMarkdownContentType, "Markdown"}},
	               note.content_type) +
	        "</label>";
	ColorSheet sheet;
	body += "<div class=\"field\"><span>Colour</span>" + ColorSwatches(sheet, note.color) + "</div>";
	body += "<p>" + Button(editing ? "Save" : "Add note") + " " +
	        Link(editing ? ItemHref(room, msgnum) : RoomHref(room), "Cancel") + "</p>";
	body += FormEnd();

	PageOpts opts;
	opts.active = "notes";
	opts.view = (int)room.default_view;
	opts.style = sheet.Css();
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
	// A third state FormatSelect()/ResolveFormat() do not model: "" means plain
	// text, rendered escaped rather than through a sanitizer — the only choice
	// that existed before this field did, and still the default.
	std::string format = ctx.req.Form("format");
	if (format == kMarkdownContentType) {
		note.content_type = kMarkdownContentType;
	} else if (format == kHtmlContentType) {
		note.content_type = kHtmlContentType;
		// Sanitized *before* storage, so what is kept is already safe rather
		// than depending on every future reader to clean it.
		note.body = quackmail::html::SanitizeForCompose(note.body);
	} else {
		note.content_type = "";
	}
	std::string color = ctx.req.Form("color");
	// Only a colour we offer; anything else clears it rather than being stored.
	note.color = ValidColor(color) ? color : "";

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
