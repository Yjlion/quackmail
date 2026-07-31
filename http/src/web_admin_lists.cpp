#include "web.hpp"

#include "quackmail/fetch.hpp"
#include "quackmail/listserv.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/util.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <cstdlib>

namespace duckdb {
namespace qmweb {

namespace listserv = quackmail::listserv;

namespace {

std::string ModeValue(listserv::Mode m) {
	switch (m) {
	case listserv::Mode::Digest:
		return "digest";
	case listserv::Mode::Both:
		return "both";
	default:
		return "post";
	}
}

std::string PolicyValue(listserv::PostPolicy p) {
	switch (p) {
	case listserv::PostPolicy::Anyone:
		return "anyone";
	case listserv::PostPolicy::Moderated:
		return "moderated";
	default:
		return "subscribers";
	}
}

std::string StateValue(listserv::SubState s) {
	switch (s) {
	case listserv::SubState::Active:
		return "active";
	case listserv::SubState::UnsubPending:
		return "leaving";
	default:
		return "unconfirmed";
	}
}

// The list named by the `<room_num>` capture, or nothing (having already
// rendered a 404).
bool ListFromPath(Ctx &ctx, listserv::List &out) {
	int64_t room_num = (int64_t)std::strtoll(ctx.Cap(0).c_str(), nullptr, 10);
	if (ctx.Cap(0).empty() || !listserv::GetList(ctx.con, room_num, out)) {
		NotFound(ctx);
		return false;
	}
	return true;
}

// ---- the list index ------------------------------------------------------

void GetLists(Ctx &ctx) {
	auto lists = listserv::ListLists(ctx.con);

	std::string body = "<div class=\"wrap\"><table><tr>" + Head("Room") + Head("Address") + Head("Mode") +
	                   Head("Who may post") + Head("Subscribers") + Head("Held") + Head("Enabled") +
	                   Head("") + "</tr>";
	for (auto &l : lists) {
		int64_t active = 0;
		for (auto &s : listserv::Subscribers(ctx.con, l.room_num, "active")) {
			(void)s;
			active++;
		}
		int64_t held = (int64_t)listserv::HeldMessages(ctx.con, l.room_num, "held").size();
		body += "<tr>";
		body += Cell(l.display_name);
		body += Cell(listserv::ListAddress(ctx.con, l));
		body += Cell(ModeValue(l.mode));
		body += Cell(PolicyValue(l.post_policy));
		body += Cell(std::to_string(active));
		body += Cell(held > 0 ? std::to_string(held) : "");
		body += Cell(l.enabled ? "yes" : "no");
		body += "<td>" + Link("/admin/lists/" + std::to_string(l.room_num), "Manage", "btn sec") + "</td>";
		body += "</tr>";
	}
	body += "</table></div>";
	if (lists.empty()) {
		body += "<p class=\"muted\">No mailing lists yet.</p>";
	}

	body += "<h2>Make a room into a list</h2>";
	body += FormStart(ctx, "/admin/lists/create");
	body += "<label class=\"field\"><span>Room</span>" + TextInput("room", "") + "</label>";
	body += "<label class=\"field\"><span>Address (blank for room_&lt;name&gt;)</span>" +
	        TextInput("address", "", "text", "announce") + "</label>";
	body += "<p>" + Button("Create") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">Mail to the list address is archived in the room and fanned out to every "
	        "subscriber. Anything posted in the room by any other route — the BBS, NNTP, a Citadel "
	        "client — is distributed too, because the room is what the list watches.</p>";
	body += "<p class=\"muted\">Distribution runs on the <code>qm_listserv</code> spooler. If it is not "
	        "started, messages are archived but nothing is sent.</p>";

	body += "<h2>Run the spooler now</h2>";
	body += FormStart(ctx, "/admin/lists/run");
	body += "<p>" + Button("Distribute pending messages") + "</p>";
	body += FormEnd();

	AdminPage(ctx, "Mailing lists", body);
}

void PostListCreate(Ctx &ctx) {
	quackmail::citadel::Room room;
	if (!quackmail::citadel::ResolveRoom(ctx.con, "", ctx.req.Form("room"), room)) {
		BadRequest(ctx, "no such public room");
		return;
	}
	listserv::List l;
	l.room_num = room.room_num;
	l.address = ctx.req.Form("address");
	std::string err;
	if (!listserv::SetList(ctx.con, l, err)) {
		BadRequest(ctx, err);
		return;
	}
	listserv::GetList(ctx.con, room.room_num, l);
	AideLog(ctx, "Mailing list created", room.display_name + " is now the mailing list " +
	                                         listserv::ListAddress(ctx.con, l) + ".");
	RedirectTo(ctx, "/admin/lists/" + std::to_string(room.room_num), "created");
}

void PostListRun(Ctx &ctx) {
	listserv::SpoolResult res;
	std::string err;
	listserv::SpoolOnce(ctx.con, res, err);
	RedirectTo(ctx, "/admin/lists", "distributed");
}

// ---- one list ------------------------------------------------------------

void GetList(Ctx &ctx) {
	listserv::List l;
	if (!ListFromPath(ctx, l)) {
		return;
	}
	std::string base = "/admin/lists/" + std::to_string(l.room_num);
	std::string addr = listserv::ListAddress(ctx.con, l);

	std::string body = "<p class=\"muted\">Posting address <code>" + T(addr) +
	                   "</code>. Subscribe and unsubscribe addresses are <code>" + T(l.address) +
	                   "-subscribe@</code> and <code>" + T(l.address) + "-unsubscribe@</code>.</p>";

	// ---- settings ----
	body += "<h2>Settings</h2>";
	body += FormStart(ctx, base + "/save");
	body += "<label class=\"field\"><span>Address</span>" + TextInput("address", l.address) + "</label>";
	body += "<label class=\"field\"><span>Delivery</span>" +
	        Select("mode",
	               {{"post", "post — send each message as it arrives"},
	                {"digest", "digest — send a periodic batch"},
	                {"both", "both — each subscriber's choice"}},
	               ModeValue(l.mode)) +
	        "</label>";
	body += "<label class=\"field\"><span>Who may post</span>" +
	        Select("post_policy",
	               {{"subscribers", "subscribers — anyone else is held"},
	                {"anyone", "anyone"},
	                {"moderated", "moderated — everything is held"}},
	               PolicyValue(l.post_policy)) +
	        "</label>";
	body += "<label class=\"field\"><span>Reply-To</span>" +
	        Select("reply_to", {{"sender", "sender — replies go to the author"},
	                            {"list", "list — replies go to everyone"}},
	               l.reply_to_list ? "list" : "sender") +
	        "</label>";
	body += "<label class=\"field\"><span>Subject tag</span>" +
	        TextInput("subject_tag", l.subject_tag, "text", "[announce]") + "</label>";
	body += "<label class=\"field\"><span>Footer</span>" + TextArea("footer", l.footer, 4) + "</label>";
	body += "<label class=\"field\"><span>Digest interval, seconds</span>" +
	        TextInput("digest_interval", std::to_string(l.digest_interval_secs), "number") + "</label>";
	body += "<label class=\"field\"><span>Messages per digest</span>" +
	        TextInput("digest_max", std::to_string(l.digest_max), "number") + "</label>";
	body += "<p>" + Checkbox("enabled", l.enabled, "Enabled") + "</p>";
	body += "<p>" + Button("Save") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">The footer is added to plain-text messages only — splicing it into a "
	        "multipart message would corrupt the parts.</p>";

	// ---- subscribers ----
	auto subs = listserv::Subscribers(ctx.con, l.room_num, "");
	body += "<h2>Subscribers</h2>";
	body += "<div class=\"wrap\"><table><tr>" + Head("Address") + Head("Delivery") + Head("State") +
	        Head("Since") + Head("") + "</tr>";
	for (auto &s : subs) {
		body += "<tr>";
		body += Cell(s.address);
		body += Cell(s.kind == listserv::SubKind::Digest ? "digest" : "post");
		body += Cell(StateValue(s.state));
		body += Cell(s.confirmed_at > 0 ? FormatTime(ctx, s.confirmed_at) : "");
		body += "<td>" + FormStart(ctx, base + "/unsubscribe", "inline") + Hidden("address", s.address) +
		        Button("Remove", "danger") + FormEnd() + "</td>";
		body += "</tr>";
	}
	body += "</table></div>";
	if (subs.empty()) {
		body += "<p class=\"muted\">Nobody is subscribed yet.</p>";
	}

	body += "<h3>Add a subscriber</h3>";
	body += FormStart(ctx, base + "/subscribe");
	body += "<label class=\"field\"><span>Address</span>" + TextInput("address", "", "email") + "</label>";
	body += "<label class=\"field\"><span>Delivery</span>" +
	        Select("kind", {{"post", "post"}, {"digest", "digest"}}, "post") + "</label>";
	body += "<p>" + Button("Subscribe") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">Adding somebody here subscribes them outright. Anyone who mails "
	        "<code>" +
	        T(l.address) + "-subscribe@</code> has to answer a confirmation first, so that nobody can "
	                       "sign up an address they do not read.</p>";

	// ---- moderation queue ----
	auto held = listserv::HeldMessages(ctx.con, l.room_num, "held");
	if (!held.empty()) {
		body += "<h2>Held for moderation</h2>";
		body += "<div class=\"wrap\"><table><tr>" + Head("From") + Head("Subject") + Head("Received") +
		        Head("") + "</tr>";
		for (auto &h : held) {
			body += "<tr>";
			body += Cell(h.mail_from);
			body += Cell(h.subject);
			body += Cell(FormatTime(ctx, h.received_at));
			body += "<td>" + FormStart(ctx, base + "/approve", "inline") +
			        Hidden("id", std::to_string(h.id)) + Button("Approve") + FormEnd() +
			        FormStart(ctx, base + "/reject", "inline") + Hidden("id", std::to_string(h.id)) +
			        Button("Reject", "danger") + FormEnd() + "</td>";
			body += "</tr>";
		}
		body += "</table></div>";
		body += "<p class=\"muted\">Approving posts the message into the room; the spooler distributes "
		        "it on its next pass.</p>";
	}

	// ---- removal ----
	body += "<h2>Stop being a list</h2>";
	body += FormStart(ctx, base + "/remove");
	body += "<p class=\"muted\">The room and its messages stay; only the list configuration and its "
	        "subscribers are removed.</p>";
	body += "<p>" + Button("Remove the list", "danger") + "</p>";
	body += FormEnd();

	AdminPage(ctx, "Mailing list: " + l.display_name, body);
}

void PostListSave(Ctx &ctx) {
	listserv::List l;
	if (!ListFromPath(ctx, l)) {
		return;
	}
	l.address = ctx.req.Form("address");
	l.enabled = ctx.req.HasForm("enabled");
	l.mode = ctx.req.Form("mode") == "digest"
	             ? listserv::Mode::Digest
	             : (ctx.req.Form("mode") == "both" ? listserv::Mode::Both : listserv::Mode::Post);
	l.post_policy = ctx.req.Form("post_policy") == "anyone"
	                    ? listserv::PostPolicy::Anyone
	                    : (ctx.req.Form("post_policy") == "moderated" ? listserv::PostPolicy::Moderated
	                                                                 : listserv::PostPolicy::Subscribers);
	l.reply_to_list = ctx.req.Form("reply_to") == "list";
	l.subject_tag = ctx.req.Form("subject_tag");
	l.footer = ctx.req.Form("footer");
	l.digest_interval_secs = ctx.FormInt("digest_interval", 86400);
	l.digest_max = ctx.FormInt("digest_max", 50);

	std::string err;
	if (!listserv::SetList(ctx.con, l, err)) {
		BadRequest(ctx, err);
		return;
	}
	AideLog(ctx, "Mailing list changed", "Settings for the " + l.address + " list were updated.");
	RedirectTo(ctx, "/admin/lists/" + std::to_string(l.room_num), "saved");
}

void PostListRemove(Ctx &ctx) {
	listserv::List l;
	if (!ListFromPath(ctx, l)) {
		return;
	}
	std::string err;
	if (!listserv::RemoveList(ctx.con, l.room_num, err)) {
		BadRequest(ctx, err);
		return;
	}
	AideLog(ctx, "Mailing list removed", l.display_name + " is no longer a mailing list.");
	RedirectTo(ctx, "/admin/lists", "deleted");
}

void PostListSubscribe(Ctx &ctx) {
	listserv::List l;
	if (!ListFromPath(ctx, l)) {
		return;
	}
	auto kind = ctx.req.Form("kind") == "digest" ? listserv::SubKind::Digest : listserv::SubKind::Post;
	std::string token, err;
	if (!listserv::Subscribe(ctx.con, l, ctx.req.Form("address"), kind, true, token, err)) {
		BadRequest(ctx, err);
		return;
	}
	AideLog(ctx, "List subscriber added",
	        ctx.req.Form("address") + " was subscribed to " + l.address + ".");
	RedirectTo(ctx, "/admin/lists/" + std::to_string(l.room_num), "created");
}

void PostListUnsubscribe(Ctx &ctx) {
	listserv::List l;
	if (!ListFromPath(ctx, l)) {
		return;
	}
	std::string token, err;
	if (!listserv::Unsubscribe(ctx.con, l, ctx.req.Form("address"), true, token, err)) {
		BadRequest(ctx, err);
		return;
	}
	AideLog(ctx, "List subscriber removed",
	        ctx.req.Form("address") + " was unsubscribed from " + l.address + ".");
	RedirectTo(ctx, "/admin/lists/" + std::to_string(l.room_num), "deleted");
}

void PostListApprove(Ctx &ctx) {
	listserv::List l;
	if (!ListFromPath(ctx, l)) {
		return;
	}
	std::string err;
	if (!listserv::Approve(ctx.con, ctx.FormInt("id", -1), err)) {
		BadRequest(ctx, err);
		return;
	}
	AideLog(ctx, "List message approved", "A held message for " + l.address + " was approved.");
	RedirectTo(ctx, "/admin/lists/" + std::to_string(l.room_num), "approved");
}

void PostListReject(Ctx &ctx) {
	listserv::List l;
	if (!ListFromPath(ctx, l)) {
		return;
	}
	std::string err;
	if (!listserv::Reject(ctx.con, ctx.FormInt("id", -1), err)) {
		BadRequest(ctx, err);
		return;
	}
	AideLog(ctx, "List message rejected", "A held message for " + l.address + " was rejected.");
	RedirectTo(ctx, "/admin/lists/" + std::to_string(l.room_num), "rejected");
}

// ---- public self-service -------------------------------------------------
//
// Anonymous by design: the point is that someone who is not a user of this BBS
// can join a list. Which is exactly why nothing here takes effect on its own —
// a subscribe request only ever mails a confirmation to the address given, and
// the token in that mail is the sole authority for the change. Without that,
// this form would be a way to sign anybody up for anything.

void GetPublicLists(Ctx &ctx) {
	std::string body = "<div class=\"wrap\"><table><tr>" + Head("List") + Head("Address") + "</tr>";
	int64_t shown = 0;
	for (auto &l : listserv::ListLists(ctx.con, true)) {
		body += "<tr>" + Cell(l.display_name) + Cell(listserv::ListAddress(ctx.con, l)) + "</tr>";
		shown++;
	}
	body += "</table></div>";
	if (shown == 0) {
		body += "<p class=\"muted\">There are no public mailing lists on this server.</p>";
	}

	body += "<h2>Subscribe or unsubscribe</h2>";
	body += FormStart(ctx, "/lists/request");
	body += "<label class=\"field\"><span>List address</span>" +
	        TextInput("list", "", "text", "announce") + "</label>";
	body += "<label class=\"field\"><span>Your e-mail address</span>" +
	        TextInput("address", "", "email") + "</label>";
	body += "<label class=\"field\"><span>Action</span>" +
	        Select("action", {{"subscribe", "subscribe"}, {"unsubscribe", "unsubscribe"}}, "subscribe") +
	        "</label>";
	body += "<p>" + Button("Send me a confirmation") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">We will e-mail you a confirmation link. Nothing changes until you follow "
	        "it, so nobody can subscribe or unsubscribe an address they do not read.</p>";

	Render(ctx, "Mailing lists", body);
}

void PostPublicRequest(Ctx &ctx) {
	std::string want = ctx.req.Form("list");
	std::string address = ctx.req.Form("address");
	bool subscribing = ctx.req.Form("action") != "unsubscribe";

	listserv::List l;
	listserv::Command cmd;
	bool found = listserv::ResolveAddress(ctx.con, quackmail::util::LocalPart(want), l, cmd) &&
	             cmd.kind == listserv::Command::Post;

	// Everything below reports the same outcome whether or not the list exists,
	// the address is already subscribed, or the address is not subscribed at
	// all. Saying which would turn this form into a way to enumerate a list's
	// membership from outside.
	std::string token, err;
	if (found && address.find('@') != std::string::npos) {
		bool ok = subscribing ? listserv::Subscribe(ctx.con, l, address, listserv::SubKind::Post, false,
		                                            token, err)
		                      : listserv::Unsubscribe(ctx.con, l, address, false, token, err);
		if (ok && !token.empty()) {
			listserv::SendConfirmation(ctx.con, l, address, token, subscribing);
		}
	}
	RedirectTo(ctx, "/lists", "confirm_sent");
}

void GetPublicConfirm(Ctx &ctx) {
	std::string token = ctx.req.Param("token");
	std::string what, err;
	std::string body;
	if (listserv::Confirm(ctx.con, token, what, err)) {
		body = "<p>" + T(what) + ".</p>";
	} else {
		body = "<p>" + T(err) + "</p>";
	}
	body += "<p>" + Link("/lists", "Back to the mailing lists", "btn sec") + "</p>";
	Render(ctx, "Mailing list confirmation", body);
}

// ---- remote message pulls ------------------------------------------------

std::string KindLabel(quackmail::fetch::Kind k) {
	return quackmail::fetch::KindName(k);
}

void GetFeeds(Ctx &ctx) {
	auto feeds = quackmail::fetch::ListFeeds(ctx.con);

	std::string body = "<div class=\"wrap\"><table><tr>" + Head("Name") + Head("Kind") + Head("Source") +
	                   Head("Target") + Head("Every") + Head("Last run") + Head("Status") +
	                   Head("Pulled") + Head("") + "</tr>";
	for (auto &f : feeds) {
		std::string source = f.kind == quackmail::fetch::Kind::Rss
		                         ? f.url
		                         : (f.username + "@" + f.host +
		                            (f.port > 0 ? (":" + std::to_string(f.port)) : ""));
		std::string target = f.target_user.empty() ? "" : ("user " + f.target_user);
		if (target.empty()) {
			quackmail::citadel::Room room;
			target = quackmail::citadel::GetRoomByNum(ctx.con, f.target_room, room)
			             ? room.display_name
			             : ("room " + std::to_string(f.target_room));
		}
		body += "<tr>";
		body += Cell(f.name + (f.enabled ? "" : " (disabled)"));
		body += Cell(KindLabel(f.kind));
		body += Cell(source);
		body += Cell(target);
		body += Cell(std::to_string(f.interval_secs) + "s");
		body += Cell(f.last_run_at > 0 ? FormatTime(ctx, f.last_run_at) : "never");
		body += Cell(f.last_status + (f.last_error.empty() ? "" : (": " + f.last_error)));
		body += Cell(std::to_string(f.messages_pulled));
		body += "<td>" + FormStart(ctx, "/admin/feeds/run", "inline") + Hidden("name", f.name) +
		        Button("Run") + FormEnd() + FormStart(ctx, "/admin/feeds/test", "inline") +
		        Hidden("name", f.name) + Button("Test", "sec") + FormEnd() +
		        FormStart(ctx, "/admin/feeds/remove", "inline") + Hidden("name", f.name) +
		        Button("Remove", "danger") + FormEnd() + "</td>";
		body += "</tr>";
	}
	body += "</table></div>";
	if (feeds.empty()) {
		body += "<p class=\"muted\">No feeds configured.</p>";
	}

	body += "<h2>Add a feed</h2>";
	body += FormStart(ctx, "/admin/feeds/add");
	body += "<label class=\"field\"><span>Name</span>" +
	        TextInput("name", "", "text", "letters, digits, - _ .") + "</label>";
	body += "<label class=\"field\"><span>Kind</span>" +
	        Select("kind", {{"rss", "rss — an RSS or Atom feed"},
	                        {"pop3", "pop3 — a remote POP3 mailbox"},
	                        {"imap", "imap — a remote IMAP mailbox"}},
	               "rss") +
	        "</label>";
	body += "<label class=\"field\"><span>URL (rss)</span>" +
	        TextInput("url", "", "text", "https://example.com/feed.xml") + "</label>";
	body += "<label class=\"field\"><span>Host (pop3/imap)</span>" + TextInput("host", "") + "</label>";
	body += "<label class=\"field\"><span>Port (0 = the default for the transport)</span>" +
	        TextInput("port", "0", "number") + "</label>";
	body += "<label class=\"field\"><span>Transport</span>" +
	        Select("tls", {{"starttls", "STARTTLS"}, {"implicit", "implicit TLS (995/993)"},
	                       {"none", "plaintext"}},
	               "starttls") +
	        "</label>";
	body += "<label class=\"field\"><span>Username</span>" + TextInput("username", "") + "</label>";
	body += "<label class=\"field\"><span>Password</span>" +
	        TextInput("password", "", "password") + "</label>";
	body += "<label class=\"field\"><span>Mailbox (imap)</span>" +
	        TextInput("mailbox", "INBOX") + "</label>";
	body += "<label class=\"field\"><span>Post into room</span>" + TextInput("room", "") + "</label>";
	body += "<label class=\"field\"><span>…or deliver to user (their filters run)</span>" +
	        TextInput("user", "") + "</label>";
	body += "<label class=\"field\"><span>Poll every, seconds</span>" +
	        TextInput("interval", "900", "number") + "</label>";
	body += "<p>" + Checkbox("leave_on_server", true, "Leave messages on the server") + "</p>";
	body += "<p>" + Button("Add") + "</p>";
	body += FormEnd();
	body += "<p class=\"muted\">Credentials are stored in the database in the clear, exactly as DKIM "
	        "private keys are — the database file's permissions are the boundary. A stored password is "
	        "never rendered back into this page.</p>";
	body += "<p class=\"muted\">Polling happens on the <code>qm_fetch</code> spooler. If it is not "
	        "started, nothing is pulled until you press Run.</p>";

	AdminPage(ctx, "Feeds", body);
}

void PostFeedAdd(Ctx &ctx) {
	quackmail::fetch::Feed f;
	f.name = ctx.req.Form("name");
	f.kind = quackmail::fetch::ParseKind(ctx.req.Form("kind"));
	f.url = ctx.req.Form("url");
	f.host = ctx.req.Form("host");
	f.port = ctx.FormInt("port", 0);
	f.tls = quackmail::fetch::ParseTls(ctx.req.Form("tls"));
	f.username = ctx.req.Form("username");
	f.password = ctx.req.Form("password");
	f.mailbox = ctx.req.Form("mailbox");
	f.interval_secs = ctx.FormInt("interval", 900);
	f.leave_on_server = ctx.req.HasForm("leave_on_server");
	f.author_override = ctx.req.Form("author");
	f.subject_prefix = ctx.req.Form("subject_prefix");

	std::string user = ctx.req.Form("user");
	if (!user.empty()) {
		f.target_user = user;
	} else {
		quackmail::citadel::Room room;
		if (!quackmail::citadel::ResolveRoom(ctx.con, "", ctx.req.Form("room"), room)) {
			BadRequest(ctx, "no such public room");
			return;
		}
		f.target_room = room.room_num;
	}

	std::string err;
	if (!quackmail::fetch::SetFeed(ctx.con, f, err)) {
		BadRequest(ctx, err);
		return;
	}
	// The credential is deliberately not in the log line.
	AideLog(ctx, "Feed added",
	        "Feed '" + f.name + "' (" + KindLabel(f.kind) + ") now pulls into this server.");
	RedirectTo(ctx, "/admin/feeds", "created");
}

void PostFeedRemove(Ctx &ctx) {
	std::string err;
	std::string name = ctx.req.Form("name");
	if (!quackmail::fetch::RemoveFeed(ctx.con, name, err)) {
		BadRequest(ctx, err);
		return;
	}
	AideLog(ctx, "Feed removed", "Feed '" + name + "' was removed.");
	RedirectTo(ctx, "/admin/feeds", "deleted");
}

void PostFeedRun(Ctx &ctx) {
	quackmail::fetch::Feed f;
	if (!quackmail::fetch::GetFeed(ctx.con, ctx.req.Form("name"), f)) {
		BadRequest(ctx, "no such feed");
		return;
	}
	quackmail::fetch::RunResult res;
	quackmail::fetch::RunFeed(ctx.con, f, res);
	RedirectTo(ctx, "/admin/feeds", res.status == "error" ? "feed_failed" : "fetched");
}

void PostFeedTest(Ctx &ctx) {
	std::string info, err;
	bool ok = quackmail::fetch::TestFeed(ctx.con, ctx.req.Form("name"), info, err);
	RedirectTo(ctx, "/admin/feeds", ok ? "feed_ok" : "feed_failed");
}

} // namespace

void RegisterAdminListRoutes(std::vector<Route> &out) {
	out.push_back({"GET", "/admin/lists", Role::Aide, GetLists});
	out.push_back({"POST", "/admin/lists/create", Role::Aide, PostListCreate});
	out.push_back({"POST", "/admin/lists/run", Role::Aide, PostListRun});
	out.push_back({"GET", "/admin/lists/:n", Role::Aide, GetList});
	out.push_back({"POST", "/admin/lists/:n/save", Role::Aide, PostListSave});
	out.push_back({"POST", "/admin/lists/:n/remove", Role::Aide, PostListRemove});
	out.push_back({"POST", "/admin/lists/:n/subscribe", Role::Aide, PostListSubscribe});
	out.push_back({"POST", "/admin/lists/:n/unsubscribe", Role::Aide, PostListUnsubscribe});
	out.push_back({"POST", "/admin/lists/:n/approve", Role::Aide, PostListApprove});
	out.push_back({"POST", "/admin/lists/:n/reject", Role::Aide, PostListReject});

	out.push_back({"GET", "/admin/feeds", Role::Aide, GetFeeds});
	out.push_back({"POST", "/admin/feeds/add", Role::Aide, PostFeedAdd});
	out.push_back({"POST", "/admin/feeds/remove", Role::Aide, PostFeedRemove});
	out.push_back({"POST", "/admin/feeds/run", Role::Aide, PostFeedRun});
	out.push_back({"POST", "/admin/feeds/test", Role::Aide, PostFeedTest});

	// Self-service, open to anyone: see the note above PostPublicRequest.
	out.push_back({"GET", "/lists", Role::Anon, GetPublicLists});
	out.push_back({"POST", "/lists/request", Role::Anon, PostPublicRequest});
	out.push_back({"GET", "/lists/confirm", Role::Anon, GetPublicConfirm});
}

} // namespace qmweb
} // namespace duckdb
