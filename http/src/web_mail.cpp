#include "web.hpp"
#include "web_views.hpp"

#include "web_i18n.hpp"

#include "quackmail/citadel_msg.hpp"
#include "quackmail/delivery.hpp"
#include "quackmail/dkim.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mailpolicy.hpp"
#include "quackmail/html_sanitize.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/quota.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <cctype>
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

	std::string body =
	    Toolbar(Link("/mail/compose", Tr(ctx, "mail.write"), "btn"));
	Table table(ctx, "mail-folders",
	            {Column("folder", Tr(ctx, "mail.folder")),
	             Column::Num("unread", Tr(ctx, "mail.unread")),
	             Column::Num("total", Tr(ctx, "mail.total"))});
	for (size_t i = 0; i < folders.size(); i++) {
		table.Add(stats[i].new_count > 0 ? "unread" : "")
		    .Html(Link(RoomHref(folders[i]), folders[i].display_name), folders[i].display_name)
		    .Number(stats[i].new_count)
		    .Number(stats[i].total);
	}
	body += table.Render();
	body += "<p class=\"muted\">" + T(Tr(ctx, "mail.footer")) + "</p>";
	PageOpts opts;
	opts.active = "mail";
	opts.wide = true;
	Render(ctx, Tr(ctx, "mail.title"), body, opts);
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

// Case-insensitive header lookup on a parsed message. ParsedMessage pulls out
// From/Subject/Message-ID and leaves everything else in `headers`, which is
// where To, Cc and References live.
std::string HeaderOf(const quackmail::mime::ParsedMessage &msg, const char *name) {
	std::string want = quackmail::util::Lower(name);
	for (const auto &h : msg.headers) {
		if (quackmail::util::Lower(h.first) == want) {
			return h.second;
		}
	}
	return std::string();
}

// "Name <addr>", or the bare address when there is no name. The name is quoted
// whenever it holds one of RFC 5322's specials — without that, a contact filed
// as "Smith, John" comes back as two recipients, one of which does not exist.
std::string FormatAddress(const quackmail::mime::Address &a) {
	if (a.name.empty()) {
		return a.addr;
	}
	if (a.name.find_first_of("()<>[]:;@\\,.\"") == std::string::npos) {
		return a.name + " <" + a.addr + ">";
	}
	std::string quoted = "\"";
	for (char c : a.name) {
		if (c == '"' || c == '\\') {
			quoted += '\\';
		}
		quoted += c;
	}
	quoted += "\"";
	return quoted + " <" + a.addr + ">";
}

// Merge address-list header values into one field value, dropping duplicates
// and anything already in `seen`.
//
// `seen` is seeded with the signed-in user's own address and then carried from
// To to Cc, so a reply-all neither writes back to the sender themselves nor
// lists the same person in both fields.
std::string MergeAddresses(const std::vector<std::string> &values, std::set<std::string> &seen) {
	std::string out;
	for (const auto &v : values) {
		for (auto &a : quackmail::mime::ParseAddressList(DecodeHeader(v))) {
			if (a.addr.empty()) {
				continue;
			}
			if (!seen.insert(quackmail::util::Lower(a.addr)).second) {
				continue;
			}
			if (!out.empty()) {
				out += ", ";
			}
			out += FormatAddress(a);
		}
	}
	return out;
}

// Reply and forward prefixes, lowercased. Not just English: a thread that has
// been through a German, Swedish, Dutch or French client comes back carrying
// that client's prefix, and a literal `rfind("Re: ")` sees none of them and
// stacks another one on top — which is how a subject ends up reading
// "Re: AW: Re: Re: SV: lunch".
const char *const kReplyPrefixes[] = {"re", "aw", "antw", "antwort", "sv", "vs",
                                      "ref", "res", "odp", "ynt", "atb"};
const char *const kForwardPrefixes[] = {"fwd", "fw", "wg", "tr", "rv", "vs", "doorst", "i"};

