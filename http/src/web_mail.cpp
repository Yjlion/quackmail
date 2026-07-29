#include "web.hpp"

#include "quackmail/citadel_msg.hpp"
#include "quackmail/delivery.hpp"
#include "quackmail/dkim.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mailpolicy.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Message;
using quackmail::citadel::Room;

namespace {

// The personal rooms a mail user thinks of as folders, in the order Citadel
// provisions them (EnsureUserRooms).
const char *kFolders[] = {"Mail", "Sent Items", "Drafts", "Trash"};

int64_t CapNum(const Ctx &ctx, size_t i) {
	std::string s = ctx.Cap(i);
	return s.empty() ? -1 : (int64_t)std::strtoll(s.c_str(), nullptr, 10);
}

using quackmail::util::RfcDate;

// Fold a header value that must not contain CR/LF. Anything a user typed goes
// through this before it becomes a header: an embedded newline would otherwise
// inject headers of the sender's choosing (Bcc, extra recipients).
std::string HeaderSafe(const std::string &in) {
	std::string out;
	out.reserve(in.size());
	for (char c : in) {
		if (c == '\r' || c == '\n') {
			out += ' ';
		} else {
			out += c;
		}
	}
	while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) {
		out.pop_back();
	}
	return out;
}

std::string SenderAddress(Ctx &ctx) {
	return ctx.username + "@" + ConfigStr(ctx.con, "c_fqdn", "quackmail.test");
}

std::string Base64Wrapped(const std::string &raw) {
	std::string b64 = quackmail::util::Base64Encode(raw);
	std::string out;
	for (size_t i = 0; i < b64.size(); i += 76) {
		out += b64.substr(i, 76);
		out += "\r\n";
	}
	return out;
}

std::string CrlfBody(const std::string &in) {
	std::string out;
	out.reserve(in.size() + in.size() / 40);
	for (size_t i = 0; i < in.size(); i++) {
		if (in[i] == '\r') {
			continue;
		}
		if (in[i] == '\n') {
			out += "\r\n";
		} else {
			out += in[i];
		}
	}
	return out;
}

// Build the RFC822 message that gets signed, delivered and filed. Attachments
// make it multipart/mixed; without them it stays a plain text/plain message,
// which is what a Citadel or POP3 reader will see on the other side.
std::string BuildMessage(Ctx &ctx, const std::string &from, const std::string &to, const std::string &cc,
                         const std::string &subject, const std::string &body,
                         const std::vector<http::FormFile> &files, const std::string &in_reply_to,
                         const std::string &references, std::string &message_id) {
	std::string fqdn = ConfigStr(ctx.con, "c_fqdn", "quackmail.test");
	int64_t now = (int64_t)std::time(nullptr);
	message_id = "<" + quackmail::util::RandomHex(12) + "." + std::to_string(now) + "@" + fqdn + ">";

	std::string head;
	head += "Message-ID: " + message_id + "\r\n";
	head += "Date: " + RfcDate(now) + "\r\n";
	head += "From: " + HeaderSafe(from) + "\r\n";
	head += "To: " + HeaderSafe(to) + "\r\n";
	if (!cc.empty()) {
		head += "Cc: " + HeaderSafe(cc) + "\r\n";
	}
	head += "Subject: " + HeaderSafe(subject) + "\r\n";
	if (!in_reply_to.empty()) {
		head += "In-Reply-To: " + HeaderSafe(in_reply_to) + "\r\n";
	}
	if (!references.empty()) {
		head += "References: " + HeaderSafe(references) + "\r\n";
	}
	head += "MIME-Version: 1.0\r\n";

	if (files.empty()) {
		head += "Content-Type: text/plain; charset=utf-8\r\n";
		head += "Content-Transfer-Encoding: 8bit\r\n\r\n";
		return head + CrlfBody(body);
	}

	std::string boundary = "=_qc_" + quackmail::util::RandomHex(16);
	head += "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n\r\n";
	std::string out = head;
	out += "--" + boundary + "\r\n";
	out += "Content-Type: text/plain; charset=utf-8\r\n";
	out += "Content-Transfer-Encoding: 8bit\r\n\r\n";
	out += CrlfBody(body);
	out += "\r\n";
	for (auto &f : files) {
		if (f.content.empty()) {
			continue;
		}
		std::string name = http::SanitizeFilename(f.filename);
		std::string type = f.content_type.empty() ? "application/octet-stream" : f.content_type;
		out += "--" + boundary + "\r\n";
		out += "Content-Type: " + HeaderSafe(type) + "; name=\"" + HeaderSafe(name) + "\"\r\n";
		out += "Content-Disposition: attachment; filename=\"" + HeaderSafe(name) + "\"\r\n";
		out += "Content-Transfer-Encoding: base64\r\n\r\n";
		out += Base64Wrapped(f.content);
	}
	out += "--" + boundary + "--\r\n";
	return out;
}

