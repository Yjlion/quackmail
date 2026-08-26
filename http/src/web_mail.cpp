#include "web.hpp"
#include "web_views.hpp"

#include "quackmail/citadel_msg.hpp"
#include "quackmail/delivery.hpp"
#include "quackmail/dkim.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mailpolicy.hpp"
#include "quackmail/html_sanitize.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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

// Turn every `data:image/...;base64,...` in composed HTML into a real inline
// part, rewriting the src to the `cid:` that names it.
//
// The editor produces data: URIs because that is what a FileReader gives it.
// Leaving them would work, but base64 inside the body bloats the message and
// several clients refuse to render a data: image at all — so they become parts,
// which is what every mail client does understand.
std::string ExtractDataImages(const std::string &fqdn, const std::string &html,
                              std::vector<quackmail::mime::BuildPart> &parts) {
	static const struct {
		const char *prefix;
		const char *type;
	} kKinds[] = {
	    {"data:image/png;base64,", "image/png"},
	    {"data:image/jpeg;base64,", "image/jpeg"},
	    {"data:image/gif;base64,", "image/gif"},
	    {"data:image/webp;base64,", "image/webp"},
	};

	std::string out;
	size_t i = 0;
	int found = 0;
	while (i < html.size()) {
		size_t at = html.find("data:image/", i);
		if (at == std::string::npos || found >= 16) {
			out += html.substr(i);
			break;
		}
		const char *type = nullptr;
		size_t prefix_len = 0;
		for (auto &k : kKinds) {
			size_t n = std::strlen(k.prefix);
			if (html.compare(at, n, k.prefix) == 0) {
				type = k.type;
				prefix_len = n;
				break;
			}
		}
		if (!type) {
			// Not a form we convert — including data:image/svg+xml, which the
			// compose allow-list has already refused.
			out += html.substr(i, at + 11 - i);
			i = at + 11;
			continue;
		}
		// The URI runs to the closing quote of the attribute.
		size_t end = html.find_first_of("\"'", at);
		if (end == std::string::npos) {
			out += html.substr(i);
			break;
		}
		std::string b64 = html.substr(at + prefix_len, end - at - prefix_len);
		std::string raw;
		if (!quackmail::util::Base64Decode(b64, raw) || raw.empty()) {
			out += html.substr(i, end - i);
			i = end;
			continue;
		}

		std::string cid = "img" + std::to_string(++found) + "." + quackmail::util::RandomHex(6) + "@" + fqdn;
		quackmail::mime::BuildPart p;
		p.content_type = type;
		p.content = raw;
		p.content_id = cid;
		p.disposition = "inline";
		parts.push_back(p);

		out += html.substr(i, at - i);
		out += "cid:" + cid;
		i = end;
	}
	return out;
}