// Does `subject` already start with one of `tokens`? Case-insensitive, and it
// accepts the counted forms ("Re[2]:", "Re(3):") Outlook and some list managers
// write, because those are prefixes too.
bool HasPrefix(const std::string &subject, const char *const *tokens, size_t count) {
	size_t i = 0;
	while (i < subject.size() && (subject[i] == ' ' || subject[i] == '\t')) {
		i++;
	}
	size_t start = i;
	while (i < subject.size() && std::isalpha((unsigned char)subject[i])) {
		i++;
	}
	if (i == start) {
		return false;
	}
	std::string token = quackmail::util::Lower(subject.substr(start, i - start));
	if (i < subject.size() && (subject[i] == '[' || subject[i] == '(')) {
		char close = subject[i] == '[' ? ']' : ')';
		size_t j = i + 1;
		while (j < subject.size() && std::isdigit((unsigned char)subject[j])) {
			j++;
		}
		if (j > i + 1 && j < subject.size() && subject[j] == close) {
			i = j + 1;
		}
	}
	if (i >= subject.size() || subject[i] != ':') {
		return false;
	}
	for (size_t k = 0; k < count; k++) {
		if (token == tokens[k]) {
			return true;
		}
	}
	return false;
}

std::string PrefixSubject(const std::string &subject, bool reply) {
	if (reply) {
		return HasPrefix(subject, kReplyPrefixes, sizeof(kReplyPrefixes) / sizeof(*kReplyPrefixes))
		           ? subject
		           : "Re: " + subject;
	}
	return HasPrefix(subject, kForwardPrefixes, sizeof(kForwardPrefixes) / sizeof(*kForwardPrefixes))
	           ? subject
	           : "Fwd: " + subject;
}

// What the compose form holds, whatever seeded it: a blank page, a reply, a
// forward, or a draft being resumed.
struct ComposeState {
	std::string to, cc, bcc, subject, text;
	std::string in_reply_to, references;
	// The Drafts message this replaces when saved again. Resuming a draft and
	// saving it must not leave two.
	int64_t draft_of = 0;
	// The message carried along as a message/rfc822 attachment on send. Held as
	// room + number rather than as bytes, so the form stays small and the
	// ownership check happens again at POST time.
	int64_t fwd_room = -1;
	int64_t fwd_msg = 0;
};

// Seed from a saved draft. Everything comes back out of the stored RFC822,
// which is the same thing every other client would read it as.
bool LoadDraft(Ctx &ctx, int64_t msgnum, ComposeState &st) {
	Room drafts;
	if (msgnum <= 0 || !ResolveRoomFor(ctx, "Drafts", drafts)) {
		return false;
	}
	Message msg;
	if (!LoadMessageIn(ctx, drafts, msgnum, msg)) {
		return false;
	}
	auto parsed = quackmail::mime::Parse(msg.raw);
	st.to = DecodeHeader(HeaderOf(parsed, "to"));
	st.cc = DecodeHeader(HeaderOf(parsed, "cc"));
	st.bcc = DecodeHeader(HeaderOf(parsed, "bcc"));
	st.subject = DecodeHeader(parsed.subject.empty() ? msg.subject : parsed.subject);
	// Carried through, so a reply saved as a draft still threads when it is
	// finally sent.
	st.in_reply_to = HeaderOf(parsed, "in-reply-to");
	st.references = HeaderOf(parsed, "references");
	st.text = quackmail::citadel::BodyText(msg);
	st.draft_of = msgnum;
	return true;
}

// Quote a message body, one "> " per line for a reply and verbatim under a
// header block for a forward.
std::string QuoteBody(const Message &msg, const std::string &subject, bool reply) {
	std::string text = quackmail::citadel::BodyText(msg);
	std::string out = "\n\n";
	out += reply ? DecodeHeader(msg.author) + " wrote:\n"
	             : "----- Forwarded message -----\nFrom: " + DecodeHeader(msg.author) +
	                   "\nSubject: " + subject + "\n\n";
	size_t pos = 0;
	while (pos < text.size()) {
		size_t nl = text.find('\n', pos);
		out += reply ? "> " : "";
		out += text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
		out += "\n";
		if (nl == std::string::npos) {
			break;
		}
		pos = nl + 1;
	}
	return out;
}

// The signature, appended to a fresh message only. A draft already has it in
// its stored body, and appending it again on every resume is how a signature
// ends up in a message four times.
std::string SignatureFor(Ctx &ctx) {
	std::string sig = quackmail::citadel::GetUserPref(ctx.con, ctx.username, "web_signature");
	return sig.empty() ? std::string() : "\n\n-- \n" + sig + "\n";
}