// Split a comma-separated recipient list into addresses.
std::vector<std::string> SplitAddresses(const std::string &value) {
	std::vector<std::string> out;
	for (auto &a : quackmail::mime::ParseAddressList(value)) {
		if (!a.addr.empty()) {
			out.push_back(a.addr);
		}
	}
	if (out.empty()) {
		// ParseAddressList wants something address-shaped; fall back to a plain
		// comma split so a bare local user name still works.
		size_t pos = 0;
		while (pos <= value.size()) {
			size_t comma = value.find(',', pos);
			std::string one =
			    value.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
			while (!one.empty() && one.front() == ' ') {
				one.erase(0, 1);
			}
			while (!one.empty() && one.back() == ' ') {
				one.pop_back();
			}
			if (!one.empty()) {
				out.push_back(one);
			}
			if (comma == std::string::npos) {
				break;
			}
			pos = comma + 1;
		}
	}
	return out;
}

// ---- folder overview -----------------------------------------------------

void GetMailIndex(Ctx &ctx) {
	quackmail::citadel::EnsureUserRooms(ctx.con, ctx.username);

	std::vector<Room> folders;
	for (const char *name : kFolders) {
		Room room;
		if (ResolveRoomFor(ctx, name, room) && room.mailbox_owner > 0) {
			folders.push_back(room);
		}
	}
	// Any other personal room (a Sieve fileinto target, say) after the four.
	for (auto &room : quackmail::citadel::ListRooms(ctx.con, ctx.username, -1, "all")) {
		if (room.mailbox_owner == 0) {
			continue;
		}
		bool known = false;
		for (auto &f : folders) {
			known = known || f.room_num == room.room_num;
		}
		if (!known) {
			folders.push_back(room);
		}
	}

	std::vector<int64_t> nums;
	for (auto &f : folders) {
		nums.push_back(f.room_num);
	}
	auto stats = quackmail::citadel::RoomStatsBulk(ctx.con, ctx.username, nums);

	std::string body = "<div class=\"actions\">" + Link("/mail/compose", "Write a message", "btn") +
	                   "</div>";
	body += "<div class=\"wrap\"><table><tr>" + Head("Folder") + "<th class=\"num\">Unread</th>"
	                                                             "<th class=\"num\">Total</th></tr>";
	for (size_t i = 0; i < folders.size(); i++) {
		body += "<tr" + std::string(stats[i].new_count > 0 ? " class=\"unread\"" : "") + ">";
		body += "<td>" + Link(RoomHref(folders[i]), folders[i].display_name) + "</td>";
		body += "<td class=\"num\">" + std::to_string(stats[i].new_count) + "</td>";
		body += "<td class=\"num\">" + std::to_string(stats[i].total) + "</td>";
		body += "</tr>";
	}
	body += "</table></div>";
	body += "<p class=\"muted\">These are ordinary Citadel rooms — the same messages are visible over "
	        "IMAP, POP3 and the BBS.</p>";
	Render(ctx, "Mail", body);
}

