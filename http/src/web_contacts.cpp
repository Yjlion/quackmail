#include "web_views.hpp"

#include "quackmail/util.hpp"
#include "quackmail/vcard.hpp"

#include <algorithm>
#include <cstdlib>

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Message;
using quackmail::citadel::Room;
namespace vcard = quackmail::vcard;

namespace {

// One contact as it sits in a room: the card, plus the message number carrying
// it so a link can point at it.
struct Entry {
	int64_t msgnum = 0;
	vcard::Card card;
};

// Every contact in the room, sorted by display name.
//
// Messages that are not contacts are skipped rather than shown as broken rows.
// A Contacts room can hold an ordinary message — someone mails the address book,
// or a room's view is changed after the fact — and `?view=raw` is how those are
// found when they need to be.
std::vector<Entry> LoadContacts(Ctx &ctx, const Room &room) {
	std::vector<Entry> out;
	auto nums = quackmail::citadel::RoomMessages(ctx.con, room.room_num, "all", 0, 0);
	for (int64_t num : nums) {
		Message msg;
		if (!quackmail::citadel::LoadMessage(ctx.con, num, msg)) {
			continue;
		}
		std::string body = ObjectBody(msg, "text/vcard");
		if (body.empty()) {
			continue;
		}
		Entry e;
		e.msgnum = num;
		if (!vcard::ParseOne(body, e.card)) {
			continue;
		}
		out.push_back(e);
	}
	std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
		// Case-insensitive, so "de Bruijn" and "De Bruijn" sort together.
		return quackmail::util::Lower(a.card.Fn()) < quackmail::util::Lower(b.card.Fn());
	});
	return out;
}

bool LoadContact(Ctx &ctx, const Room &room, int64_t msgnum, vcard::Card &out) {
	Message msg;
	if (!LoadMessageIn(ctx, room, msgnum, msg)) {
		return false;
	}
	std::string body = ObjectBody(msg, "text/vcard");
	if (body.empty()) {
		return false;
	}
	return vcard::ParseOne(body, out);
}

std::string ItemHref(const Room &room, int64_t msgnum, const char *suffix = "") {
	return RoomHref(room, "/item/" + std::to_string(msgnum) + suffix);
}

// ---- the list ------------------------------------------------------------

void Index(Ctx &ctx, const Room &room) {
	auto entries = LoadContacts(ctx, room);
	bool may_post = quackmail::citadel::CanPost(ctx.con, ctx.username, room);

	std::string toolbar = "<div class=\"actions\">";
	if (may_post) {
		toolbar += Link(RoomHref(room, "/item/new"), "Add a contact", "btn");
	}
	// Any room can still be read as what it physically is. The first time a
	// Contacts room contains something that is not a vCard, this is how you see
	// it.
	toolbar += Link(RoomHref(room) + "?view=raw", "View as messages", "btn sec");
	toolbar += "</div>";

	std::string body;
	if (entries.empty()) {
		body += "<p class=\"muted\">No contacts here yet.</p>";
	} else {
		body += "<div class=\"wrap\"><table class=\"longlist\"><tr>" + Head("Name") + Head("E-mail") +
		        Head("Telephone") + Head("Organisation") + "</tr>";
		for (auto &e : entries) {
			body += "<tr>";
			body += "<td>" + Link(ItemHref(room, e.msgnum), e.card.Fn()) + "</td>";

			std::string mails;
			for (auto &m : e.card.Emails()) {
				if (!mails.empty()) {
					mails += "<br>";
				}
				// A contact's address is a link that composes to them, which is
				// the thing an address book is for.
				mails += Link("/mail/compose?to=" + http::PercentEncode(m), m);
			}
			body += "<td>" + RawHtml(mails) + "</td>";

			std::string tels;
			for (auto &t : e.card.Phones()) {
				if (!tels.empty()) {
					tels += "<br>";
				}
				tels += T(t);
			}
			body += "<td>" + RawHtml(tels) + "</td>";

			const vcard::Property *org = e.card.Find("ORG");
			body += Cell(org ? (org->values.empty() ? "" : org->values[0]) : "");
			body += "</tr>";
		}
		body += "</table></div>";
		body += "<p class=\"muted\">" + T(std::to_string(entries.size())) +
		        (entries.size() == 1 ? " contact." : " contacts.") + "</p>";
	}

	PageOpts opts;
	opts.active = "bbs";
	opts.view = (int)room.default_view;
	opts.wide = true;
	opts.toolbar = toolbar;
	Render(ctx, room.display_name, body, opts);
}