// The address book as "Name <addr>" strings, optionally filtered. The filter is
// a plain case-insensitive substring over both halves, which is what a person
// typing three letters of a surname means.
std::vector<std::string> AddressBook(Ctx &ctx, const std::string &query) {
	auto all = ContactAddressOptions(ctx);
	if (query.empty()) {
		return all;
	}
	std::string want = quackmail::util::Lower(query);
	std::vector<std::string> out;
	for (auto &a : all) {
		if (quackmail::util::Lower(a).find(want) != std::string::npos) {
			out.push_back(a);
		}
	}
	return out;
}

// One row of the address-book panel. A button rather than a link: it has no
// destination, it puts text in a field.
std::string AddressBookRows(const std::vector<std::string> &entries, size_t limit) {
	if (entries.empty()) {
		return "<li class=\"muted\">—</li>";
	}
	std::string out;
	size_t n = 0;
	for (auto &a : entries) {
		if (++n > limit) {
			out += "<li class=\"muted\">…</li>";
			break;
		}
		out += "<li><button type=\"button\" data-addr=\"" + A(a) + "\">" + T(a) + "</button></li>";
	}
	return out;
}

// The searchable half of the address book, as an htmx fragment. Server-side,
// because the whole book used to be inlined into every compose page and a book
// of any size made compose the heaviest page in the application.
void GetAddressBook(Ctx &ctx) {
	SecurityHeaders(ctx);
	ctx.resp.Html(AddressBookRows(AddressBook(ctx, ctx.req.Param("q")), 50));
}