// ---- message parts -------------------------------------------------------

// Find one MIME part of a message by its IMAP-style section number.
bool FindPart(const Message &msg, const std::string &section, quackmail::mime::MimePart &out) {
	if (msg.format_type != 4) {
		return false;
	}
	auto entity = quackmail::mime::ParseEntity(msg.raw);
	for (auto &p : quackmail::mime::FlattenParts(entity)) {
		if (p.section == section) {
			out = p;
			return true;
		}
	}
	return false;
}

bool LoadForPart(Ctx &ctx, Room &room, Message &msg) {
	if (!ResolveRoomNumFor(ctx, CapNum(ctx, 0), room)) {
		return false;
	}
	if (!quackmail::citadel::RoomUnlocked(ctx.con, ctx.username, room)) {
		return false;
	}
	return LoadMessageIn(ctx, room, CapNum(ctx, 1), msg);
}

void GetMsgPart(Ctx &ctx) {
	Room room;
	Message msg;
	if (!LoadForPart(ctx, room, msg)) {
		NotFound(ctx);
		return;
	}
	quackmail::mime::MimePart part;
	if (!FindPart(msg, ctx.Cap(2), part)) {
		NotFound(ctx);
		return;
	}

	// Never the sender's Content-Type, and never inline: an attachment served
	// as text/html from our origin would be a stored XSS with a download link.
	std::string name = http::SanitizeFilename(part.filename.empty() ? "attachment" : part.filename);
	SecurityHeaders(ctx, "default-src 'none'; sandbox");
	ctx.resp.Bytes(part.content, "application/octet-stream");
	ctx.resp.SetHeader("Content-Disposition",
	                   "attachment; filename=\"" + name + "\"; filename*=UTF-8''" +
	                       http::PercentEncode(name));
}

void GetMsgHtml(Ctx &ctx) {
	Room room;
	Message msg;
	if (!LoadForPart(ctx, room, msg)) {
		NotFound(ctx);
		return;
	}
	auto entity = quackmail::mime::ParseEntity(msg.raw);
	std::string html;
	for (auto &p : quackmail::mime::FlattenParts(entity)) {
		if (p.content_type == "text/html" && p.filename.empty()) {
			html = p.content;
			break;
		}
	}
	if (html.empty()) {
		NotFound(ctx);
		return;
	}

	// Sanitizing is defence in depth; the policy below is the actual boundary.
	// The parent page frames this with `sandbox` and no allow-scripts and no
	// allow-same-origin, so the markup runs in an opaque origin with no script
	// and no access to anything of ours.
	html = SanitizeHtmlPart(html);
	bool show_remote = ctx.req.Param("images") == "1";
	std::string img = show_remote ? "img-src data: https:" : "img-src data:";
	SecurityHeaders(ctx, "default-src 'none'; " + img +
	                         "; style-src 'unsafe-inline'; frame-ancestors 'self'; sandbox");
	ctx.resp.SetHeader("X-Frame-Options", "SAMEORIGIN"); // it is meant to be framed by us
	ctx.resp.Bytes(html, "text/html; charset=utf-8");
	ctx.resp.SetHeader("Content-Disposition", "inline");
}

void GetMsgSource(Ctx &ctx) {
	Room room;
	Message msg;
	if (!LoadForPart(ctx, room, msg)) {
		NotFound(ctx);
		return;
	}
	std::string node = ConfigStr(ctx.con, "c_nodename", "quackcit");
	std::string raw = msg.format_type == 4 ? msg.raw : quackmail::citadel::RenderRfc822(msg, node);
	SecurityHeaders(ctx, "default-src 'none'; sandbox");
	ctx.resp.Bytes(raw, "text/plain; charset=utf-8");
	ctx.resp.SetHeader("Content-Disposition", "inline");
}