// ---- one contact ---------------------------------------------------------

// A definition row, omitted entirely when the value is empty — a detail pane
// full of blank labels is worse than a short one.
void Row(std::string &out, const char *label, const std::string &value) {
	if (value.empty()) {
		return;
	}
	out += "<dt>" + T(label) + "</dt><dd>" + T(value) + "</dd>";
}

void Item(Ctx &ctx, const Room &room, int64_t msgnum) {
	vcard::Card card;
	if (!LoadContact(ctx, room, msgnum, card)) {
		NotFound(ctx);
		return;
	}

	std::string body = "<div class=\"msghead\"><dl>";
	const vcard::Property *n = card.Find("N");
	if (n && n->values.size() >= 2) {
		Row(body, "Given name", n->values.size() > 1 ? n->values[1] : "");
		Row(body, "Family name", n->values[0]);
	}
	const vcard::Property *org = card.Find("ORG");
	if (org && !org->values.empty()) {
		Row(body, "Organisation", org->values[0]);
		if (org->values.size() > 1) {
			Row(body, "Department", org->values[1]);
		}
	}
	Row(body, "Title", card.Find("TITLE") ? card.Find("TITLE")->Value() : "");

	for (auto *p : card.FindAll("EMAIL")) {
		std::string label = p->HasType("work") ? "E-mail (work)"
		                    : p->HasType("home") ? "E-mail (home)"
		                                         : "E-mail";
		body += "<dt>" + T(label) + "</dt><dd>" +
		        Link("/mail/compose?to=" + http::PercentEncode(p->Value()), p->Value()) + "</dd>";
	}
	for (auto *p : card.FindAll("TEL")) {
		std::string label = p->HasType("cell")   ? "Mobile"
		                    : p->HasType("work") ? "Telephone (work)"
		                    : p->HasType("home") ? "Telephone (home)"
		                    : p->HasType("fax")  ? "Fax"
		                                         : "Telephone";
		Row(body, label.c_str(), p->Value());
	}
	for (auto *p : card.FindAll("ADR")) {
		// ADR is post-office-box;extended;street;locality;region;code;country.
		// Rendered as a single readable line rather than seven labelled rows.
		std::string line;
		for (size_t i = 2; i < p->values.size(); i++) {
			if (p->values[i].empty()) {
				continue;
			}
			if (!line.empty()) {
				line += ", ";
			}
			line += p->values[i];
		}
		Row(body, p->HasType("work") ? "Address (work)" : "Address", line);
	}
	Row(body, "Web", card.Find("URL") ? card.Find("URL")->Value() : "");
	Row(body, "Birthday", card.Find("BDAY") ? card.Find("BDAY")->Value() : "");
	Row(body, "Note", card.Find("NOTE") ? card.Find("NOTE")->Value() : "");
	body += "</dl></div>";

	std::string toolbar = "<div class=\"actions\">";
	toolbar += Link(RoomHref(room), "Back to contacts", "btn sec");
	if (quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
		toolbar += Link(ItemHref(room, msgnum, "/edit"), "Edit", "btn sec");
		toolbar += FormStart(ctx, RoomHref(room, "/item/delete"), "inline") +
		           Hidden("msgnum", std::to_string(msgnum)) +
		           "<button class=\"btn danger\" type=\"submit\" data-confirm=\"Delete this contact?\">"
		           "Delete</button>" +
		           FormEnd();
	}
	// The card as it actually is, for anyone who needs to see what a client
	// wrote rather than what this page chose to show.
	toolbar += Link(RoomHref(room, "/msg/" + std::to_string(msgnum) + "/source"), "vCard source",
	                "btn sec");
	toolbar += "</div>";

	PageOpts opts;
	opts.active = "bbs";
	opts.view = (int)room.default_view;
	opts.toolbar = toolbar;
	Render(ctx, card.Fn(), body, opts);
}