// The compose form itself, without the shell. Rendered on its own for an htmx
// swap into the reading pane, and inside a page for everyone else.
std::string ComposeForm(Ctx &ctx, const ComposeState &st) {
	// data-compose is what qc-compose.js looks for. Without it — or with
	// JavaScript off — this stays exactly the plain-text form it was, which is
	// why the textarea is the field the server actually reads for the text half.
	//
	// The data-msg-* attributes are how the script gets translated strings: it
	// is one static asset shared by every locale, so anything it says out loud
	// has to be handed to it by the page that knows which locale this is.
	std::string body = "<form method=\"post\" action=\"/mail/send\" enctype=\"multipart/form-data\" "
	                   "data-compose data-msg-saved=\"" +
	                   A(Tr(ctx, "compose.saved")) + "\" data-msg-unsaved=\"" +
	                   A(Tr(ctx, "compose.unsaved")) + "\" data-msg-toobig=\"" +
	                   A(Tr(ctx, "compose.attach_toobig")) + "\" data-msg-remove=\"" +
	                   A(Tr(ctx, "compose.attach_remove")) + "\">";
	body += Hidden("_csrf", ctx.csrf);
	body += Hidden("in_reply_to", st.in_reply_to);
	body += Hidden("references", st.references);
	body += Hidden("draft_of", std::to_string(st.draft_of));
	if (st.fwd_msg > 0) {
		body += Hidden("fwd_room", std::to_string(st.fwd_room));
		body += Hidden("fwd_msg", std::to_string(st.fwd_msg));
	}
	// Filled in by the editor on submit; empty means "this message is plain
	// text", which is the state of every submission from a browser without it.
	body += Hidden("html_body", "");

	// There is one identity per account today (SenderAddress), so this is shown
	// rather than chosen. Showing it still matters: it is the address the
	// recipient will reply to.
	body += "<label class=\"field\"><span>" + T(Tr(ctx, "compose.from")) + "</span>" +
	        "<output class=\"fromline\">" + T(SenderAddress(ctx)) + "</output></label>";

	// A <datalist> works with no script at all — the browser's own autocomplete
	// offers a match as you type, and the value it inserts is a complete
	// "Name <addr>" rather than the bare address this used to offer. Capped,
	// because it is inlined; the panel below is the searchable one.
	auto addresses = AddressBook(ctx, "");
	if (!addresses.empty()) {
		body += "<datalist id=\"addressbook\">";
		size_t n = 0;
		for (auto &a : addresses) {
			if (++n > 100) {
				break;
			}
			body += "<option value=\"" + A(a) + "\">";
		}
		body += "</datalist>";
	}

	struct Field {
		const char *name;
		const char *id;
		const char *key;
		const std::string *value;
	};
	const Field fields[] = {
	    {"to", "compose-to", "compose.to", &st.to},
	    {"cc", "compose-cc", "compose.cc", &st.cc},
	    {"bcc", "compose-bcc", "compose.bcc", &st.bcc},
	};
	for (auto &f : fields) {
		body += "<label class=\"field recipients\"><span>" + T(Tr(ctx, f.key)) +
		        "</span><input type=\"text\" name=\"" + f.name + "\" id=\"" + f.id + "\" value=\"" +
		        A(*f.value) + "\" list=\"addressbook\" autocomplete=\"off\"></label>";
	}
	body += "<p class=\"muted small\">" + T(Tr(ctx, "compose.bcc_note")) + "</p>";

	body += "<label class=\"field\"><span>" + T(Tr(ctx, "compose.subject")) + "</span>" +
	        TextInput("subject", st.subject) + "</label>";
	body += "<label class=\"field\"><span>" + T(Tr(ctx, "compose.message")) + "</span>" +
	        TextArea("body", st.text, 18) + "</label>";
	// Hidden until the editor loads and marks it available: offering "formatted
	// text" to someone who will only ever get a textarea would be a lie. Whether
	// it starts on is the user's preference, read here so the server and the
	// script cannot disagree about it.
	bool rich = quackmail::citadel::GetUserPref(ctx.con, ctx.username, "web_compose_rich", "1") != "0";
	body += "<label class=\"chk richopt\"><input type=\"checkbox\" name=\"rich\" value=\"1\"" +
	        std::string(rich ? " checked" : "") + "> " + T(Tr(ctx, "compose.formatted_text")) + "</label>";

	body += "<label class=\"field\"><span>" + T(Tr(ctx, "compose.attachment")) +
	        "</span><input type=\"file\" name=\"attachment\" multiple></label>";
	// The ceiling is the whole request body, not one file, and exceeding it is a
	// connection-level rejection that loses everything typed — so it is stated
	// up front and checked in the browser before the POST rather than
	// discovered afterwards.
	body += "<ul class=\"attachlist\" id=\"attachlist\" hidden></ul>";
	body += "<p class=\"muted small\" data-maxbody=\"" +
	        std::to_string((int64_t)http::Limits().max_body) + "\">" +
	        T(Tr(ctx, "compose.attach_limit")) + "</p>";

	if (st.fwd_msg > 0) {
		body += "<p class=\"muted small\">" + T(Tr(ctx, "compose.forward_attached")) + "</p>";
	}

	body += "<div class=\"toolbar\">" + IconButton(Tr(ctx, "compose.send"), "send") + " ";
	body += "<button class=\"secondary\" name=\"draft\" value=\"1\">" + T(Tr(ctx, "compose.save_draft")) +
	        "</button> ";
	body += "<span class=\"spacer\"></span>";
	body += "<span class=\"muted small\" id=\"compose-status\" role=\"status\"></span> ";
	body += Link("/mail/", Tr(ctx, "compose.cancel"), "btn sec") + "</div>";
	body += "</form>";

	// Outside the form on purpose: an <input> in here would ride along with
	// every send, and Enter in it would submit the message.
	if (!addresses.empty()) {
		body += "<div class=\"addressbook\">";
		body += "<button type=\"button\" class=\"secondary\" id=\"addressbook-toggle\">" +
		        T(Tr(ctx, "compose.address_book")) + "</button>";
		body += "<div id=\"addressbook-panel\" class=\"addressbook-panel\" hidden>";
		body += "<label class=\"vh\" for=\"addressbook-q\">" + T(Tr(ctx, "compose.search")) + "</label>";
		body += "<input type=\"search\" id=\"addressbook-q\" name=\"q\" placeholder=\"" +
		        A(Tr(ctx, "compose.search")) + "\" hx-get=\"/mail/addressbook\" "
		        "hx-trigger=\"input changed delay:200ms, search\" hx-target=\"#addressbook-results\">";
		body += "<ul id=\"addressbook-results\">" + AddressBookRows(addresses, 50) + "</ul>";
		body += "</div></div>";
	}
	return body;
}