// ---- compose and send ----------------------------------------------------

void GetCompose(Ctx &ctx) {
	std::string to = ctx.req.Param("to");
	std::string cc, subject, quoted, in_reply_to, references;

	int64_t src_room = ctx.ParamInt("room", -1);
	int64_t reply = ctx.ParamInt("reply", 0);
	int64_t forward = ctx.ParamInt("forward", 0);
	bool reply_all = ctx.req.Param("all") == "1";
	if (src_room >= 0 && (reply > 0 || forward > 0)) {
		Room room;
		Message msg;
		if (ResolveRoomNumFor(ctx, src_room, room) && LoadMessageIn(ctx, room, reply > 0 ? reply : forward, msg)) {
			std::string node = ConfigStr(ctx.con, "c_nodename", "quackcit");
			auto parsed = quackmail::mime::Parse(msg.raw);
			std::string subj = DecodeHeader(msg.subject);
			if (reply > 0) {
				to = quackmail::mime::ExtractAddress(parsed.from);
				if (to.empty()) {
					to = msg.author;
				}
				if (reply_all) {
					for (auto &h : parsed.headers) {
						if (quackmail::util::Lower(h.first) == "cc") {
							cc = h.second;
						}
					}
				}
				subject = subj.rfind("Re: ", 0) == 0 ? subj : "Re: " + subj;
				in_reply_to = parsed.message_id.empty() ? quackmail::citadel::MessageId(msg, node)
				                                        : parsed.message_id;
				references = in_reply_to;
			} else {
				subject = subj.rfind("Fwd: ", 0) == 0 ? subj : "Fwd: " + subj;
			}
			std::string text = quackmail::citadel::BodyText(msg);
			quoted = "\n\n";
			quoted += reply > 0 ? DecodeHeader(msg.author) + " wrote:\n"
			                    : "----- Forwarded message -----\nFrom: " + DecodeHeader(msg.author) +
			                          "\nSubject: " + subj + "\n\n";
			size_t pos = 0;
			while (pos < text.size()) {
				size_t nl = text.find('\n', pos);
				quoted += (reply > 0 ? "> " : "");
				quoted += text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
				quoted += "\n";
				if (nl == std::string::npos) {
					break;
				}
				pos = nl + 1;
			}
		}
	}

	std::string body = "<form method=\"post\" action=\"/mail/send\" enctype=\"multipart/form-data\">";
	body += Hidden("_csrf", ctx.csrf);
	body += Hidden("in_reply_to", in_reply_to);
	body += Hidden("references", references);
	body += "<label class=\"field\"><span>To</span>" + TextInput("to", to) + "</label>";
	body += "<label class=\"field\"><span>Cc</span>" + TextInput("cc", cc) + "</label>";
	body += "<label class=\"field\"><span>Subject</span>" + TextInput("subject", subject) + "</label>";
	body += "<label class=\"field\"><span>Message</span>" + TextArea("body", quoted, 18) + "</label>";
	body += "<label class=\"field\"><span>Attachment</span><input type=\"file\" name=\"attachment\" "
	        "multiple></label>";
	body += "<p>" + Button("Send") + " ";
	body += "<button class=\"btn sec\" name=\"draft\" value=\"1\">Save as draft</button> ";
	body += Link("/mail/", "Cancel") + "</p>";
	body += "</form>";
	Render(ctx, "Write a message", body);
}