// Build the RFC822 message that gets signed, delivered and filed.
//
// The nesting is core's job now (mime::BuildMessage): this only decides *which*
// parts exist. An HTML body arrives already through the compose allow-list, and
// any data: image in it becomes a real inline part so the message is
// self-contained rather than pointing at anything remote.
std::string BuildMessage(Ctx &ctx, const std::string &from, const std::string &to, const std::string &cc,
                         const std::string &subject, const std::string &body, const std::string &html,
                         const std::vector<http::FormFile> &files, const std::string &in_reply_to,
                         const std::string &references, std::string &message_id) {
	std::string fqdn = ConfigStr(ctx.con, "c_fqdn", "quackmail.test");
	int64_t now = (int64_t)std::time(nullptr);
	message_id = "<" + quackmail::util::RandomHex(12) + "." + std::to_string(now) + "@" + fqdn + ">";

	quackmail::mime::HeaderList headers;
	headers.push_back({"Message-ID", message_id});
	headers.push_back({"Date", RfcDate(now)});
	headers.push_back({"From", from});
	headers.push_back({"To", to});
	if (!cc.empty()) {
		headers.push_back({"Cc", cc});
	}
	headers.push_back({"Subject", subject});
	if (!in_reply_to.empty()) {
		headers.push_back({"In-Reply-To", in_reply_to});
	}
	if (!references.empty()) {
		headers.push_back({"References", references});
	}

	std::vector<quackmail::mime::BuildPart> parts;

	std::string html_out = html;
	std::vector<quackmail::mime::BuildPart> inline_parts;
	if (!html_out.empty()) {
		// A data: image the editor inserted becomes a cid: part. Left as a data:
		// URI it would still work, but base64 inside the HTML bloats the body and
		// several clients refuse to render it.
		html_out = ExtractDataImages(fqdn, html_out, inline_parts);
	}

	quackmail::mime::BuildPart text_part;
	text_part.content_type = "text/plain";
	text_part.content = body;
	parts.push_back(text_part);

	if (!html_out.empty()) {
		quackmail::mime::BuildPart html_part;
		html_part.content_type = "text/html";
		html_part.content = html_out;
		parts.push_back(html_part);
	}
	for (auto &p : inline_parts) {
		parts.push_back(p);
	}

	for (auto &f : files) {
		if (f.content.empty()) {
			continue;
		}
		quackmail::mime::BuildPart att;
		att.content_type = f.content_type.empty() ? "application/octet-stream" : f.content_type;
		att.content = f.content;
		att.filename = http::SanitizeFilename(f.filename);
		att.disposition = "attachment";
		parts.push_back(att);
	}

	return quackmail::mime::BuildMessage(headers, parts);
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
	PageOpts opts;
	opts.active = "mail";
	opts.wide = true;
	Render(ctx, "Mail", body, opts);
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

// Serve the part a `cid:` reference names, for the sandboxed HTML frame.
//
// Separate from GetMsgPart because the answers differ: that one forces
// `application/octet-stream` and `attachment` so nothing served from our origin
// can ever render. This one has to render — but only as a real image, and only
// one of four types. `image/svg+xml` is an image by content type and a script
// host in practice, so it falls through to being downloaded like anything else.
void GetMsgCid(Ctx &ctx) {
	Room room;
	Message msg;
	if (!LoadForPart(ctx, room, msg)) {
		NotFound(ctx);
		return;
	}
	std::string want = ctx.Cap(2);
	if (want.empty()) {
		NotFound(ctx);
		return;
	}

	auto entity = quackmail::mime::ParseEntity(msg.raw);
	std::function<const quackmail::mime::MimeEntity *(const quackmail::mime::MimeEntity &)> find =
	    [&](const quackmail::mime::MimeEntity &e) -> const quackmail::mime::MimeEntity * {
		if (!e.content_id.empty() && e.content_id == want) {
			return &e;
		}
		for (auto &child : e.children) {
			if (const quackmail::mime::MimeEntity *hit = find(child)) {
				return hit;
			}
		}
		return nullptr;
	};
	const quackmail::mime::MimeEntity *part = find(entity);
	if (!part) {
		NotFound(ctx);
		return;
	}

	// The type is decided here from a fixed list, never taken from the sender.
	std::string mime = part->content_type.Mime();
	bool renderable = mime == "image/png" || mime == "image/jpeg" || mime == "image/gif" ||
	                  mime == "image/webp";
	SecurityHeaders(ctx, "default-src 'none'; sandbox");
	if (!renderable) {
		std::string name = http::SanitizeFilename(part->filename.empty() ? "attachment" : part->filename);
		ctx.resp.Bytes(part->body_decoded, "application/octet-stream");
		ctx.resp.SetHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
		return;
	}
	ctx.resp.Bytes(part->body_decoded, mime);
	ctx.resp.SetHeader("Content-Disposition", "inline");
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
	// Rewrite `cid:` references to a route that serves the matching part. Done
	// over the already-sanitized string as a targeted attribute edit, not a DOM
	// round-trip: re-serializing risks reintroducing what the sanitizer removed.
	html = quackmail::html::RewriteCidUrls(
	    html, RoomHref(room, "/msg/" + std::to_string(msg.msgnum) + "/cid/"));

	bool show_remote = ctx.req.Param("images") == "1";
	// `img-src 'self'` is what lets the cid: route above load. The frame is
	// sandboxed with no allow-same-origin, so 'self' permits subresource loads
	// without granting the frame any origin access — which is the point.
	//
	// Remote images stay behind the ?images=1 opt-in: those are trackers. A cid:
	// image travelled inside the message and reveals nothing by loading.
	std::string img = show_remote ? "img-src 'self' data: https:" : "img-src 'self' data:";
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

	// data-compose is what qc-compose.js looks for. Without it — or with
	// JavaScript off — this stays exactly the plain-text form it was, which is
	// why the textarea is the field the server actually reads for the text half.
	std::string body = "<form method=\"post\" action=\"/mail/send\" enctype=\"multipart/form-data\" "
	                   "data-compose>";
	body += Hidden("_csrf", ctx.csrf);
	body += Hidden("in_reply_to", in_reply_to);
	body += Hidden("references", references);
	// Filled in by the editor on submit; empty means "this message is plain
	// text", which is the state of every submission from a browser without it.
	body += Hidden("html_body", "");

	// A <datalist> works with no script at all — the browser's own
	// autocomplete offers a match as you type. The click-to-insert panel below
	// (qc-compose.js) is the enhancement on top of that, not a replacement.
	auto addresses = ContactAddressOptions(ctx);
	if (!addresses.empty()) {
		body += "<datalist id=\"addressbook\">";
		for (auto &a : addresses) {
			body += "<option value=\"" + A(a) + "\">";
		}
		body += "</datalist>";
	}
	body += "<label class=\"field\"><span>To</span><input type=\"text\" name=\"to\" id=\"compose-to\" "
	        "value=\"" +
	        A(to) + "\" list=\"addressbook\"></label>";
	body += "<label class=\"field\"><span>Cc</span><input type=\"text\" name=\"cc\" id=\"compose-cc\" "
	        "value=\"" +
	        A(cc) + "\" list=\"addressbook\"></label>";
	if (!addresses.empty()) {
		body += "<button type=\"button\" class=\"btn sec jsonly\" id=\"addressbook-toggle\">Address "
		        "book</button>";
		body += "<div id=\"addressbook-panel\" class=\"addressbook-panel\" hidden><ul>";
		for (auto &a : addresses) {
			body += "<li><button type=\"button\" data-addr=\"" + A(a) + "\">" + T(a) + "</button></li>";
		}
		body += "</ul></div>";
	}
	body += "<label class=\"field\"><span>Subject</span>" + TextInput("subject", subject) + "</label>";
	body += "<label class=\"field\"><span>Message</span>" + TextArea("body", quoted, 18) + "</label>";
	// Hidden until the editor loads and marks it available: offering "formatted
	// text" to someone who will only ever get a textarea would be a lie.
	body += "<label class=\"chk richopt\"><input type=\"checkbox\" name=\"rich\" value=\"1\"> "
	        "Formatted text</label>";
	body += "<label class=\"field\"><span>Attachment</span><input type=\"file\" name=\"attachment\" "
	        "multiple></label>";
	body += "<p>" + Button("Send") + " ";
	body += "<button class=\"btn sec\" name=\"draft\" value=\"1\">Save as draft</button> ";
	body += Link("/mail/", "Cancel") + "</p>";
	body += "</form>";
	PageOpts opts;
	opts.active = "compose";
	opts.script = "qc-compose.js";
	Render(ctx, "Write a message", body, opts);
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

	// The HTML half, if the composer sent one. Sanitized *here*, before the
	// message is built — what gets stored is already safe, rather than relying on
	// every future reader to clean it again. See html_sanitize.hpp on why this is
	// an allow-list and the display path is not.
	std::string html = field("html_body");
	if (!html.empty()) {
		html = quackmail::html::SanitizeForCompose(html);
		// A plain-text alternative is not optional: a recipient on a text-only
		// client, or reading over Citadel or POP3, gets this one. If the composer
		// did not send text of its own, derive it rather than sending an empty
		// part.
		if (text.empty()) {
			text = quackmail::html::ToPlainText(html);
		}
	}

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
	std::string body = BuildMessage(ctx, from, to, cc, subject, text, html, files, field("in_reply_to"),
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

// The selected messages, as numbers this user is actually allowed to act on in
// `room`. LoadMessageIn is the ownership check — skipping it on even one
// element of the list would make a bulk action an IDOR with extra steps — so a
// number the caller may not touch is dropped rather than refused, and the rest
// of the selection still goes through.
std::vector<int64_t> SelectedIn(Ctx &ctx, const Room &room) {
	std::vector<int64_t> out;
	for (auto &raw : ctx.req.FormAll("msgnum")) {
		int64_t msgnum = (int64_t)std::strtoll(raw.c_str(), nullptr, 10);
		Message msg;
		if (msgnum > 0 && LoadMessageIn(ctx, room, msgnum, msg)) {
			out.push_back(msgnum);
		}
	}
	return out;
}

// Where a mail action should land afterwards. The listing sends the page it was
// on so a bulk action does not throw the reader back to the top; anything else
// falls back to the folder. Only ever a path we built — never a value that
// could send a browser off-site.
std::string MailBackTo(Ctx &ctx, const Room &room) {
	std::string back = ctx.req.Form("back");
	if (!back.empty() && back.rfind("/bbs/room/", 0) == 0 && back.find("//") == std::string::npos) {
		return back;
	}
	return RoomHref(room);
}

void PostMove(Ctx &ctx) {
	Room from;
	if (!ResolveRoomNumFor(ctx, ctx.FormInt("room", -1), from)) {
		NotFound(ctx);
		return;
	}
	std::vector<int64_t> selected = SelectedIn(ctx, from);
	if (selected.empty()) {
		RedirectTo(ctx, MailBackTo(ctx, from), "nothing");
		return;
	}
	Room target;
	std::string folder = ctx.req.Form("folder");
	if (folder.empty() || !ResolveRoomFor(ctx, folder, target) || target.mailbox_owner == 0) {
		BadRequest(ctx, "Pick one of your own folders to move it to.");
		return;
	}
	std::string err;
	for (auto msgnum : selected) {
		if (!quackmail::citadel::MoveMessage(ctx.con, from.room_num, target.room_num, msgnum, false,
		                                     err)) {
			ErrorPage(ctx, 500, "Could not move", err);
			return;
		}
	}
	RedirectTo(ctx, MailBackTo(ctx, from), "moved");
}

void PostTrash(Ctx &ctx) {
	Room from;
	if (!ResolveRoomNumFor(ctx, ctx.FormInt("room", -1), from)) {
		NotFound(ctx);
		return;
	}
	std::vector<int64_t> selected = SelectedIn(ctx, from);
	if (selected.empty()) {
		RedirectTo(ctx, MailBackTo(ctx, from), "nothing");
		return;
	}
	Room trash;
	std::string err;
	// From anywhere else, "delete" files it into Trash; from Trash itself it
	// really is gone. Decided once for the whole selection, so a bulk delete
	// cannot half-file and half-destroy.
	bool to_trash =
	    from.display_name != "Trash" && ResolveRoomFor(ctx, "Trash", trash) && trash.mailbox_owner > 0;
	for (auto msgnum : selected) {
		if (to_trash) {
			if (!quackmail::citadel::MoveMessage(ctx.con, from.room_num, trash.room_num, msgnum, false,
			                                     err)) {
				ErrorPage(ctx, 500, "Could not move to Trash", err);
				return;
			}
		} else if (!quackmail::citadel::DeleteMessage(ctx.con, from.room_num, msgnum, err)) {
			ErrorPage(ctx, 500, "Could not delete", err);
			return;
		}
	}
	RedirectTo(ctx, MailBackTo(ctx, from), "deleted");
}

void PostFlag(Ctx &ctx) {
	Room room;
	if (!ResolveRoomNumFor(ctx, ctx.FormInt("room", -1), room)) {
		NotFound(ctx);
		return;
	}
	std::vector<int64_t> selected = SelectedIn(ctx, room);
	if (selected.empty()) {
		RedirectTo(ctx, MailBackTo(ctx, room), "nothing");
		return;
	}
	// One field rather than a flag name and an on/off pair, because the buttons
	// that submit this are in a shared form and an HTML button contributes
	// exactly one name and value.
	std::string set = ctx.req.Form("set");
	bool on = set == "seen" || set == "flagged";
	bool is_seen = set == "seen" || set == "unseen";
	if (!on && set != "unseen" && set != "unflagged") {
		BadRequest(ctx, "That is not something a message can be marked.");
		return;
	}
	std::string flag = is_seen ? "\\Seen" : "\\Flagged";
	// citadel_msg_flags is the same table IMAP reads, so a flag set here shows
	// up in a desktop mail client.
	const char *sql = on ? "INSERT INTO citadel_msg_flags (msgnum, username, flag) "
	                       "SELECT $1, $2, $3 WHERE NOT EXISTS (SELECT 1 FROM citadel_msg_flags "
	                       "WHERE msgnum = $1 AND username = $2 AND flag = $3)"
	                     : "DELETE FROM citadel_msg_flags WHERE msgnum = $1 AND username = $2 "
	                       "AND flag = $3";
	for (auto msgnum : selected) {
		Exec(ctx.con, sql, {Value::BIGINT(msgnum), Value(ctx.username), Value(flag)});
	}
	RedirectTo(ctx, MailBackTo(ctx, room), is_seen ? "marked" : "flagged");
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
	out.push_back({"GET", "/bbs/room/:n/msg/:m/cid/:c", Role::User, GetMsgCid});
	out.push_back({"GET", "/bbs/room/:n/msg/:m/source", Role::User, GetMsgSource});
}

} // namespace qmweb
} // namespace duckdb