void GetCompose(Ctx &ctx) {
	ComposeState st;
	st.to = ctx.req.Param("to");
	st.cc = ctx.req.Param("cc");
	st.bcc = ctx.req.Param("bcc");
	st.subject = ctx.req.Param("subject");

	bool seeded = LoadDraft(ctx, ctx.ParamInt("draft", 0), st);

	int64_t src_room = ctx.ParamInt("room", -1);
	int64_t reply = ctx.ParamInt("reply", 0);
	int64_t forward = ctx.ParamInt("forward", 0);
	bool reply_all = ctx.req.Param("all") == "1";
	if (!seeded && src_room >= 0 && (reply > 0 || forward > 0)) {
		Room room;
		Message msg;
		if (ResolveRoomNumFor(ctx, src_room, room) && LoadMessageIn(ctx, room, reply > 0 ? reply : forward, msg)) {
			std::string node = ConfigStr(ctx.con, "c_nodename", "quackcit");
			auto parsed = quackmail::mime::Parse(msg.raw);
			std::string subj = DecodeHeader(msg.subject);
			if (reply > 0) {
				std::set<std::string> seen;
				// Never write back to yourself. `seen` then carries into Cc, so
				// an address on both the original To and Cc appears once.
				seen.insert(quackmail::util::Lower(SenderAddress(ctx)));
				seen.insert(quackmail::util::Lower(ctx.username));

				std::string from = parsed.from.empty() ? msg.author : parsed.from;
				std::vector<std::string> to_values = {from};
				if (reply_all) {
					// Everyone the message was addressed to, not just the sender.
					// Dropping the original To is what made reply-all reply to
					// one person.
					to_values.push_back(HeaderOf(parsed, "to"));
				}
				st.to = MergeAddresses(to_values, seen);
				if (st.to.empty()) {
					st.to = msg.author;
				}
				if (reply_all) {
					st.cc = MergeAddresses({HeaderOf(parsed, "cc")}, seen);
				}
				st.subject = PrefixSubject(subj, true);
				st.in_reply_to = parsed.message_id.empty() ? quackmail::citadel::MessageId(msg, node)
				                                           : parsed.message_id;
				// The whole chain, not just the parent. RFC 5322 §3.6.4: a reply
				// carries the parent's References plus the parent's Message-ID.
				// Setting it to the parent alone breaks threading for every
				// recipient from the second message on.
				std::string parent_refs = HeaderOf(parsed, "references");
				if (parent_refs.empty()) {
					parent_refs = msg.references;
				}
				st.references = parent_refs.empty() ? st.in_reply_to : parent_refs + " " + st.in_reply_to;
			} else {
				st.subject = PrefixSubject(subj, false);
				// Carried as a real message/rfc822 attachment on send, so the
				// original's own attachments survive being forwarded. The quoted
				// text below is still there for a client that will not open it.
				st.fwd_room = room.room_num;
				st.fwd_msg = msg.msgnum;
			}
			st.text = QuoteBody(msg, subj, reply > 0) + SignatureFor(ctx);
		}
	} else if (!seeded) {
		st.text = SignatureFor(ctx);
	}

	std::string form = ComposeForm(ctx, st);

	// An htmx request wants the compose form and nothing else — that is what
	// docks it in the reading pane instead of throwing the mailbox away. Every
	// other client, including the test suite, announces nothing and gets the
	// whole page, so /mail/compose?to=… stays exactly as linkable as it reads.
	if (ctx.req.HasHeader("HX-Request")) {
		SecurityHeaders(ctx);
		ctx.resp.Html("<div class=\"compose\">" + form + "</div>");
		return;
	}

	PageOpts opts;
	opts.active = "compose";
	opts.script = "qc-compose.js";
	Render(ctx, Tr(ctx, "compose.title"), "<div class=\"compose\">" + form + "</div>", opts);
}

// The fields of a composed message, on their way into an RFC822 body.
struct ComposeFields {
	std::string to, cc, bcc, subject, text, html, in_reply_to, references;
	// The forwarded original, verbatim, or empty.
	std::string forwarded;
	std::string forwarded_subject;
};