void PostSend(Ctx &ctx) {
	std::vector<std::pair<std::string, std::string>> fields;
	std::vector<http::FormFile> files;
	std::string ct = ctx.req.Header("Content-Type");
	if (!http::ParseMultipart(ct, ctx.req.body, fields, files)) {
		BadRequest(ctx, "The compose form must be submitted as multipart/form-data.");
		return;
	}
	auto field = [&](const char *name) {
		for (auto &f : fields) {
			if (f.first == name) {
				return f.second;
			}
		}
		return std::string();
	};

	// Multipart bodies are parsed here, not by the router, so the CSRF token
	// has to be re-checked against the parsed fields.
	if (!quackmail::web::CheckCsrf(ctx.con, ctx.session_hash, field("_csrf"))) {
		Forbidden(ctx, "This form has expired. Go back, reload the page and try again.");
		return;
	}

	std::string to = field("to");
	std::string cc = field("cc");
	std::string subject = field("subject");
	std::string text = field("body");
	bool draft = !field("draft").empty();

	auto rcpts = SplitAddresses(to);
	for (auto &a : SplitAddresses(cc)) {
		rcpts.push_back(a);
	}
	if (!draft && rcpts.empty()) {
		BadRequest(ctx, "A message needs at least one recipient.");
		return;
	}

	std::string from = SenderAddress(ctx);
	std::string message_id;
	std::string body = BuildMessage(ctx, from, to, cc, subject, text, files, field("in_reply_to"),
	                                field("references"), message_id);

	if (draft) {
		int64_t drafts = quackmail::citadel::GetOrCreateUserRoom(ctx.con, ctx.username, "Drafts");
		Message msg;
		msg.author = ctx.username;
		msg.author_usernum = quackmail::citadel::GetOrAssignUserNum(ctx.con, ctx.username);
		msg.recipient = to;
		msg.msgtime = (int64_t)std::time(nullptr);
		msg.format_type = 4;
		msg.subject = subject;
		msg.origin_room = "Drafts";
		msg.node = ConfigStr(ctx.con, "c_nodename", "quackcit");
		msg.raw = body;
		std::string err;
		std::vector<int64_t> rooms = {drafts};
		quackmail::citadel::InsertMessage(ctx.con, msg, rooms, err);
		RedirectTo(ctx, "/mail/", "saved");
		return;
	}

	// The same quota the SMTP submission port enforces, charged per envelope
	// recipient. Without this webmail would simply be the way around it.
	auto quota = quackmail::policy::CheckRate(ctx.con, ctx.username, (int64_t)rcpts.size());
	if (!quota.allowed) {
		ErrorPage(ctx, 429, "Sending limit reached",
		          quota.reason + " — try again in " + std::to_string(quota.retry_after) + " seconds.");
		return;
	}

	std::string received = "Received: from webmail (authenticated as " + ctx.username + ")\r\n\tby " +
	                       ConfigStr(ctx.con, "c_fqdn", "quackmail.test") + " (QuackCit) with HTTP" +
	                       (ctx.tls ? "S" : "") + ";\r\n\t" + RfcDate((int64_t)std::time(nullptr)) + "\r\n";
	body = received + body;

	// Sign once, so the filed copy and every queued copy carry the same
	// signature — exactly what the submission path does.
	quackmail::policy::DkimKey key;
	std::string domain = ConfigStr(ctx.con, "c_fqdn", "");
	if (quackmail::policy::DkimKeyFor(ctx.con, domain, key) && !key.private_key.empty()) {
		std::string signed_body, err;
		if (quackmail::dkim::Sign(body, key.domain.empty() ? domain : key.domain, key.selector,
		                          key.private_key, key.headers, signed_body, err)) {
			body = signed_body;
		}
	}

	std::vector<std::string> local;
	int64_t queued = 0;
	for (auto &r : rcpts) {
		if (quackmail::citadel::IsLocalUser(ctx.con, r)) {
			local.push_back(r);
		} else {
			quackmail::store::EnqueueOutbound(ctx.con, from, r, body);
			queued++;
		}
	}
	std::string err;
	if (!local.empty() && !quackmail::deliver::LocalDeliver(ctx.con, from, local, body, err)) {
		ErrorPage(ctx, 500, "Could not send", err);
		return;
	}
	for (auto &r : rcpts) {
		quackmail::policy::RecordSend(ctx.con, ctx.username, r, 1);
	}
	quackmail::policy::PruneSendLog(ctx.con);

	// File the sender's own copy.
	int64_t sent = quackmail::citadel::GetOrCreateUserRoom(ctx.con, ctx.username, "Sent Items");
	Message copy;
	copy.author = ctx.username;
	copy.author_usernum = quackmail::citadel::GetOrAssignUserNum(ctx.con, ctx.username);
	copy.recipient = to;
	copy.msgtime = (int64_t)std::time(nullptr);
	copy.format_type = 4;
	copy.subject = subject;
	copy.origin_room = "Sent Items";
	copy.node = ConfigStr(ctx.con, "c_nodename", "quackcit");
	copy.raw = body;
	std::string copy_err;
	std::vector<int64_t> rooms = {sent};
	quackmail::citadel::InsertMessage(ctx.con, copy, rooms, copy_err);

	RedirectTo(ctx, "/mail/", "sent");
}

