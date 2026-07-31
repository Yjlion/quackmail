#include "web.hpp"

#include "quackmail/fetch.hpp"
#include "quackmail/listserv.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <cstdlib>

// Per-room management, and self-serve room creation.
//
// The point of this file is that it is *not* the admin console. Everything here
// is a Role::User route gated on citadel::CanAdminister — the RFC 4314 `a`
// right — so an aide delegates a room by granting `a` and the delegate manages
// it without ever reaching /admin, which is root-equivalent. The grant can be
// made from this server's console or from any IMAP client's SETACL; there is no
// QuackCit-only administrator column, and nothing here that only the web sees.
//
// Two things deliberately stay aide-only even for a room's own administrator:
//
//   * Creating a feed. fetch::Feed stores a plaintext password and RunFeed
//     dials an arbitrary host, so for a non-aide that is an SSRF primitive with
//     credential storage attached. A room administrator may *run* a feed an
//     aide already pointed at their room, which polls a host an aide chose.
//   * Turning a room into a mailing list, and the addressing that goes with it
//     (the list's local part, its delivery mode, who may post). Those mint an
//     inbound address and a fan-out engine. Once an aide has done it, the room's
//     administrator owns the presentation and the membership.

namespace duckdb {
namespace qmweb {

namespace listserv = quackmail::listserv;

using quackmail::citadel::Room;

namespace {

int64_t CapNum(const Ctx &ctx, size_t i) {
	std::string s = ctx.Cap(i);
	return s.empty() ? -1 : (int64_t)std::strtoll(s.c_str(), nullptr, 10);
}

// ---- what a room administrator may change --------------------------------

struct RoomFlag {
	const char *field;
	int64_t bit;
	const char *label;
};

// Deliberately a subset of the admin console's list. QR_DIRECTORY / QR_UPLOAD /
// QR_DOWNLOAD / QR_VISDIR describe a file area this server does not implement
// yet, and QR_NETWORK is inter-node replication — all three are the operator's
// business, not a room administrator's. QR_MAILBOX is never a checkbox anywhere:
// it is what makes a room somebody's personal folder.
const RoomFlag kRoomFlags[] = {
    {"private", quackmail::citadel::QR_PRIVATE, "Invitation only"},
    {"passworded", quackmail::citadel::QR_PASSWORDED, "Password protected"},
    {"guessname", quackmail::citadel::QR_GUESSNAME, "Guess-name"},
    {"readonly", quackmail::citadel::QR_READONLY, "Read only"},
    {"prefonly", quackmail::citadel::QR_PREFONLY, "Preferred users only"},
    {"permanent", quackmail::citadel::QR_PERMANENT, "Never auto-purge"},
};

int64_t EditableFlagMask() {
	int64_t mask = 0;
	for (auto &f : kRoomFlags) {
		mask |= f.bit;
	}
	return mask;
}

// The new flag word after a save. Bits this form does not offer are carried
// over rather than dropped — a checkbox list that is not exhaustive silently
// clears whatever it forgot, which for QR_MAILBOX would stop a personal room
// being personal.
int64_t FlagsFromForm(Ctx &ctx, const Room &room) {
	int64_t chosen = 0;
	for (auto &f : kRoomFlags) {
		if (ctx.req.HasForm(f.field)) {
			chosen |= f.bit;
		}
	}
	return (room.qr_flags & ~EditableFlagMask()) | chosen;
}

std::vector<std::pair<std::string, std::string>> ViewOptions() {
	// Only the views that have a renderer. VIEW_WIKI and VIEW_QUEUE are in the
	// enum because the wire numbering is Citadel's, not because we draw them;
	// offering one here would set a room to a view that falls back to the plain
	// message list, which reads as a bug rather than as a choice.
	return {{"0", "Message board"}, {"2", "Address book"}, {"3", "Calendar"},
	        {"7", "Calendar, as a list"}, {"4", "Tasks"},  {"5", "Notes"},
	        {"10", "Blog"},              {"8", "Journal"}};
}

std::vector<std::pair<std::string, std::string>> FloorOptions(Ctx &ctx) {
	std::vector<std::pair<std::string, std::string>> out;
	for (auto &f : quackmail::citadel::ListFloors(ctx.con)) {
		out.emplace_back(std::to_string(f.floor_num), f.name);
	}
	return out;
}

// Resolve the room in the path and confirm the caller administers it. Renders
// the 404/403 itself, so callers are one `if` away from their real work.
bool RoomForAdmin(Ctx &ctx, Room &room) {
	if (!ResolveRoomNumFor(ctx, CapNum(ctx, 0), room)) {
		NotFound(ctx);
		return false;
	}
	if (room.mailbox_owner != 0) {
		// A personal folder is not a room anybody administers. Its owner holds
		// every right on it — which is what lets IMAP report MYRIGHTS correctly —
		// but this page is the wrong tool for one: renaming "Mail" would leave
		// IMAP's INBOX and every Sieve `fileinto` pointing at a room that no
		// longer exists, and the next delivery would quietly make a second one.
		Forbidden(ctx, "Personal folders are managed from Mail, not here.");
		return false;
	}
	if (!quackmail::citadel::CanAdminister(ctx.con, ctx.username, room)) {
		Forbidden(ctx, "You do not administer this room. An aide, or the room's current administrator, "
		               "can grant you the \"a\" right on it.");
		return false;
	}
	return true;
}

std::string SettingsHref(const Room &room, const std::string &suffix = "") {
	return "/bbs/room/" + std::to_string(room.room_num) + "/settings" + suffix;
}

// ---- the access list -----------------------------------------------------

struct RightDoc {
	char letter;
	const char *what;
};

const RightDoc kRightDocs[] = {
    {'l', "see that the room exists"},
    {'r', "read messages"},
    {'s', "keep a read pointer"},
    {'w', "set message flags"},
    {'i', "post a message"},
    {'p', "post by e-mail, without signing in"},
    // `k` is stored and reported over IMAP, but nothing in this server acts on
    // it yet. Listing it without saying so would be the page claiming a
    // capability the grant does not actually confer.
    {'k', "create rooms under this one — stored, but nothing acts on it yet"},
    {'x', "delete the room"},
    {'t', "delete a message"},
    {'e', "expunge deleted messages"},
    {'a', "administer the room — including this page"},
};

std::string RightsLegend() {
	std::string out = "<dl class=\"rights\">";
	for (auto &d : kRightDocs) {
		out += "<dt><code>" + T(std::string(1, d.letter)) + "</code></dt><dd>" + T(d.what) + "</dd>";
	}
	return out + "</dl>";
}

std::string AclSection(Ctx &ctx, const Room &room) {
	std::string base = SettingsHref(room);
	std::string out = "<h2>Who may do what</h2>";

	auto acl = quackmail::citadel::ListRights(ctx.con, room);
	out += "<div class=\"wrap\"><table><tr>" + Head("Who") + Head("Rights") + Head("") + "</tr>";
	for (auto &e : acl) {
		out += "<tr>";
		out += Cell(e.first);
		out += "<td>" + FormStart(ctx, base + "/acl", "inline") + Hidden("identifier", e.first) +
		       TextInput("rights", e.second) + Button("Save", "sec") + FormEnd() + "</td>";
		out += "<td>" + FormStart(ctx, base + "/acl", "inline") + Hidden("identifier", e.first) +
		       Hidden("rights", "") + Button("Remove", "danger") + FormEnd() + "</td>";
		out += "</tr>";
	}
	out += "</table></div>";
	if (acl.empty()) {
		out += "<p class=\"muted\">No explicit grants. Access follows the room's own attributes: an aide "
		       "may do anything, and everyone else reads and posts unless the room is private, "
		       "passworded or read-only.</p>";
	}

	out += "<h3>Grant access</h3>";
	out += FormStart(ctx, base + "/acl");
	out += "<label class=\"field\"><span>User name, or <code>anyone</code></span>" +
	       TextInput("identifier", "") + "</label>";
	out += "<label class=\"field\"><span>Rights</span>" +
	       TextInput("rights", "lrswi", "text", "lrswi") + "</label>";
	out += "<p>" + Button("Grant") + "</p>";
	out += FormEnd();
	out += RawHtml(RightsLegend());
	out += "<p class=\"muted\">These are RFC 4314 access rights, the same ones an IMAP client reads with "
	       "<code>GETACL</code> and writes with <code>SETACL</code>. A grant can only widen access: to "
	       "take reading away from everybody, make the room invitation-only above and then invite the "
	       "people who should have it.</p>";

	if (room.qr_flags & quackmail::citadel::QR_PRIVATE) {
		out += "<p class=\"muted\">This room is invitation-only, so it is listed only for people named "
		       "here with the <code>l</code> right — and for aides, who see every room.</p>";
	}
	return out;
}

// Reachability by e-mail is a grant of `p` to "anyone", not a room flag — the
// same thing `SETACL <room> anyone p` does from a mail client. Only meaningful
// for a public room, and only actually honoured when the site has room mail on.
std::string MailSection(Ctx &ctx, const Room &room) {
	if (room.mailbox_owner != 0 ||
	    (room.qr_flags & (quackmail::citadel::QR_PRIVATE | quackmail::citadel::QR_PASSWORDED))) {
		return std::string();
	}
	std::string addr = "room_" + room.display_name;
	std::replace(addr.begin(), addr.end(), ' ', '_');
	addr += "@" + quackmail::citadel::GetConfig(ctx.con, "c_fqdn", "");
	bool open = quackmail::citadel::CanPost(ctx.con, "", room);

	std::string out = "<h2>E-mail</h2>";
	out += FormStart(ctx, SettingsHref(room, "/mail"));
	out += Hidden("open", open ? "0" : "1");
	out += "<p>" +
	       Button(open ? "Stop accepting mail" : "Accept mail from anyone", open ? "danger" : "") + "</p>";
	out += FormEnd();
	out += "<p class=\"muted\">" +
	       (open ? T("Anyone may post here by sending mail to " + addr + ".")
	             : T("Turn this on and anyone may post here by sending mail to " + addr + ".")) +
	       "</p>";
	if (!ConfigBool(ctx.con, "qm_room_mail", true)) {
		out += "<p class=\"muted\">Room addresses are switched off for this whole server "
		       "(<code>qm_room_mail</code>), so this grant will have no effect until an operator turns "
		       "them back on.</p>";
	}
	return out;
}

// ---- the mailing list, if this room is one -------------------------------

std::string ListSection(Ctx &ctx, const Room &room) {
	listserv::List l;
	if (!listserv::GetList(ctx.con, room.room_num, l)) {
		return "<h2>Mailing list</h2><p class=\"muted\">This room is not a mailing list. An aide can make "
		       "it one — that mints an inbound address and starts sending copies to people, which is why "
		       "it is not something a room administrator does alone. Once it exists you can set the tag, "
		       "the footer and the membership here.</p>";
	}
	std::string base = SettingsHref(room);
	std::string out = "<h2>Mailing list</h2>";
	out += "<p class=\"muted\">Posting address <code>" + T(listserv::ListAddress(ctx.con, l)) +
	       "</code>. Everything posted in this room by any route is distributed, because the room is "
	       "what the list watches.</p>";

	out += FormStart(ctx, base + "/list");
	out += "<label class=\"field\"><span>Subject tag</span>" +
	       TextInput("subject_tag", l.subject_tag, "text", "[announce]") + "</label>";
	out += "<label class=\"field\"><span>Footer</span>" + TextArea("footer", l.footer, 4) + "</label>";
	out += "<label class=\"field\"><span>Reply-To</span>" +
	       Select("reply_to", {{"sender", "sender — replies go to the author"},
	                           {"list", "list — replies go to everyone"}},
	              l.reply_to_list ? "list" : "sender") +
	       "</label>";
	out += "<label class=\"field\"><span>Digest interval, seconds</span>" +
	       TextInput("digest_interval", std::to_string(l.digest_interval_secs), "number") + "</label>";
	out += "<label class=\"field\"><span>Messages per digest</span>" +
	       TextInput("digest_max", std::to_string(l.digest_max), "number") + "</label>";
	out += "<p>" + Button("Save list settings") + "</p>";
	out += FormEnd();
	out += "<p class=\"muted\">The footer is added to plain-text messages only — splicing it into a "
	       "multipart message would corrupt the parts. The address, the delivery mode and who may post "
	       "are set by an aide under <a href=\"/admin/lists\">Mailing lists</a>.</p>";

	auto subs = listserv::Subscribers(ctx.con, l.room_num, "");
	out += "<h3>Subscribers</h3>";
	out += "<div class=\"wrap\"><table><tr>" + Head("Address") + Head("Delivery") + Head("State") +
	       Head("Since") + Head("") + "</tr>";
	for (auto &s : subs) {
		std::string state = s.state == listserv::SubState::Active
		                        ? "active"
		                        : (s.state == listserv::SubState::UnsubPending ? "leaving" : "unconfirmed");
		out += "<tr>";
		out += Cell(s.address);
		out += Cell(s.kind == listserv::SubKind::Digest ? "digest" : "post");
		out += Cell(state);
		out += Cell(s.confirmed_at > 0 ? FormatTime(ctx, s.confirmed_at) : "");
		out += "<td>" + FormStart(ctx, base + "/unsubscribe", "inline") + Hidden("address", s.address) +
		       Button("Remove", "danger") + FormEnd() + "</td>";
		out += "</tr>";
	}
	out += "</table></div>";
	if (subs.empty()) {
		out += "<p class=\"muted\">Nobody is subscribed yet.</p>";
	}

	out += "<h3>Invite somebody</h3>";
	out += FormStart(ctx, base + "/subscribe");
	out += "<label class=\"field\"><span>Address</span>" + TextInput("address", "", "email") + "</label>";
	out += "<label class=\"field\"><span>Delivery</span>" +
	       Select("kind", {{"post", "post"}, {"digest", "digest"}}, "post") + "</label>";
	out += "<p>" + Button("Send an invitation") + "</p>";
	out += FormEnd();
	out += "<p class=\"muted\">This e-mails a confirmation and nothing else: the address is not "
	       "subscribed until somebody reading it follows the link. Signing an address up outright is an "
	       "aide's action, on the admin console.</p>";

	auto held = listserv::HeldMessages(ctx.con, l.room_num, "held");
	if (!held.empty()) {
		out += "<h3>Held for moderation</h3>";
		out += "<div class=\"wrap\"><table><tr>" + Head("From") + Head("Subject") + Head("Received") +
		       Head("") + "</tr>";
		for (auto &h : held) {
			out += "<tr>";
			out += Cell(h.mail_from);
			out += Cell(h.subject);
			out += Cell(FormatTime(ctx, h.received_at));
			out += "<td>" + FormStart(ctx, base + "/approve", "inline") +
			       Hidden("id", std::to_string(h.id)) + Button("Approve") + FormEnd() +
			       FormStart(ctx, base + "/reject", "inline") + Hidden("id", std::to_string(h.id)) +
			       Button("Reject", "danger") + FormEnd() + "</td>";
			out += "</tr>";
		}
		out += "</table></div>";
		out += "<p class=\"muted\">Approving posts the message into the room; the spooler distributes it "
		       "on its next pass.</p>";
	}
	return out;
}

// ---- feeds pointed at this room ------------------------------------------

std::string FeedSection(Ctx &ctx, const Room &room) {
	std::vector<quackmail::fetch::Feed> mine;
	for (auto &f : quackmail::fetch::ListFeeds(ctx.con)) {
		if (f.target_user.empty() && f.target_room == room.room_num) {
			mine.push_back(f);
		}
	}
	std::string out = "<h2>Feeds</h2>";
	if (mine.empty()) {
		out += "<p class=\"muted\">Nothing is being pulled into this room. An aide can point a POP3 or "
		       "IMAP mailbox, or an RSS feed, at it from <a href=\"/admin/feeds\">Feeds</a>.</p>";
		return out;
	}
	out += "<div class=\"wrap\"><table><tr>" + Head("Name") + Head("Kind") + Head("Source") +
	       Head("Every") + Head("Last run") + Head("Status") + Head("Pulled") + Head("") + "</tr>";
	for (auto &f : mine) {
		// Never the stored credential, and for a mailbox never the password —
		// only what the source is, which the room's readers can see anyway from
		// the messages that arrive.
		std::string source = f.kind == quackmail::fetch::Kind::Rss
		                         ? f.url
		                         : (f.host + (f.port > 0 ? (":" + std::to_string(f.port)) : ""));
		out += "<tr>";
		out += Cell(f.name + (f.enabled ? "" : " (disabled)"));
		out += Cell(quackmail::fetch::KindName(f.kind));
		out += Cell(source);
		out += Cell(std::to_string(f.interval_secs) + "s");
		out += Cell(f.last_run_at > 0 ? FormatTime(ctx, f.last_run_at) : "never");
		out += Cell(f.last_status + (f.last_error.empty() ? "" : (": " + f.last_error)));
		out += Cell(std::to_string(f.messages_pulled));
		out += "<td>" + FormStart(ctx, SettingsHref(room, "/feedrun"), "inline") +
		       Hidden("name", f.name) + Button("Poll now", "sec") + FormEnd() + "</td>";
		out += "</tr>";
	}
	out += "</table></div>";
	out += "<p class=\"muted\">You can poll a feed an aide has already set up; adding or editing one is "
	       "an operator's action, because a feed stores a password and dials whatever host it names.</p>";
	return out;
}

// ---- the settings page ---------------------------------------------------

void GetRoomSettings(Ctx &ctx) {
	Room room;
	if (!RoomForAdmin(ctx, room)) {
		return;
	}
	std::string base = SettingsHref(room);
	std::string body = "<p>" + Link(RoomHref(room), "Back to " + room.display_name) + "</p>";

	body += "<h2>This room</h2>";
	body += FormStart(ctx, base + "/save");
	body += "<label class=\"field\"><span>Name</span>" + TextInput("display_name", room.display_name) +
	        "</label>";
	body += "<label class=\"field\"><span>Floor</span>" +
	        Select("floor", FloorOptions(ctx), std::to_string(room.floor_num)) + "</label>";
	body += "<label class=\"field\"><span>What this room holds</span>" +
	        Select("view", ViewOptions(), std::to_string(room.default_view)) + "</label>";
	body += "<label class=\"field\"><span>Description</span>" + TextArea("info", room.info, 4) +
	        "</label>";
	body += "<label class=\"field\"><span>List order</span>" +
	        TextInput("listorder", std::to_string(room.listorder), "number") + "</label>";
	body += "<label class=\"field\"><span>Room password</span>" +
	        TextInput("password", room.password, "text") + "</label>";
	for (auto &f : kRoomFlags) {
		body += Checkbox(f.field, (room.qr_flags & f.bit) != 0, f.label) + " ";
	}
	body += "<p>" + Button("Save") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">Changing what a room holds changes how it is drawn, not what is in it: "
	        "the objects are messages either way, and <a href=\"" + A(RoomHref(room) + "?view=raw") +
	        "\">the plain message list</a> always works.</p>";

	body += AclSection(ctx, room);
	body += MailSection(ctx, room);
	body += ListSection(ctx, room);
	body += FeedSection(ctx, room);

	if (room.room_num != quackmail::citadel::kLobbyRoom &&
	    room.room_num != quackmail::citadel::kAideRoom && room.mailbox_owner == 0) {
		body += "<h2>Delete this room</h2>";
		body += FormStart(ctx, base + "/kill");
		body += "<label class=\"field\"><span>Type the room's name to confirm</span>" +
		        TextInput("confirm", "") + "</label>";
		body += "<p>" + Button("Delete " + room.display_name, "danger") + "</p>";
		body += FormEnd();
		body += "<p class=\"muted\">The messages themselves survive if they are also pointed into another "
		        "room; the pointers from this one, its access list and everyone's read state go.</p>";
		listserv::List as_list;
		if (listserv::GetList(ctx.con, room.room_num, as_list)) {
			body += "<p class=\"muted\">Not while this room is the mailing list <code>" +
			        T(listserv::ListAddress(ctx.con, as_list)) +
			        "</code>, though — an aide has to remove the list first, or its address would go on "
			        "accepting mail for a room that is no longer there.</p>";
		}
	}

	PageOpts opts;
	opts.active = "bbs";
	Render(ctx, "Settings: " + room.display_name, body, opts);
}

void PostRoomSave(Ctx &ctx) {
	Room room;
	if (!RoomForAdmin(ctx, room)) {
		return;
	}
	Room updated = room;
	updated.display_name = ctx.req.Form("display_name");
	updated.floor_num = ctx.FormInt("floor", room.floor_num);
	updated.default_view = ctx.FormInt("view", room.default_view);
	updated.listorder = ctx.FormInt("listorder", room.listorder);
	updated.password = ctx.req.Form("password");
	updated.info = ctx.req.Form("info");
	updated.qr_flags = FlagsFromForm(ctx, room);

	std::string err;
	if (!quackmail::citadel::UpdateRoom(ctx.con, updated, err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, SettingsHref(room), "saved");
}

void PostRoomAcl(Ctx &ctx) {
	Room room;
	if (!RoomForAdmin(ctx, room)) {
		return;
	}
	std::string identifier = ctx.req.Form("identifier");
	std::string rights = ctx.req.Form("rights");
	// "anyone" covers unauthenticated callers, so granting it `a` would hand
	// room administration to the whole internet. Nothing legitimate wants that,
	// and it is exactly the entry somebody types by accident while trying to
	// open a room to everyone.
	if (rights.find('a') != std::string::npos &&
	    quackmail::util::Lower(identifier) == "anyone") {
		BadRequest(ctx, "\"anyone\" cannot administer a room — that would include people who are not "
		                "signed in. Name the people who should administer it.");
		return;
	}
	// Dropping your own administer right locks you out of this page, and for a
	// non-aide there is nobody to undo it but an aide. Refusing is cheaper than
	// the support request; an aide can still hand the room to somebody else.
	if (!ctx.IsAide() && quackmail::util::Lower(identifier) == quackmail::util::Lower(ctx.username) &&
	    rights.find('a') == std::string::npos) {
		Forbidden(ctx, "That would take away your own administration of this room, and only an aide "
		               "could give it back. Grant it to somebody else first.");
		return;
	}
	std::string err;
	if (!quackmail::citadel::SetRights(ctx.con, room, identifier, rights, err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, SettingsHref(room), rights.empty() ? "deleted" : "saved");
}

void PostRoomMail(Ctx &ctx) {
	Room room;
	if (!RoomForAdmin(ctx, room)) {
		return;
	}
	if (room.mailbox_owner != 0 ||
	    (room.qr_flags & (quackmail::citadel::QR_PRIVATE | quackmail::citadel::QR_PASSWORDED))) {
		BadRequest(ctx, "Only a public room can be opened to e-mail.");
		return;
	}
	bool open = ctx.req.Form("open") == "1";
	std::string err;
	if (!quackmail::citadel::SetRights(ctx.con, room, "anyone", open ? "lrsp" : "", err)) {
		BadRequest(ctx, err);
		return;
	}
	AideLog(ctx, "Room e-mail " + std::string(open ? "opened" : "closed") + ": " + room.display_name,
	        open ? "The room now accepts mail from anyone (RFC 4314 \"anyone lrsp\")."
	             : "The room no longer accepts mail from anyone.");
	RedirectTo(ctx, SettingsHref(room), "saved");
}

void PostRoomKill(Ctx &ctx) {
	Room room;
	if (!RoomForAdmin(ctx, room)) {
		return;
	}
	if (ctx.req.Form("confirm") != room.display_name) {
		BadRequest(ctx, "The name you typed does not match this room, so nothing was deleted.");
		return;
	}
	// KillRoom removes the room's messages, state and access list — not its list
	// configuration, which lives in tables core/citadel_store.cpp cannot reach
	// without a dependency cycle. Deleting the room out from under a list would
	// leave an inbound address resolving to nothing, so this route refuses the
	// order rather than doing half of it. Unmaking a list is an aide's action
	// for the same reason making one is.
	listserv::List still_a_list;
	if (listserv::GetList(ctx.con, room.room_num, still_a_list)) {
		BadRequest(ctx, "This room is still the mailing list " +
		                    listserv::ListAddress(ctx.con, still_a_list) +
		                    ". An aide has to remove the list first, or its address would go on "
		                    "accepting mail for a room that no longer exists.");
		return;
	}
	std::string err;
	if (!quackmail::citadel::KillRoom(ctx.con, room.room_num, err)) {
		BadRequest(ctx, err);
		return;
	}
	AideLog(ctx, "Room deleted: " + room.display_name,
	        "The room and its message pointers were removed by its administrator.");
	RedirectTo(ctx, "/bbs/", "deleted");
}

// ---- list settings -------------------------------------------------------

// Every field goes through listserv::SetField by name, so this form can cover a
// subset of a list's settings without a partial struct silently resetting the
// ones it does not show.
void PostRoomList(Ctx &ctx) {
	Room room;
	if (!RoomForAdmin(ctx, room)) {
		return;
	}
	listserv::List l;
	if (!listserv::GetList(ctx.con, room.room_num, l)) {
		NotFound(ctx);
		return;
	}
	static const char *const kFields[] = {"subject_tag", "footer", "reply_to", "digest_interval",
	                                      "digest_max"};
	std::string err;
	for (const char *key : kFields) {
		if (!ctx.req.HasForm(key)) {
			continue;
		}
		if (!listserv::SetField(ctx.con, room.room_num, key, ctx.req.Form(key), err)) {
			BadRequest(ctx, err);
			return;
		}
	}
	RedirectTo(ctx, SettingsHref(room), "saved");
}

void PostRoomSubscribe(Ctx &ctx) {
	Room room;
	if (!RoomForAdmin(ctx, room)) {
		return;
	}
	listserv::List l;
	if (!listserv::GetList(ctx.con, room.room_num, l)) {
		NotFound(ctx);
		return;
	}
	std::string address = ctx.req.Form("address");
	auto kind = ctx.req.Form("kind") == "digest" ? listserv::SubKind::Digest : listserv::SubKind::Post;
	std::string token, err;
	// `confirmed = false`: this mails a token to the address claimed and changes
	// nothing until it comes back. An aide can subscribe outright from the admin
	// console; a room administrator cannot sign up an address on somebody's
	// behalf, which is the same rule the public /lists form follows.
	if (!listserv::Subscribe(ctx.con, l, address, kind, false, token, err)) {
		BadRequest(ctx, err);
		return;
	}
	if (!token.empty()) {
		listserv::SendConfirmation(ctx.con, l, address, token, true);
	}
	RedirectTo(ctx, SettingsHref(room), "invited");
}

void PostRoomUnsubscribe(Ctx &ctx) {
	Room room;
	if (!RoomForAdmin(ctx, room)) {
		return;
	}
	listserv::List l;
	if (!listserv::GetList(ctx.con, room.room_num, l)) {
		NotFound(ctx);
		return;
	}
	// Removing somebody needs no confirmation from them: taking an address off a
	// list can only ever mean less mail, and the room's administrator is
	// entitled to decide who is on their list.
	std::string token, err;
	if (!listserv::Unsubscribe(ctx.con, l, ctx.req.Form("address"), true, token, err)) {
		BadRequest(ctx, err);
		return;
	}
	RedirectTo(ctx, SettingsHref(room), "deleted");
}

void PostRoomApprove(Ctx &ctx) {
	Room room;
	if (!RoomForAdmin(ctx, room)) {
		return;
	}
	listserv::List l;
	if (!listserv::GetList(ctx.con, room.room_num, l)) {
		NotFound(ctx);
		return;
	}
	// The held row carries its own room, so an id from another list would
	// otherwise be approved from here — check it belongs to this one.
	bool ours = false;
	for (auto &h : listserv::HeldMessages(ctx.con, l.room_num, "held")) {
		ours = ours || h.id == ctx.FormInt("id", -1);
	}
	std::string err;
	if (!ours || !listserv::Approve(ctx.con, ctx.FormInt("id", -1), err)) {
		BadRequest(ctx, ours ? err : "That message is not held for this list.");
		return;
	}
	RedirectTo(ctx, SettingsHref(room), "approved");
}

void PostRoomReject(Ctx &ctx) {
	Room room;
	if (!RoomForAdmin(ctx, room)) {
		return;
	}
	listserv::List l;
	if (!listserv::GetList(ctx.con, room.room_num, l)) {
		NotFound(ctx);
		return;
	}
	bool ours = false;
	for (auto &h : listserv::HeldMessages(ctx.con, l.room_num, "held")) {
		ours = ours || h.id == ctx.FormInt("id", -1);
	}
	std::string err;
	if (!ours || !listserv::Reject(ctx.con, ctx.FormInt("id", -1), err)) {
		BadRequest(ctx, ours ? err : "That message is not held for this list.");
		return;
	}
	RedirectTo(ctx, SettingsHref(room), "rejected");
}

void PostRoomFeedRun(Ctx &ctx) {
	Room room;
	if (!RoomForAdmin(ctx, room)) {
		return;
	}
	quackmail::fetch::Feed f;
	// The feed must already point at this room. Without that check this route
	// would run *any* configured feed, which is a way for a room administrator
	// to make the server dial a host chosen for somebody else's room.
	if (!quackmail::fetch::GetFeed(ctx.con, ctx.req.Form("name"), f) || !f.target_user.empty() ||
	    f.target_room != room.room_num) {
		NotFound(ctx);
		return;
	}
	quackmail::fetch::RunResult res;
	quackmail::fetch::RunFeed(ctx.con, f, res);
	RedirectTo(ctx, SettingsHref(room), res.status == "error" ? "feed_failed" : "fetched");
}

// ---- self-serve room creation --------------------------------------------

void GetNewRoom(Ctx &ctx) {
	if (!MayCreateRooms(ctx)) {
		Forbidden(ctx, "Creating rooms on this server is reserved. An operator sets the access level it "
		               "needs with qm_room_create_axlevel.");
		return;
	}
	std::string body = FormStart(ctx, "/bbs/new");
	body += "<label class=\"field\"><span>Name</span>" + TextInput("display_name", "") + "</label>";
	body += "<label class=\"field\"><span>Floor</span>" + Select("floor", FloorOptions(ctx), "0") +
	        "</label>";
	body += "<label class=\"field\"><span>What it holds</span>" + Select("view", ViewOptions(), "0") +
	        "</label>";
	body += "<label class=\"field\"><span>Description</span>" + TextArea("info", "", 3) + "</label>";
	body += "<label class=\"field\"><span>Password (only if you tick "
	        "&quot;password protected&quot;)</span>" +
	        TextInput("password", "", "text") + "</label>";
	for (auto &f : kRoomFlags) {
		body += Checkbox(f.field, false, f.label) + " ";
	}
	body += "<p>" + Button("Create") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">You will administer the room you create: its name, who may read and post, "
	        "and whether it is listed at all. An invitation-only room is shown only to the people you "
	        "grant access to — and to aides, who see every room on the server.</p>";

	PageOpts opts;
	opts.active = "newroom";
	Render(ctx, "Create a room", body, opts);
}

void PostNewRoom(Ctx &ctx) {
	if (!MayCreateRooms(ctx)) {
		Forbidden(ctx, "Creating rooms on this server is reserved.");
		return;
	}
	std::string name = ctx.req.Form("display_name");
	if (name.empty()) {
		BadRequest(ctx, "A room needs a name.");
		return;
	}
	Room existing;
	if (quackmail::citadel::ResolveRoom(ctx.con, ctx.username, name, existing)) {
		BadRequest(ctx, "There is already a room with that name.");
		return;
	}

	Room blank; // qr_flags 0: nothing to carry over on a room that does not exist yet
	int64_t flags = FlagsFromForm(ctx, blank);
	std::string password = ctx.req.Form("password");
	if (!(flags & quackmail::citadel::QR_PASSWORDED)) {
		password.clear();
	} else if (password.empty()) {
		BadRequest(ctx, "A password-protected room needs a password.");
		return;
	}

	std::string err;
	// CreateRoom refuses a name shaped like a personal room's internal key
	// ("0000000002.Mail"), which is the one name that would collide with the
	// mailbox keyspace rather than merely with another room.
	int64_t room_num = quackmail::citadel::CreateRoom(ctx.con, name, ctx.FormInt("floor", 0), flags,
	                                                  password, 0, err);
	if (room_num < 0) {
		BadRequest(ctx, err);
		return;
	}

	Room room;
	if (!quackmail::citadel::GetRoomByNum(ctx.con, room_num, room)) {
		ErrorPage(ctx, 500, "Could not create the room", "The room was created but cannot be read back.");
		return;
	}
	room.default_view = ctx.FormInt("view", 0);
	room.info = ctx.req.Form("info");
	quackmail::citadel::UpdateRoom(ctx.con, room, err);

	// The creator's grant is what makes them the administrator. Without it a
	// non-aide would have derived rights only — and for an invitation-only room
	// that is nothing at all, so they could not even see what they just made.
	if (!quackmail::citadel::SetRights(ctx.con, room, ctx.username, quackmail::citadel::kAclRights, err)) {
		ErrorPage(ctx, 500, "Could not create the room",
		          "The room was created but you could not be made its administrator: " + err);
		return;
	}
	AideLog(ctx, "Room created: " + room.display_name,
	        "A room was created from the web interface.\n\nRoom: " + room.display_name +
	            "\nAdministrator: " + ctx.username);
	RedirectTo(ctx, SettingsHref(room), "room_created");
}

} // namespace

bool MayCreateRooms(const Ctx &ctx) {
	if (!ctx.Authed()) {
		return false;
	}
	// Defaults to the aide level, so nothing changes on an existing server until
	// an operator lowers it. A value that is not a number is treated as the
	// default rather than as 0 — a typo must not open the door.
	std::string want = ConfigStr(ctx.con, "qm_room_create_axlevel", "");
	int64_t level = quackmail::citadel::kAideAxLevel;
	if (!want.empty()) {
		char *end = nullptr;
		long parsed = std::strtol(want.c_str(), &end, 10);
		if (end != want.c_str() && *end == '\0' && parsed >= 0 && parsed <= 6) {
			level = (int64_t)parsed;
		}
	}
	return ctx.axlevel >= level;
}

void RegisterRoomAdminRoutes(std::vector<Route> &out) {
	out.push_back({"GET", "/bbs/new", Role::User, GetNewRoom});
	out.push_back({"POST", "/bbs/new", Role::User, PostNewRoom});
	out.push_back({"GET", "/bbs/room/:n/settings", Role::User, GetRoomSettings});
	out.push_back({"POST", "/bbs/room/:n/settings/save", Role::User, PostRoomSave});
	out.push_back({"POST", "/bbs/room/:n/settings/acl", Role::User, PostRoomAcl});
	out.push_back({"POST", "/bbs/room/:n/settings/mail", Role::User, PostRoomMail});
	out.push_back({"POST", "/bbs/room/:n/settings/kill", Role::User, PostRoomKill});
	out.push_back({"POST", "/bbs/room/:n/settings/list", Role::User, PostRoomList});
	out.push_back({"POST", "/bbs/room/:n/settings/subscribe", Role::User, PostRoomSubscribe});
	out.push_back({"POST", "/bbs/room/:n/settings/unsubscribe", Role::User, PostRoomUnsubscribe});
	out.push_back({"POST", "/bbs/room/:n/settings/approve", Role::User, PostRoomApprove});
	out.push_back({"POST", "/bbs/room/:n/settings/reject", Role::User, PostRoomReject});
	out.push_back({"POST", "/bbs/room/:n/settings/feedrun", Role::User, PostRoomFeedRun});
}

} // namespace qmweb
} // namespace duckdb