// Build the RFC822 message that gets signed, delivered and filed.
//
// The nesting is core's job now (mime::BuildMessage): this only decides *which*
// parts exist. An HTML body arrives already through the compose allow-list, and
// any data: image in it becomes a real inline part so the message is
// self-contained rather than pointing at anything remote.
//
// Bcc is deliberately *not* a header here. It is a delivery instruction, and a
// copy of it on the wire would tell every recipient who the blind ones were;
// PostSend adds it to the sender's own filed copy afterwards, which is the only
// place it belongs.
std::string BuildMessage(Ctx &ctx, const std::string &from, const ComposeFields &f,
                         const std::vector<http::FormFile> &files, std::string &message_id) {
	std::string fqdn = ConfigStr(ctx.con, "c_fqdn", "quackmail.test");
	int64_t now = (int64_t)std::time(nullptr);
	message_id = "<" + quackmail::util::RandomHex(12) + "." + std::to_string(now) + "@" + fqdn + ">";

	quackmail::mime::HeaderList headers;
	headers.push_back({"Message-ID", message_id});
	headers.push_back({"Date", RfcDate(now)});
	headers.push_back({"From", from});
	headers.push_back({"To", f.to});
	if (!f.cc.empty()) {
		headers.push_back({"Cc", f.cc});
	}
	headers.push_back({"Subject", f.subject});
	if (!f.in_reply_to.empty()) {
		headers.push_back({"In-Reply-To", f.in_reply_to});
	}
	if (!f.references.empty()) {
		headers.push_back({"References", f.references});
	}

	std::vector<quackmail::mime::BuildPart> parts;

	std::string html_out = f.html;
	std::vector<quackmail::mime::BuildPart> inline_parts;
	if (!html_out.empty()) {
		// A data: image the editor inserted becomes a cid: part. Left as a data:
		// URI it would still work, but base64 inside the HTML bloats the body and
		// several clients refuse to render it.
		html_out = ExtractDataImages(fqdn, html_out, inline_parts);
	}

	quackmail::mime::BuildPart text_part;
	text_part.content_type = "text/plain";
	text_part.content = f.text;
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

	if (!f.forwarded.empty()) {
		quackmail::mime::BuildPart fwd;
		fwd.content_type = "message/rfc822";
		fwd.content = f.forwarded;
		// A filename is what makes BuildMessage treat it as an attachment; .eml
		// is what every client offers to open it as.
		fwd.filename = (f.forwarded_subject.empty() ? std::string("forwarded") : f.forwarded_subject) + ".eml";
		fwd.disposition = "attachment";
		parts.push_back(fwd);
	}

	for (auto &file : files) {
		if (file.content.empty()) {
			continue;
		}
		quackmail::mime::BuildPart att;
		att.content_type = file.content_type.empty() ? "application/octet-stream" : file.content_type;
		att.content = file.content;
		att.filename = http::SanitizeFilename(file.filename);
		att.disposition = "attachment";
		parts.push_back(att);
	}

	return quackmail::mime::BuildMessage(headers, parts);
}

// Everything a compose POST carries, parsed once. Both /mail/send and the
// autosave route read the same shape, so neither can drift from the form.
struct ComposePost {
	std::vector<std::pair<std::string, std::string>> fields;
	std::vector<http::FormFile> files;
	std::string Field(const char *name) const {
		for (auto &f : fields) {
			if (f.first == name) {
				return f.second;
			}
		}
		return std::string();
	}
	int64_t Num(const char *name) const {
		std::string v = Field(name);
		return v.empty() ? 0 : (int64_t)std::strtoll(v.c_str(), nullptr, 10);
	}
};

// Parse and authorize a compose POST. Multipart bodies are parsed here, not by
// the router, so the CSRF token has to be re-checked against the parsed fields.
bool ReadComposePost(Ctx &ctx, ComposePost &out) {
	std::string ct = ctx.req.Header("Content-Type");
	if (!http::ParseMultipart(ct, ctx.req.body, out.fields, out.files)) {
		BadRequest(ctx, "The compose form must be submitted as multipart/form-data.");
		return false;
	}
	if (!quackmail::web::CheckCsrf(ctx.con, ctx.session_hash, out.Field("_csrf"))) {
		Forbidden(ctx, "This form has expired. Go back, reload the page and try again.");
		return false;
	}
	return true;
}