// ---- filing --------------------------------------------------------------

void PostMove(Ctx &ctx) {
	Room from;
	if (!ResolveRoomNumFor(ctx, ctx.FormInt("room", -1), from)) {
		NotFound(ctx);
		return;
	}
	int64_t msgnum = ctx.FormInt("msgnum", 0);
	Message msg;
	if (!LoadMessageIn(ctx, from, msgnum, msg)) {
		NotFound(ctx);
		return;
	}
	Room target;
	std::string folder = ctx.req.Form("folder");
	if (folder.empty() || !ResolveRoomFor(ctx, folder, target) || target.mailbox_owner == 0) {
		BadRequest(ctx, "Pick one of your own folders to move it to.");
		return;
	}
	std::string err;
	if (!quackmail::citadel::MoveMessage(ctx.con, from.room_num, target.room_num, msgnum, false, err)) {
		ErrorPage(ctx, 500, "Could not move", err);
		return;
	}
	RedirectTo(ctx, RoomHref(from), "moved");
}

void PostTrash(Ctx &ctx) {
	Room from;
	if (!ResolveRoomNumFor(ctx, ctx.FormInt("room", -1), from)) {
		NotFound(ctx);
		return;
	}
	int64_t msgnum = ctx.FormInt("msgnum", 0);
	Message msg;
	if (!LoadMessageIn(ctx, from, msgnum, msg)) {
		NotFound(ctx);
		return;
	}
	Room trash;
	std::string err;
	// From anywhere else, "delete" files it into Trash; from Trash itself it
	// really is gone.
	if (from.display_name != "Trash" && ResolveRoomFor(ctx, "Trash", trash) && trash.mailbox_owner > 0) {
		if (!quackmail::citadel::MoveMessage(ctx.con, from.room_num, trash.room_num, msgnum, false, err)) {
			ErrorPage(ctx, 500, "Could not move to Trash", err);
			return;
		}
	} else if (!quackmail::citadel::DeleteMessage(ctx.con, from.room_num, msgnum, err)) {
		ErrorPage(ctx, 500, "Could not delete", err);
		return;
	}
	RedirectTo(ctx, RoomHref(from), "deleted");
}

void PostFlag(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, ctx.FormInt("room", -1), room)) {
		NotFound(ctx);
		return;
	}
	int64_t msgnum = ctx.FormInt("msgnum", 0);
	Message msg;
	if (!LoadMessageIn(ctx, room, msgnum, msg)) {
		NotFound(ctx);
		return;
	}
	// citadel_msg_flags is the same table IMAP reads, so a flag set here shows
	// up in a desktop mail client.
	std::string flag = ctx.req.Form("flag") == "seen" ? "\\Seen" : "\\Flagged";
	bool on = ctx.req.Form("on") == "1";
	const char *sql = on ? "INSERT INTO citadel_msg_flags (msgnum, username, flag) "
	                       "SELECT $1, $2, $3 WHERE NOT EXISTS (SELECT 1 FROM citadel_msg_flags "
	                       "WHERE msgnum = $1 AND username = $2 AND flag = $3)"
	                     : "DELETE FROM citadel_msg_flags WHERE msgnum = $1 AND username = $2 "
	                       "AND flag = $3";
	Exec(ctx.con, sql, {Value::BIGINT(msgnum), Value(ctx.username), Value(flag)});
	RedirectTo(ctx, RoomHref(room, "/msg/" + std::to_string(msgnum)));
}

} // namespace