// ---- the form ------------------------------------------------------------

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

	vcard::Card card;
	bool editing = msgnum >= 0;
	if (editing && !LoadContact(ctx, room, msgnum, card)) {
		NotFound(ctx);
		return;
	}

	auto val = [&](const char *prop) {
		const vcard::Property *p = card.Find(prop);
		return p ? p->Value() : std::string();
	};
	auto comp = [&](const char *prop, size_t i) {
		const vcard::Property *p = card.Find(prop);
		return (p && p->values.size() > i) ? p->values[i] : std::string();
	};
	// The first of each, since the form models one. Any others the card carries
	// are left untouched — see the note on SaveObject.
	auto first = [&](const char *prop) {
		auto all = card.FindAll(prop);
		return all.empty() ? std::string() : all[0]->Value();
	};

	std::string body = FormStart(ctx, RoomHref(room, "/item/save"));
	if (editing) {
		body += Hidden("msgnum", std::to_string(msgnum));
	}
	body += Field("given", "Given name", comp("N", 1));
	body += Field("family", "Family name", comp("N", 0));
	body += Field("fn", "Display name", val("FN"));
	body += "<p class=\"muted\">Left blank, the display name is assembled from the two above.</p>";
	body += Field("org", "Organisation", comp("ORG", 0));
	body += Field("title", "Title", val("TITLE"));
	body += Field("email", "E-mail", first("EMAIL"), "email");
	body += Field("tel", "Telephone", first("TEL"), "text");
	body += Field("url", "Web", val("URL"), "text");
	body += Field("bday", "Birthday", val("BDAY"), "text");
	body += "<label class=\"field\"><span>Note</span>" + TextArea("note", val("NOTE"), 4) + "</label>";
	body += "<p>" + Button(editing ? "Save" : "Add contact") + " " +
	        Link(editing ? ItemHref(room, msgnum) : RoomHref(room), "Cancel") + "</p>";
	body += FormEnd();

	if (editing) {
		body += "<p class=\"muted\">Fields this form does not show — extra addresses, custom "
		        "properties written by another program — are kept as they are.</p>";
	}

	PageOpts opts;
	opts.active = "bbs";
	opts.view = (int)room.default_view;
	Render(ctx, editing ? "Edit contact" : "Add a contact", body, opts);
}

void Save(Ctx &ctx, const Room &room) {
	int64_t msgnum = ctx.FormInt("msgnum", -1);

	// Editing starts from the card as stored, so every property the form does
	// not model survives. Building a fresh card from the form fields would
	// silently delete a second address, a photo, or anything a phone wrote.
	vcard::Card card;
	bool editing = msgnum >= 0;
	if (editing && !LoadContact(ctx, room, msgnum, card)) {
		NotFound(ctx);
		return;
	}

	std::string given = ctx.req.Form("given");
	std::string family = ctx.req.Form("family");
	std::string fn = ctx.req.Form("fn");
	if (fn.empty()) {
		fn = given;
		if (!family.empty()) {
			if (!fn.empty()) {
				fn += " ";
			}
			fn += family;
		}
	}
	if (fn.empty() && ctx.req.Form("email").empty()) {
		BadRequest(ctx, "A contact needs at least a name or an e-mail address.");
		return;
	}

	card.SetComponents("N", {family, given, "", "", ""});
	if (!fn.empty()) {
		card.Set("FN", fn);
	}

	// An emptied field is a removal, not an empty property: a vCard carrying
	// TEL: with no value is worse than one with no TEL at all.
	auto put = [&](const char *prop, const char *field) {
		std::string v = ctx.req.Form(field);
		if (v.empty()) {
			card.Remove(prop);
		} else {
			card.Set(prop, v);
		}
	};
	put("ORG", "org");
	put("TITLE", "title");
	put("EMAIL", "email");
	put("TEL", "tel");
	put("URL", "url");
	put("BDAY", "bday");
	put("NOTE", "note");

	if (card.Uid().empty()) {
		card.Set("UID", vcard::NewUid(ConfigStr(ctx.con, "c_fqdn", "localhost")));
	}

	std::string euid = vcard::EuidFor(card);
	std::string emitted = vcard::Emit(card, card.version);
	if (!SaveObject(ctx, room, euid, card.Fn(), "text/vcard", emitted)) {
		return;
	}
	RedirectTo(ctx, RoomHref(room), editing ? "saved" : "created");
}

void Remove(Ctx &ctx, const Room &room) {
	int64_t msgnum = ctx.FormInt("msgnum", -1);
	// Confirm it is a contact in *this* room before deleting: the message number
	// came from a form.
	vcard::Card card;
	if (!LoadContact(ctx, room, msgnum, card)) {
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

const RoomViewHandler kContacts = {
    quackmail::citadel::VIEW_ADDRESSBOOK, "Contacts", "contact", Index, Item, Edit, Save, Remove};

} // namespace

const RoomViewHandler &ContactsView() {
	return kContacts;
}

} // namespace qmweb
} // namespace duckdb