// Turn the posted fields into the message body, resolving the HTML half and the
// forwarded original. Returns false only when it has already answered.
bool ComposeBodyFrom(Ctx &ctx, const ComposePost &post, ComposeFields &f, std::string &body,
                     std::string &message_id) {
	f.to = post.Field("to");
	f.cc = post.Field("cc");
	f.bcc = post.Field("bcc");
	f.subject = post.Field("subject");
	f.text = post.Field("body");

	// The HTML half, if the composer sent one. Sanitized *here*, before the
	// message is built — what gets stored is already safe, rather than relying on
	// every future reader to clean it again. See html_sanitize.hpp on why this is
	// an allow-list and the display path is not.
	f.html = post.Field("html_body");
	if (!f.html.empty()) {
		f.html = quackmail::html::SanitizeForCompose(f.html);
		// A plain-text alternative is not optional: a recipient on a text-only
		// client, or reading over Citadel or POP3, gets this one. If the composer
		// did not send text of its own, derive it rather than sending an empty
		// part.
		if (f.text.empty()) {
			f.text = quackmail::html::ToPlainText(f.html);
		}
	}

	// The forwarded original is named by the form, so it is re-resolved and
	// re-checked here rather than trusted: LoadMessageIn is what confirms this
	// user may read that number in that room, exactly as the reader does.
	int64_t fwd_room = post.Num("fwd_room");
	int64_t fwd_msg = post.Num("fwd_msg");
	if (fwd_msg > 0) {
		Room room;
		Message orig;
		if (ResolveRoomNumFor(ctx, fwd_room, room) && LoadMessageIn(ctx, room, fwd_msg, orig)) {
			f.forwarded = quackmail::citadel::RenderRfc822(
			    orig, ConfigStr(ctx.con, "c_nodename", "quackcit"));
			f.forwarded_subject = http::SanitizeFilename(DecodeHeader(orig.subject));
		}
	}

	f.in_reply_to = post.Field("in_reply_to");
	f.references = post.Field("references");
	body = BuildMessage(ctx, SenderAddress(ctx), f, post.files, message_id);
	return true;
}

// File a draft, replacing the one it was resumed from. Returns the new msgnum,
// or 0 when the store refused it.
int64_t StoreDraft(Ctx &ctx, const ComposeFields &f, const std::string &body, int64_t replaces) {
	int64_t drafts = quackmail::citadel::GetOrCreateUserRoom(ctx.con, ctx.username, "Drafts");
	Message msg;
	msg.author = ctx.username;
	msg.author_usernum = quackmail::citadel::GetOrAssignUserNum(ctx.con, ctx.username);
	msg.recipient = f.to;
	msg.msgtime = (int64_t)std::time(nullptr);
	msg.format_type = 4;
	msg.subject = f.subject;
	msg.origin_room = "Drafts";
	msg.node = ConfigStr(ctx.con, "c_nodename", "quackcit");
	// Bcc *is* stored on a draft: it is the user's own copy, and losing the
	// blind recipients every time a draft is saved would be worse than keeping
	// them somewhere only the author can read.
	msg.raw = f.bcc.empty() ? body : "Bcc: " + HeaderSafe(f.bcc) + "\r\n" + body;
	std::string err;
	std::vector<int64_t> rooms = {drafts};
	int64_t msgnum = quackmail::citadel::InsertMessage(ctx.con, msg, rooms, err);
	if (msgnum > 0 && replaces > 0 && replaces != msgnum) {
		// Only after the replacement is safely stored, and only from the room it
		// was resumed from — a failed insert must leave the old draft alone.
		Room drafts_room;
		Message old;
		if (ResolveRoomFor(ctx, "Drafts", drafts_room) &&
		    LoadMessageIn(ctx, drafts_room, replaces, old)) {
			std::string unlink_err;
			quackmail::citadel::DeleteMessage(ctx.con, drafts_room.room_num, replaces, unlink_err);
		}
	}
	return msgnum;
}