// A conservative allow-list sanitizer for a text/html mail part. The real
// boundary is the sandboxed frame and its own `default-src 'none'` policy;
// this exists so that a browser which somehow ignores one of those still does
// not see a <script> tag.
std::string SanitizeHtmlPart(const std::string &in) {
	static const char *kDropWithContent[] = {"script", "style", "iframe", "object", "embed", "applet",
	                                         "form",   "frame", "frameset"};
	std::string out;
	out.reserve(in.size());
	size_t i = 0;
	while (i < in.size()) {
		if (in[i] != '<') {
			out += in[i++];
			continue;
		}
		size_t close = in.find('>', i);
		if (close == std::string::npos) {
			break; // a truncated tag: drop the remainder rather than guess
		}
		std::string tag = in.substr(i, close - i + 1);
		std::string lower = quackmail::util::Lower(tag);

		// Element name.
		size_t p = 1;
		bool closing = p < lower.size() && lower[p] == '/';
		if (closing) {
			p++;
		}
		std::string name;
		while (p < lower.size() && ((lower[p] >= 'a' && lower[p] <= 'z') || (lower[p] >= '0' && lower[p] <= '9'))) {
			name += lower[p++];
		}

		bool drop = false;
		for (const char *bad : kDropWithContent) {
			if (name == bad) {
				drop = true;
				break;
			}
		}
		if (name == "link" || name == "meta" || name == "base") {
			drop = true;
		}
		if (drop) {
			// Skip the element's content too, for the ones that have any.
			if (!closing && (name == "script" || name == "style")) {
				size_t endtag = lower.find("</" + name, close);
				i = endtag == std::string::npos ? in.size() : in.find('>', endtag);
				i = i == std::string::npos ? in.size() : i + 1;
			} else {
				i = close + 1;
			}
			continue;
		}
		// Event handlers and javascript:/vbscript: URLs, whatever element they
		// are on.
		if (lower.find(" on") != std::string::npos || lower.find("javascript:") != std::string::npos ||
		    lower.find("vbscript:") != std::string::npos) {
			// Keep the element, lose its attributes.
			out += "<" + std::string(closing ? "/" : "") + name + ">";
			i = close + 1;
			continue;
		}
		out += tag;
		i = close + 1;
	}
	return out;
}

void RegisterMailRoutes(std::vector<Route> &out) {
	out.push_back({"GET", "/mail/", Role::User, GetMailIndex});
	out.push_back({"GET", "/mail/compose", Role::User, GetCompose});
	out.push_back({"POST", "/mail/send", Role::User, PostSend});
	out.push_back({"POST", "/mail/move", Role::User, PostMove});
	out.push_back({"POST", "/mail/delete", Role::User, PostTrash});
	out.push_back({"POST", "/mail/flag", Role::User, PostFlag});
	// The per-message part routes hang off the canonical room/message URL, so
	// BBS posts and mail behave identically.
	out.push_back({"GET", "/bbs/room/:n/msg/:m/part/:s", Role::User, GetMsgPart});
	out.push_back({"GET", "/bbs/room/:n/msg/:m/html", Role::User, GetMsgHtml});
	out.push_back({"GET", "/bbs/room/:n/msg/:m/source", Role::User, GetMsgSource});
}

} // namespace qmweb
} // namespace duckdb