// Autosave. Answers with the draft's number so the form can keep replacing the
// same one instead of leaving a trail of them, and never redirects — this is
// only ever called by script.
void PostDraft(Ctx &ctx) {
	ComposePost post;
	if (!ReadComposePost(ctx, post)) {
		return;
	}
	ComposeFields f;
	std::string body, message_id;
	if (!ComposeBodyFrom(ctx, post, f, body, message_id)) {
		return;
	}
	if (quackmail::quota::WouldExceed(ctx.con, ctx.username, (int64_t)body.size())) {
		ctx.resp.Text("0", 507);
		return;
	}
	int64_t msgnum = StoreDraft(ctx, f, body, post.Num("draft_of"));
	SecurityHeaders(ctx);
	ctx.resp.Text(std::to_string(msgnum));
}

void PostSend(Ctx &ctx) {
	ComposePost post;
	if (!ReadComposePost(ctx, post)) {
		return;
	}

	ComposeFields f;
	std::string body, message_id;
	if (!ComposeBodyFrom(ctx, post, f, body, message_id)) {
		return;
	}
	bool draft = !post.Field("draft").empty();

	auto rcpts = SplitAddresses(f.to);
	for (auto &a : SplitAddresses(f.cc)) {
		rcpts.push_back(a);
	}
	// Blind recipients are envelope-only: they receive the message, and nothing
	// on the wire says they did.
	for (auto &a : SplitAddresses(f.bcc)) {
		rcpts.push_back(a);
	}
	if (!draft && rcpts.empty()) {
		BadRequest(ctx, "A message needs at least one recipient.");
		return;
	}

	std::string from = SenderAddress(ctx);

	if (draft) {
		// A draft is a pure store operation with no send quota beside it, so the
		// storage quota is the only limit it answers to.
		if (quackmail::quota::WouldExceed(ctx.con, ctx.username, (int64_t)body.size())) {
			ErrorPage(ctx, 507, "Mailbox full",
			          "There is not enough room left in your mailbox to save this draft.");
			return;
		}
		StoreDraft(ctx, f, body, post.Num("draft_of"));
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

	// And the storage quota, because sending also files a copy into Sent Items.
	// 507 rather than 429: the rate limiter already owns 429, and the two have
	// to read differently in a log.
	//
	// Checked here, before the message goes out. The Sent Items copy further
	// down is deliberately *not* checked — it is covered by the backstop inside
	// InsertMessage, and refusing the sender's own copy after the mail has
	// already left would be worse than storing it.
	if (quackmail::quota::WouldExceed(ctx.con, ctx.username, (int64_t)body.size())) {
		ErrorPage(ctx, 507, "Mailbox full",
		          "There is not enough room left in your mailbox to send this message.");
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
	copy.recipient = f.to;
	copy.msgtime = (int64_t)std::time(nullptr);
	copy.format_type = 4;
	copy.subject = f.subject;
	copy.origin_room = "Sent Items";
	copy.node = ConfigStr(ctx.con, "c_nodename", "quackcit");
	// The Bcc list goes on the filed copy and nowhere else, so the sender can
	// still see who it went to. It is prepended rather than built in, so the
	// bytes that were signed and sent are byte-identical to the bytes that
	// left — and the DKIM h= list never names Bcc, so the signature on this copy
	// still verifies.
	copy.raw = f.bcc.empty() ? body : "Bcc: " + HeaderSafe(f.bcc) + "\r\n" + body;
	std::string copy_err;
	std::vector<int64_t> rooms = {sent};
	quackmail::citadel::InsertMessage(ctx.con, copy, rooms, copy_err);

	// A sent draft is no longer a draft.
	int64_t was_draft = post.Num("draft_of");
	if (was_draft > 0) {
		Room drafts;
		Message old;
		if (ResolveRoomFor(ctx, "Drafts", drafts) && LoadMessageIn(ctx, drafts, was_draft, old)) {
			std::string unlink_err;
			quackmail::citadel::DeleteMessage(ctx.con, drafts.room_num, was_draft, unlink_err);
		}
	}

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
	out.push_back({"GET", "/mail/addressbook", Role::User, GetAddressBook});
	out.push_back({"POST", "/mail/send", Role::User, PostSend});
	out.push_back({"POST", "/mail/draft", Role::User, PostDraft});
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
