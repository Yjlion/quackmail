#include "jmap.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include "quackmail/citadel_msg.hpp"
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

namespace mime = quackmail::mime;

// JMAP dates are RFC 3339 in UTC and nothing else — not the RFC 5322 form
// util::RfcDate produces and not the human one FormatTimeIn does, either of
// which a client will simply fail to parse.
std::string Utc8601(int64_t epoch) {
	time_t t = (time_t)epoch;
	struct tm tm_utc;
#ifdef _WIN32
	gmtime_s(&tm_utc, &t);
#else
	gmtime_r(&t, &tm_utc);
#endif
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm_utc.tm_year + 1900,
	              tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
	return buf;
}

// The inverse, for the `before` and `after` conditions of an Email/query
// filter. Only the UTC form JMAP defines is accepted; an offset would be a
// second timezone implementation for no caller that sends one.
bool ParseUtc8601(const std::string &v, int64_t &out) {
	if (v.size() < 20 || v[4] != '-' || v[7] != '-' || v[10] != 'T' || v[13] != ':' || v[16] != ':') {
		return false;
	}
	auto num = [&](size_t at, size_t len) {
		return (int)std::strtol(v.substr(at, len).c_str(), nullptr, 10);
	};
	struct tm tm_v;
	std::memset(&tm_v, 0, sizeof(tm_v));
	tm_v.tm_year = num(0, 4) - 1900;
	tm_v.tm_mon = num(5, 2) - 1;
	tm_v.tm_mday = num(8, 2);
	tm_v.tm_hour = num(11, 2);
	tm_v.tm_min = num(14, 2);
	tm_v.tm_sec = num(17, 2);
	if (tm_v.tm_mon < 0 || tm_v.tm_mon > 11 || tm_v.tm_mday < 1 || tm_v.tm_mday > 31) {
		return false;
	}
#ifdef _WIN32
	out = (int64_t)_mkgmtime(&tm_v);
#else
	out = (int64_t)timegm(&tm_v);
#endif
	return true;
}

// ---- keywords ------------------------------------------------------------
//
// IMAP and JMAP name the same four system flags differently. The store keeps
// the IMAP spelling because IMAP wrote them first; translating on the way out
// is what stops the two front-ends from drifting into two different truths
// about whether a message has been read.
struct FlagMap {
	const char *imap;
	const char *jmap;
};

const FlagMap kFlagMap[] = {
    {"\\Seen", "$seen"},
    {"\\Flagged", "$flagged"},
    {"\\Answered", "$answered"},
    {"\\Draft", "$draft"},
    {"\\Deleted", "$deleted"},
};

std::string JmapKeywordFor(const std::string &imap_flag) {
	for (const auto &m : kFlagMap) {
		if (imap_flag == m.imap) {
			return m.jmap;
		}
	}
	// A user-defined IMAP keyword is already a bare word and is its own JMAP
	// keyword, lower-cased because JMAP keywords are case-insensitive and
	// clients compare them literally.
	return quackmail::util::Lower(imap_flag);
}

std::string PreviewOf(const std::string &text) {
	std::string out;
	bool space = false;
	for (char c : text) {
		if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
			space = true;
			continue;
		}
		if (space && !out.empty()) {
			out += ' ';
		}
		space = false;
		out += c;
		if (out.size() >= 256) {
			break;
		}
	}
	return out;
}

js::Value AddressList(const std::string &header) {
	if (header.empty()) {
		return js::Value();
	}
	js::Value arr = js::Value::MakeArray();
	for (const auto &a : mime::ParseAddressList(header)) {
		js::Value one = js::Value::MakeObject();
		std::string name = mime::DecodeEncodedWords(a.name);
		if (name.empty()) {
			one.Set("name", js::Value());
		} else {
			one.Set("name", name);
		}
		one.Set("email", a.addr);
		arr.Push(one);
	}
	return arr.Size() ? arr : js::Value();
}

// The header value, or a JSON null. JMAP distinguishes "absent" from "empty",
// and a client uses the difference when deciding whether to show a field.
js::Value HeaderOrNull(const mime::MimeEntity &e, const char *name) {
	for (const auto &h : e.headers) {
		if (quackmail::util::Lower(h.first) == quackmail::util::Lower(name)) {
			return js::Value::MakeString(mime::DecodeEncodedWords(h.second));
		}
	}
	return js::Value();
}

// A whitespace-separated list header (References), as JMAP's array of ids.
js::Value IdListHeader(const mime::MimeEntity &e, const char *name) {
	js::Value raw = HeaderOrNull(e, name);
	if (raw.IsNull()) {
		return js::Value();
	}
	js::Value arr = js::Value::MakeArray();
	std::string cur;
	for (char c : raw.str) {
		if (c == '<') {
			cur.clear();
		} else if (c == '>') {
			if (!cur.empty()) {
				arr.Push(js::Value::MakeString(cur));
			}
			cur.clear();
		} else {
			cur += c;
		}
	}
	return arr.Size() ? arr : js::Value();
}

bool WantsProp(const js::Value &properties, const char *name) {
	// An absent or null `properties` means the default set, which for our
	// purposes is everything — a client that wants less says so.
	if (properties.type != js::Value::Array) {
		return true;
	}
	for (size_t i = 0; i < properties.Size(); i++) {
		if (properties.At(i).AsString() == name) {
			return true;
		}
	}
	return false;
}

// The parts of a message, split into the two lists JMAP asks for plus the
// attachments. `bodyStructure` is deliberately not produced: it is the one
// Email property whose shape mirrors MIME exactly, and nothing needs it that
// textBody/htmlBody/attachments does not already answer.
struct Bodies {
	std::vector<mime::MimePart> text;
	std::vector<mime::MimePart> html;
	std::vector<mime::MimePart> attachments;
};

Bodies SplitBodies(const mime::MimeEntity &root) {
	Bodies b;
	for (const auto &part : mime::FlattenParts(root)) {
		std::string type = quackmail::util::Lower(part.content_type);
		bool named = !part.filename.empty();
		if (!named && type == "text/plain") {
			b.text.push_back(part);
		} else if (!named && type == "text/html") {
			b.html.push_back(part);
		} else {
			b.attachments.push_back(part);
		}
	}
	return b;
}

js::Value BodyPartJson(const mime::MimePart &part, int64_t msgnum, size_t index) {
	js::Value v = js::Value::MakeObject();
	v.Set("partId", part.section.empty() ? std::to_string(index) : part.section);
	// A blobId that names the message and the section, so download can find the
	// part again without the client having to hold anything else.
	v.Set("blobId", IdOf(msgnum) + "." + (part.section.empty() ? std::to_string(index) : part.section));
	v.Set("size", (int64_t)part.content.size());
	v.Set("type", part.content_type.empty() ? std::string("application/octet-stream") : part.content_type);
	if (part.charset.empty()) {
		v.Set("charset", js::Value());
	} else {
		v.Set("charset", part.charset);
	}
	if (part.filename.empty()) {
		v.Set("name", js::Value());
		v.Set("disposition", js::Value());
	} else {
		v.Set("name", mime::DecodeEncodedWords(part.filename));
		v.Set("disposition", "attachment");
	}
	v.Set("cid", js::Value());
	return v;
}

} // namespace

// ---- shared helpers declared in jmap.hpp ---------------------------------

std::vector<Room> JmapMailboxes(Ctx &ctx) {
	std::vector<Room> out;
	for (auto &room : quackmail::citadel::ListRooms(ctx.con, ctx.username, -1, "all")) {
		// A passworded room the user has not unlocked stays out: JMAP has no way
		// to carry a room password, so listing it would only produce a mailbox
		// whose every message is unreadable.
		if (!quackmail::citadel::RoomUnlocked(ctx.con, ctx.username, room)) {
			continue;
		}
		out.push_back(room);
	}
	return out;
}

bool ResolveMailbox(Ctx &ctx, const std::string &id, Room &out) {
	int64_t num = IdNum(id);
	if (num < 0) {
		return false;
	}
	Room room;
	// ResolveRoomNumFor applies the visibility rules GetRoomByNum does not.
	if (!ResolveRoomNumFor(ctx, num, room)) {
		return false;
	}
	if (!quackmail::citadel::RoomUnlocked(ctx.con, ctx.username, room)) {
		return false;
	}
	out = room;
	return true;
}

std::string MailboxRole(Ctx &ctx, const Room &room) {
	(void)ctx;
	// Only a personal mail room has a role: a public BBS room named "Drafts"
	// is not this user's drafts folder, and telling a client otherwise would
	// have it file outgoing mail into a room everyone can read.
	if (room.mailbox_owner == 0) {
		return std::string();
	}
	std::string name = quackmail::util::Lower(room.display_name);
	if (name == "mail") {
		return "inbox";
	}
	if (name == "sent items") {
		return "sent";
	}
	if (name == "drafts") {
		return "drafts";
	}
	if (name == "trash") {
		return "trash";
	}
	return std::string();
}

std::vector<std::string> KeywordsFor(Ctx &ctx, int64_t msgnum) {
	std::vector<std::string> out;
	auto r = Exec(ctx.con, "SELECT flag FROM citadel_msg_flags WHERE msgnum = $1 AND username = $2",
	              {Value::BIGINT(msgnum), Value(ctx.username)});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		out.push_back(JmapKeywordFor(mat.GetValue(0, i).ToString()));
	}
	return out;
}

std::string ImapFlagFor(const std::string &keyword) {
	std::string lower = quackmail::util::Lower(keyword);
	for (const auto &m : kFlagMap) {
		if (lower == m.jmap) {
			return m.imap;
		}
	}
	return std::string();
}

void SetKeyword(Ctx &ctx, int64_t msgnum, const std::string &keyword, bool on) {
	std::string flag = ImapFlagFor(keyword);
	if (flag.empty()) {
		flag = keyword; // a user-defined keyword is stored as itself
	}
	if (!on) {
		Exec(ctx.con, "DELETE FROM citadel_msg_flags WHERE msgnum = $1 AND username = $2 AND flag = $3",
		     {Value::BIGINT(msgnum), Value(ctx.username), Value(flag)});
		return;
	}
	// Checked rather than upserted: citadel_msg_flags has no unique constraint,
	// so an unconditional insert would let one keyword accumulate rows.
	auto existing =
	    Exec(ctx.con, "SELECT 1 FROM citadel_msg_flags WHERE msgnum = $1 AND username = $2 AND flag = $3",
	         {Value::BIGINT(msgnum), Value(ctx.username), Value(flag)});
	if (existing && existing->Cast<MaterializedQueryResult>().RowCount() > 0) {
		return;
	}
	Exec(ctx.con, "INSERT INTO citadel_msg_flags (msgnum, username, flag) VALUES ($1, $2, $3)",
	     {Value::BIGINT(msgnum), Value(ctx.username), Value(flag)});
}

std::string ThreadIdFor(const Message &msg, const std::string &node) {
	// A reply joins the thread its root started. References holds that root
	// first — GetBbsCompose builds it as `orig.references + " " + MessageId(orig)`,
	// so the first entry is the root's Message-ID however deep the reply is — and
	// the thread id is a function of the headers rather than of anything we
	// store, which is what makes it stable across a restart and identical for two
	// copies of one message in different rooms.
	//
	// The root has to hash *its own Message-ID* for this to close: naming it
	// after its msgnum instead (which is what this did until the reading pane
	// needed it to group) gives a root an id no reply can ever produce, so every
	// conversation came out as a root alone plus its replies in a thread of their
	// own.
	const std::string &refs = msg.references;
	size_t open = refs.find('<');
	if (open != std::string::npos) {
		size_t close = refs.find('>', open);
		if (close != std::string::npos && close > open + 1) {
			return "T" + quackmail::util::Sha256Hex(refs.substr(open + 1, close - open - 1)).substr(0, 16);
		}
	}
	// No references: the root of a thread, named by the same Message-ID its
	// replies will point back at.
	std::string id = quackmail::citadel::MessageId(msg, node);
	if (id.size() > 2 && id.front() == '<' && id.back() == '>') {
		id = id.substr(1, id.size() - 2);
	}
	return "T" + quackmail::util::Sha256Hex(id).substr(0, 16);
}

// The node name the Message-IDs above are minted under. One lookup per listing
// rather than per message, which is why callers hold it.
std::string NodeName(Ctx &ctx) {
	return ConfigStr(ctx.con, "c_nodename", "quackcit");
}

std::vector<JmapEmail> ListEmails(Ctx &ctx, int64_t only_room) {
	std::vector<JmapEmail> out;
	std::string rooms;
	for (const auto &room : JmapMailboxes(ctx)) {
		if (only_room >= 0 && room.room_num != only_room) {
			continue;
		}
		if (!rooms.empty()) {
			rooms += ",";
		}
		// Inlined rather than bound: these are room numbers this function just
		// read out of the store, never anything a client supplied.
		rooms += std::to_string(room.room_num);
	}
	if (rooms.empty()) {
		return out;
	}
	// Newest first, which is Email/query's default sort and the order every
	// client shows a mailbox in. msgnum breaks a tie, so the order is total and
	// a paged query cannot show one message twice.
	auto r = Exec(ctx.con,
	              "SELECT m.msgnum, rm.room_num, m.msgtime "
	              "FROM citadel_room_msgs rm JOIN citadel_messages m ON m.msgnum = rm.msgnum "
	              "WHERE rm.room_num IN (" + rooms + ") "
	              "ORDER BY m.msgtime DESC, m.msgnum DESC",
	              {});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (idx_t i = 0; i < mat.RowCount(); i++) {
		JmapEmail e;
		e.msgnum = mat.GetValue(0, i).GetValue<int64_t>();
		e.room_num = mat.GetValue(1, i).GetValue<int64_t>();
		Value t = mat.GetValue(2, i);
		e.received = t.IsNull() ? 0 : t.GetValue<int64_t>();
		out.push_back(e);
	}
	return out;
}

js::Value EmailToJson(Ctx &ctx, const Message &msg, int64_t room_num, const js::Value &properties,
                      const js::Value &body_properties, bool fetch_text, bool fetch_html) {
	(void)body_properties;
	// The RFC822 *view*, never msg.raw: a native Citadel message has no headers
	// of its own, and RenderRfc822 is what synthesizes them. IMAP made the same
	// choice for the same reason, and two front-ends disagreeing about what a
	// message looks like is exactly what that rule prevents.
	std::string node = quackmail::citadel::GetConfig(ctx.con, "c_nodename", "quackcit");
	std::string rfc822 = quackmail::citadel::RenderRfc822(msg, node);
	mime::MimeEntity entity = mime::ParseEntity(rfc822);
	Bodies bodies = SplitBodies(entity);

	js::Value v = js::Value::MakeObject();
	v.Set("id", IdOf(msg.msgnum));

	if (WantsProp(properties, "blobId")) {
		v.Set("blobId", IdOf(msg.msgnum));
	}
	if (WantsProp(properties, "threadId")) {
		v.Set("threadId", ThreadIdFor(msg, node));
	}
	if (WantsProp(properties, "mailboxIds")) {
		// Every room this message is pointed into *and* this user may see. A
		// message shared into a room they cannot read must not be listed here.
		js::Value ids = js::Value::MakeObject();
		for (const auto &room : JmapMailboxes(ctx)) {
			if (quackmail::citadel::MessageInRoom(ctx.con, room.room_num, msg.msgnum)) {
				ids.Set(IdOf(room.room_num), true);
			}
		}
		if (ids.members.empty() && room_num >= 0) {
			ids.Set(IdOf(room_num), true);
		}
		v.Set("mailboxIds", ids);
	}
	if (WantsProp(properties, "keywords")) {
		js::Value kw = js::Value::MakeObject();
		for (const auto &k : KeywordsFor(ctx, msg.msgnum)) {
			kw.Set(k, true);
		}
		v.Set("keywords", kw);
	}
	if (WantsProp(properties, "size")) {
		v.Set("size", (int64_t)rfc822.size());
	}
	if (WantsProp(properties, "receivedAt")) {
		v.Set("receivedAt", Utc8601(msg.msgtime));
	}
	if (WantsProp(properties, "sentAt")) {
		// The Date header if it parses, the store's own time otherwise. Either
		// way it goes out as RFC 3339: a client cannot be handed the RFC 5322
		// form the header holds.
		int64_t sent = msg.msgtime;
		js::Value date = HeaderOrNull(entity, "Date");
		if (!date.IsNull()) {
			int64_t parsed = 0;
			if (mime::ParseDate(date.str, parsed)) {
				sent = parsed;
			}
		}
		v.Set("sentAt", Utc8601(sent));
	}
	if (WantsProp(properties, "subject")) {
		std::string subject = mime::DecodeEncodedWords(msg.subject);
		v.Set("subject", subject.empty() ? js::Value() : js::Value::MakeString(subject));
	}
	if (WantsProp(properties, "messageId")) {
		v.Set("messageId", IdListHeader(entity, "Message-ID"));
	}
	if (WantsProp(properties, "inReplyTo")) {
		v.Set("inReplyTo", IdListHeader(entity, "In-Reply-To"));
	}
	if (WantsProp(properties, "references")) {
		v.Set("references", IdListHeader(entity, "References"));
	}
	for (const char *field : {"from", "to", "cc", "bcc", "replyTo", "sender"}) {
		if (!WantsProp(properties, field)) {
			continue;
		}
		const char *header = std::string(field) == "replyTo" ? "Reply-To" : field;
		js::Value hv = HeaderOrNull(entity, header);
		v.Set(field, hv.IsNull() ? js::Value() : AddressList(hv.str));
	}
	if (WantsProp(properties, "hasAttachment")) {
		v.Set("hasAttachment", !bodies.attachments.empty());
	}
	if (WantsProp(properties, "preview")) {
		v.Set("preview", PreviewOf(bodies.text.empty() ? std::string() : bodies.text[0].content));
	}

	if (WantsProp(properties, "textBody")) {
		js::Value arr = js::Value::MakeArray();
		for (size_t i = 0; i < bodies.text.size(); i++) {
			arr.Push(BodyPartJson(bodies.text[i], msg.msgnum, i));
		}
		v.Set("textBody", arr);
	}
	if (WantsProp(properties, "htmlBody")) {
		js::Value arr = js::Value::MakeArray();
		for (size_t i = 0; i < bodies.html.size(); i++) {
			arr.Push(BodyPartJson(bodies.html[i], msg.msgnum, i));
		}
		v.Set("htmlBody", arr);
	}
	if (WantsProp(properties, "attachments")) {
		js::Value arr = js::Value::MakeArray();
		for (size_t i = 0; i < bodies.attachments.size(); i++) {
			arr.Push(BodyPartJson(bodies.attachments[i], msg.msgnum, i));
		}
		v.Set("attachments", arr);
	}

	if (fetch_text || fetch_html) {
		js::Value values = js::Value::MakeObject();
		auto add = [&](const std::vector<mime::MimePart> &parts) {
			for (size_t i = 0; i < parts.size(); i++) {
				js::Value bv = js::Value::MakeObject();
				bv.Set("value", parts[i].content);
				// Nothing is truncated and nothing arrives as a problem: the
				// whole part is in memory already, so saying otherwise would be
				// a lie a client would act on.
				bv.Set("isEncodingProblem", false);
				bv.Set("isTruncated", false);
				values.Set(parts[i].section.empty() ? std::to_string(i) : parts[i].section, bv);
			}
		};
		if (fetch_text) {
			add(bodies.text);
		}
		if (fetch_html) {
			add(bodies.html);
		}
		v.Set("bodyValues", values);
	}

	return v;
}

// ---- Mailbox -------------------------------------------------------------

namespace {

js::Value MailboxRights(Ctx &ctx, const Room &room) {
	std::string rights = quackmail::citadel::EffectiveRights(ctx.con, ctx.username, room);
	auto has = [&](char c) { return rights.find(c) != std::string::npos; };
	// CanPost is the one predicate for "may change this room", asked rather
	// than re-derived — the same rule every other front-end follows.
	bool may_post = quackmail::citadel::CanPost(ctx.con, ctx.username, room);
	js::Value r = js::Value::MakeObject();
	r.Set("mayReadItems", has('r'));
	r.Set("mayAddItems", may_post);
	r.Set("mayRemoveItems", may_post && (has('t') || has('e')));
	r.Set("maySetSeen", has('s') || has('r'));
	r.Set("maySetKeywords", has('w') || has('r'));
	r.Set("mayCreateChild", false); // rooms do not nest
	r.Set("mayRename", quackmail::citadel::CanAdminister(ctx.con, ctx.username, room));
	r.Set("mayDelete", quackmail::citadel::CanAdminister(ctx.con, ctx.username, room));
	r.Set("maySubmit", may_post);
	return r;
}

js::Value MailboxToJson(Ctx &ctx, const Room &room, const js::Value &properties) {
	auto stats = quackmail::citadel::GetRoomStats(ctx.con, ctx.username, room.room_num);
	js::Value v = js::Value::MakeObject();
	v.Set("id", IdOf(room.room_num));
	if (WantsProp(properties, "name")) {
		v.Set("name", room.display_name);
	}
	if (WantsProp(properties, "parentId")) {
		v.Set("parentId", js::Value()); // rooms are flat
	}
	if (WantsProp(properties, "role")) {
		std::string role = MailboxRole(ctx, room);
		v.Set("role", role.empty() ? js::Value() : js::Value::MakeString(role));
	}
	if (WantsProp(properties, "sortOrder")) {
		v.Set("sortOrder", room.listorder);
	}
	if (WantsProp(properties, "totalEmails")) {
		v.Set("totalEmails", stats.total);
	}
	if (WantsProp(properties, "unreadEmails")) {
		v.Set("unreadEmails", stats.new_count);
	}
	if (WantsProp(properties, "totalThreads")) {
		// Threads are derived from headers rather than stored, so a true count
		// would mean loading every message in the room to answer a listing.
		// The message count is the honest upper bound and what a client shows
		// when it is not grouping.
		v.Set("totalThreads", stats.total);
	}
	if (WantsProp(properties, "unreadThreads")) {
		v.Set("unreadThreads", stats.new_count);
	}
	if (WantsProp(properties, "myRights")) {
		v.Set("myRights", MailboxRights(ctx, room));
	}
	if (WantsProp(properties, "isSubscribed")) {
		// A zapped room is one the user has forgotten, which is exactly what
		// JMAP means by unsubscribed — and ListRooms has already excluded them,
		// so anything we are listing is subscribed.
		v.Set("isSubscribed", true);
	}
	return v;
}

js::Value MailboxGet(JmapCtx &jc, const js::Value &args) {
	js::Value err;
	if (!CheckAccount(jc, args, err)) {
		return err;
	}
	Ctx &ctx = jc.ctx;
	std::vector<std::string> ids;
	bool have_ids = false;
	if (!ResolveIds(jc, args, ids, have_ids)) {
		return MethodError("invalidResultReference");
	}

	js::Value list = js::Value::MakeArray();
	js::Value not_found = js::Value::MakeArray();
	const js::Value &properties = args["properties"];

	if (!have_ids) {
		for (const auto &room : JmapMailboxes(ctx)) {
			list.Push(MailboxToJson(ctx, room, properties));
		}
	} else {
		for (const auto &id : ids) {
			Room room;
			if (ResolveMailbox(ctx, id, room)) {
				list.Push(MailboxToJson(ctx, room, properties));
			} else {
				not_found.Push(js::Value::MakeString(id));
			}
		}
	}

	js::Value out = js::Value::MakeObject();
	out.Set("accountId", jc.account);
	out.Set("state", AccountState(ctx));
	out.Set("list", list);
	out.Set("notFound", not_found);
	return out;
}

js::Value MailboxQuery(JmapCtx &jc, const js::Value &args) {
	js::Value err;
	if (!CheckAccount(jc, args, err)) {
		return err;
	}
	Ctx &ctx = jc.ctx;
	const js::Value &filter = args["filter"];
	std::string want_role = filter["role"].AsString();
	std::string want_name = quackmail::util::Lower(filter["name"].AsString());
	bool has_role_filter = filter.Has("role");

	js::Value ids = js::Value::MakeArray();
	for (const auto &room : JmapMailboxes(ctx)) {
		std::string role = MailboxRole(ctx, room);
		if (has_role_filter) {
			// A null role in a filter means "any mailbox without a role", which
			// is how a client asks for the user's own folders.
			if (filter["role"].IsNull() ? !role.empty() : role != want_role) {
				continue;
			}
		}
		if (!want_name.empty() &&
		    quackmail::util::Lower(room.display_name).find(want_name) == std::string::npos) {
			continue;
		}
		ids.Push(js::Value::MakeString(IdOf(room.room_num)));
	}

	js::Value out = js::Value::MakeObject();
	out.Set("accountId", jc.account);
	out.Set("queryState", AccountState(ctx));
	out.Set("canCalculateChanges", false);
	out.Set("position", (int64_t)0);
	out.Set("ids", ids);
	out.Set("total", (int64_t)ids.Size());
	return out;
}

js::Value MailboxChanges(JmapCtx &jc, const js::Value &args) {
	js::Value err;
	if (!CheckAccount(jc, args, err)) {
		return err;
	}
	// Honest rather than convenient: nothing records room creation or deletion,
	// so there is no way to say which mailboxes appeared since a given state.
	// cannotCalculateChanges is the defined answer, and it tells the client to
	// re-fetch the list — which is cheap, because an account has a handful of
	// rooms rather than a mailbox full of messages.
	return MethodError("cannotCalculateChanges",
	                   "mailbox creation and deletion are not journalled; re-fetch with Mailbox/get");
}

// ---- Email ---------------------------------------------------------------

// Load a message the user is allowed to see, and say which of their mailboxes
// it was found in. LoadMessage has no notion of ownership, so going straight to
// it from a client-supplied id is a direct IDOR.
bool LoadEmailFor(Ctx &ctx, int64_t msgnum, Message &out, int64_t &room_num) {
	for (const auto &room : JmapMailboxes(ctx)) {
		if (quackmail::citadel::MessageInRoom(ctx.con, room.room_num, msgnum)) {
			room_num = room.room_num;
			return quackmail::citadel::LoadMessage(ctx.con, msgnum, out);
		}
	}
	return false;
}

js::Value EmailGet(JmapCtx &jc, const js::Value &args) {
	js::Value err;
	if (!CheckAccount(jc, args, err)) {
		return err;
	}
	Ctx &ctx = jc.ctx;
	std::vector<std::string> ids;
	bool have_ids = false;
	if (!ResolveIds(jc, args, ids, have_ids)) {
		return MethodError("invalidResultReference");
	}
	if (!have_ids) {
		// Unlike Mailbox/get, `ids` is required: an account's whole message
		// store is not a thing to serialize because a client forgot a filter.
		return MethodError("invalidArguments", "ids is required");
	}

	const js::Value &properties = args["properties"];
	bool fetch_text = args["fetchTextBodyValues"].AsBool(false) || args["fetchAllBodyValues"].AsBool(false);
	bool fetch_html = args["fetchHTMLBodyValues"].AsBool(false) || args["fetchAllBodyValues"].AsBool(false);

	js::Value list = js::Value::MakeArray();
	js::Value not_found = js::Value::MakeArray();
	for (const auto &id : ids) {
		int64_t msgnum = IdNum(id);
		Message msg;
		int64_t room_num = -1;
		if (msgnum < 0 || !LoadEmailFor(ctx, msgnum, msg, room_num)) {
			not_found.Push(js::Value::MakeString(id));
			continue;
		}
		list.Push(EmailToJson(ctx, msg, room_num, properties, args["bodyProperties"], fetch_text, fetch_html));
	}

	js::Value out = js::Value::MakeObject();
	out.Set("accountId", jc.account);
	out.Set("state", AccountState(ctx));
	out.Set("list", list);
	out.Set("notFound", not_found);
	return out;
}

// Does one message satisfy the filter? Only the conditions a mail client
// actually sends are implemented; an unknown condition matches, because
// silently returning nothing looks to a user like their mail is gone.
bool MatchesFilter(Ctx &ctx, const Message &msg, const js::Value &filter, int64_t room_num) {
	if (filter.type != js::Value::Object) {
		return true;
	}
	if (filter.Has("inMailbox")) {
		int64_t want = IdNum(filter["inMailbox"].AsString());
		if (want != room_num && !quackmail::citadel::MessageInRoom(ctx.con, want, msg.msgnum)) {
			return false;
		}
	}
	if (filter.Has("before") || filter.Has("after")) {
		// Compared as instants rather than as strings: the client sends
		// "2026-07-31T00:00:00Z" and the store holds a unix time.
		std::string before = filter["before"].AsString();
		std::string after = filter["after"].AsString();
		int64_t t = 0;
		if (!before.empty() && ParseUtc8601(before, t) && msg.msgtime >= t) {
			return false;
		}
		if (!after.empty() && ParseUtc8601(after, t) && msg.msgtime < t) {
			return false;
		}
	}
	auto has_kw = [&](const std::string &kw) {
		for (const auto &k : KeywordsFor(ctx, msg.msgnum)) {
			if (quackmail::util::Lower(k) == quackmail::util::Lower(kw)) {
				return true;
			}
		}
		return false;
	};
	if (filter.Has("hasKeyword") && !has_kw(filter["hasKeyword"].AsString())) {
		return false;
	}
	if (filter.Has("notKeyword") && has_kw(filter["notKeyword"].AsString())) {
		return false;
	}
	auto contains = [](const std::string &hay, const std::string &needle) {
		return quackmail::util::Lower(hay).find(quackmail::util::Lower(needle)) != std::string::npos;
	};
	if (filter.Has("subject") && !contains(msg.subject, filter["subject"].AsString())) {
		return false;
	}
	if (filter.Has("from") && !contains(msg.author, filter["from"].AsString())) {
		return false;
	}
	if (filter.Has("to") && !contains(msg.recipient, filter["to"].AsString())) {
		return false;
	}
	if (filter.Has("text") || filter.Has("body")) {
		std::string needle = filter.Has("text") ? filter["text"].AsString() : filter["body"].AsString();
		std::string hay = msg.subject + "\n" + msg.author + "\n" + quackmail::citadel::BodyText(msg);
		if (!contains(hay, needle)) {
			return false;
		}
	}
	return true;
}

js::Value EmailQuery(JmapCtx &jc, const js::Value &args) {
	js::Value err;
	if (!CheckAccount(jc, args, err)) {
		return err;
	}
	Ctx &ctx = jc.ctx;
	const js::Value &filter = args["filter"];

	// When the filter names a mailbox, resolve it once and scan only that room
	// rather than the whole account.
	int64_t only_room = -1;
	if (filter.Has("inMailbox")) {
		Room room;
		if (!ResolveMailbox(ctx, filter["inMailbox"].AsString(), room)) {
			// A mailbox the user cannot see is not an error that names it as
			// existing; an empty result is the answer that reveals nothing.
			js::Value out = js::Value::MakeObject();
			out.Set("accountId", jc.account);
			out.Set("queryState", AccountState(ctx));
			out.Set("canCalculateChanges", false);
			out.Set("position", (int64_t)0);
			out.Set("ids", js::Value::MakeArray());
			out.Set("total", (int64_t)0);
			return out;
		}
		only_room = room.room_num;
	}

	int64_t position = args["position"].AsInt(0);
	if (position < 0) {
		position = 0;
	}
	int64_t limit = args["limit"].AsInt(256);
	if (limit < 0 || limit > 4096) {
		limit = 4096;
	}

	std::vector<std::string> matched;
	for (const auto &e : ListEmails(ctx, only_room)) {
		Message msg;
		if (!quackmail::citadel::LoadMessage(ctx.con, e.msgnum, msg)) {
			continue;
		}
		if (!MatchesFilter(ctx, msg, filter, e.room_num)) {
			continue;
		}
		matched.push_back(IdOf(e.msgnum));
	}

	js::Value ids = js::Value::MakeArray();
	for (size_t i = (size_t)position; i < matched.size() && (int64_t)ids.Size() < limit; i++) {
		ids.Push(js::Value::MakeString(matched[i]));
	}

	js::Value out = js::Value::MakeObject();
	out.Set("accountId", jc.account);
	out.Set("queryState", AccountState(ctx));
	out.Set("canCalculateChanges", false);
	out.Set("position", position);
	out.Set("ids", ids);
	if (args["calculateTotal"].AsBool(false)) {
		out.Set("total", (int64_t)matched.size());
	}
	return out;
}

js::Value EmailChanges(JmapCtx &jc, const js::Value &args) {
	js::Value err;
	if (!CheckAccount(jc, args, err)) {
		return err;
	}
	Ctx &ctx = jc.ctx;
	int64_t since = AccountStateValue(args["sinceState"].AsString());
	if (since < 0) {
		// A state string we did not mint. RFC 8620 wants cannotCalculateChanges
		// rather than an empty diff, so the client re-queries instead of quietly
		// missing everything that happened.
		return MethodError("cannotCalculateChanges", "unrecognised sinceState");
	}
	int64_t max_changes = args["maxChanges"].AsInt(0);

	js::Value created = js::Value::MakeArray();
	js::Value updated = js::Value::MakeArray();
	js::Value destroyed = js::Value::MakeArray();
	std::vector<int64_t> seen;

	// The same two halves CalDAV's sync-collection walks: messages newer than
	// the cursor, and the tombstones left by removals.
	for (const auto &room : JmapMailboxes(ctx)) {
		for (const auto &ch : quackmail::citadel::RoomChangesSince(ctx.con, room.room_num, since)) {
			if (std::find(seen.begin(), seen.end(), ch.msgnum) != seen.end()) {
				continue;
			}
			seen.push_back(ch.msgnum);
			// A message removed from one room may still be in another the user
			// can see, in which case it changed rather than went away.
			Message msg;
			int64_t where = -1;
			bool still_there = LoadEmailFor(ctx, ch.msgnum, msg, where);
			if (!still_there) {
				destroyed.Push(js::Value::MakeString(IdOf(ch.msgnum)));
			} else if (ch.deleted) {
				updated.Push(js::Value::MakeString(IdOf(ch.msgnum)));
			} else {
				created.Push(js::Value::MakeString(IdOf(ch.msgnum)));
			}
		}
	}

	int64_t total = (int64_t)(created.Size() + updated.Size() + destroyed.Size());
	if (max_changes > 0 && total > max_changes) {
		return MethodError("cannotCalculateChanges", "more changes than maxChanges");
	}

	js::Value out = js::Value::MakeObject();
	out.Set("accountId", jc.account);
	out.Set("oldState", std::to_string(since));
	out.Set("newState", AccountState(ctx));
	out.Set("hasMoreChanges", false);
	out.Set("created", created);
	out.Set("updated", updated);
	out.Set("destroyed", destroyed);
	return out;
}

// Point a message at some mailboxes and unlink it from others.
//
// **Additions run before removals, always.** Moving a message from Drafts to
// Sent is one patch naming both, and applying them in the order the client
// happened to write them unlinks the only pointer first — after which there is
// no source room left to copy from, the add silently fails, and the message
// belongs to no mailbox at all. Ordering the two passes is what makes the
// operation a move rather than a way to lose mail.
bool ApplyMailboxWants(Ctx &ctx, int64_t msgnum, const std::vector<std::pair<int64_t, bool>> &wants,
                       std::string &why) {
	for (int pass = 0; pass < 2; pass++) {
		const bool adding = (pass == 0);
		for (const auto &w : wants) {
			if (w.second != adding) {
				continue;
			}
			Room room;
			if (!ResolveMailbox(ctx, IdOf(w.first), room)) {
				why = "no such mailbox";
				return false;
			}
			if (!quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
				why = "you cannot change that mailbox";
				return false;
			}
			if (quackmail::citadel::MessageInRoom(ctx.con, room.room_num, msgnum) == adding) {
				continue; // already where the client wants it
			}
			std::string err;
			if (adding) {
				// Filing into a mailbox is a copy, so the message keeps its
				// pointers into every other room it is already in. The source
				// is re-resolved here rather than captured earlier, because an
				// earlier item in this same patch may have moved it.
				Message msg;
				int64_t from = -1;
				if (!LoadEmailFor(ctx, msgnum, msg, from)) {
					why = "no such email";
					return false;
				}
				quackmail::citadel::MoveMessage(ctx.con, from, room.room_num, msgnum, true, err);
			} else {
				quackmail::citadel::DeleteMessage(ctx.con, room.room_num, msgnum, err);
			}
		}
	}
	return true;
}

} // namespace

// Declared in jmap.hpp: EmailSubmission's onSuccessUpdateEmail sends the same
// patch shape and must go through this rather than growing a second copy.
//
// JMAP patches are flat pointer paths ("keywords/$seen": true), and only the two
// a mail client actually sends are honoured: keywords and mailboxIds.
bool ApplyEmailPatch(Ctx &ctx, int64_t msgnum, const js::Value &patch, std::string &why) {
	std::vector<std::pair<int64_t, bool>> wants;
	for (const auto &m : patch.members) {
		const std::string &path = m.first;
		if (path == "keywords" && m.second.type == js::Value::Object) {
			for (const auto &k : KeywordsFor(ctx, msgnum)) {
				SetKeyword(ctx, msgnum, k, false);
			}
			for (const auto &kv : m.second.members) {
				SetKeyword(ctx, msgnum, kv.first, kv.second.AsBool(false));
			}
			continue;
		}
		if (path.rfind("keywords/", 0) == 0) {
			// A null value removes, which is how JMAP spells "unset" in a patch.
			SetKeyword(ctx, msgnum, path.substr(9), m.second.AsBool(false));
			continue;
		}
		if (path.rfind("mailboxIds/", 0) == 0) {
			int64_t id = IdNum(path.substr(11));
			if (id < 0) {
				why = "no such mailbox";
				return false;
			}
			wants.push_back(std::make_pair(id, m.second.AsBool(false)));
			continue;
		}
		if (path == "mailboxIds" && m.second.type == js::Value::Object) {
			// A wholesale replacement: in everything named, out of everything
			// not.
			for (const auto &room : JmapMailboxes(ctx)) {
				bool want = m.second[IdOf(room.room_num)].AsBool(false);
				if (want != quackmail::citadel::MessageInRoom(ctx.con, room.room_num, msgnum)) {
					wants.push_back(std::make_pair(room.room_num, want));
				}
			}
			continue;
		}
		why = "cannot set " + path;
		return false;
	}
	return ApplyMailboxWants(ctx, msgnum, wants, why);
}

namespace {

js::Value EmailSet(JmapCtx &jc, const js::Value &args) {
	js::Value err;
	if (!CheckAccount(jc, args, err)) {
		return err;
	}
	Ctx &ctx = jc.ctx;
	std::string if_in_state = args["ifInState"].AsString();
	std::string old_state = AccountState(ctx);
	if (!if_in_state.empty() && if_in_state != old_state) {
		// The optimistic-concurrency check. Without it two clients editing one
		// mailbox silently overwrite each other.
		return MethodError("stateMismatch");
	}

	js::Value created = js::Value::MakeObject();
	js::Value not_created = js::Value::MakeObject();
	js::Value updated = js::Value::MakeObject();
	js::Value not_updated = js::Value::MakeObject();
	js::Value destroyed = js::Value::MakeArray();
	js::Value not_destroyed = js::Value::MakeObject();

	// ---- create ----------------------------------------------------------
	const js::Value &to_create = args["create"];
	for (const auto &m : to_create.members) {
		const js::Value &spec = m.second;
		// Which mailbox. A create with no mailboxIds has nowhere to go.
		Room room;
		bool found = false;
		for (const auto &kv : spec["mailboxIds"].members) {
			if (kv.second.AsBool(false) && ResolveMailbox(ctx, kv.first, room)) {
				found = true;
				break;
			}
		}
		if (!found) {
			not_created.Set(m.first, SetError("invalidProperties", "mailboxIds names no mailbox you can use"));
			continue;
		}
		// Unlike IMAP's APPEND, which does not ask. A front-end that skips this
		// is how a read-only room stops being one.
		if (!quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
			not_created.Set(m.first, SetError("forbidden", "you cannot add to that mailbox"));
			continue;
		}

		// Build the RFC822 message from the JMAP shape.
		mime::HeaderList headers;
		std::string fqdn = ConfigStr(ctx.con, "c_fqdn", "localhost");
		auto addr_header = [&](const char *field, const char *header) {
			const js::Value &list = spec[field];
			std::string joined;
			for (size_t i = 0; i < list.Size(); i++) {
				const js::Value &a = list.At(i);
				std::string email = a["email"].AsString();
				if (email.empty()) {
					continue;
				}
				if (!joined.empty()) {
					joined += ", ";
				}
				std::string name = a["name"].AsString();
				joined += name.empty() ? email : mime::EncodeEncodedWord(name) + " <" + email + ">";
			}
			if (!joined.empty()) {
				headers.push_back(std::make_pair(header, joined));
			}
		};
		if (!spec.Has("from")) {
			headers.push_back(std::make_pair("From", ctx.username + "@" + fqdn));
		}
		addr_header("from", "From");
		addr_header("to", "To");
		addr_header("cc", "Cc");
		addr_header("bcc", "Bcc");
		addr_header("replyTo", "Reply-To");
		std::string subject = spec["subject"].AsString();
		headers.push_back(std::make_pair("Subject", mime::EncodeEncodedWord(subject)));
		headers.push_back(std::make_pair("Date", quackmail::util::RfcDate()));
		headers.push_back(std::make_pair("Message-ID", "<" + quackmail::util::RandomHex(12) + "@" + fqdn + ">"));

		std::vector<mime::BuildPart> parts;
		const js::Value &body_values = spec["bodyValues"];
		auto push_body = [&](const char *which, const char *type) {
			const js::Value &list = spec[which];
			for (size_t i = 0; i < list.Size(); i++) {
				std::string part_id = list.At(i)["partId"].AsString();
				std::string text = body_values[part_id]["value"].AsString();
				if (text.empty()) {
					continue;
				}
				mime::BuildPart p;
				p.content_type = type;
				p.content = text;
				parts.push_back(p);
			}
		};
		push_body("textBody", "text/plain");
		push_body("htmlBody", "text/html");

		// Attachments, by the blobId the client got from /jmap/upload. The
		// bytes are copied into the message here rather than referenced: a blob
		// is temporary by JMAP's own definition, and a message that pointed at
		// one would lose its attachment the moment the staging row aged out.
		bool blob_missing = false;
		const js::Value &atts = spec["attachments"];
		for (size_t i = 0; i < atts.Size(); i++) {
			const js::Value &a = atts.At(i);
			std::string blob_id = a["blobId"].AsString();
			std::string content, stored_type;
			if (blob_id.empty() || !LoadBlob(ctx, blob_id, content, stored_type)) {
				blob_missing = true;
				break;
			}
			mime::BuildPart p;
			// The client's declared type wins over the one the upload recorded,
			// which is what JMAP says and what lets it correct a bad guess made
			// at upload time.
			p.content_type = a["type"].AsString().empty() ? stored_type : a["type"].AsString();
			p.content = content;
			p.filename = a["name"].AsString();
			p.content_id = a["cid"].AsString();
			p.disposition = a["disposition"].AsString().empty()
			                    ? std::string(p.content_id.empty() ? "attachment" : "inline")
			                    : a["disposition"].AsString();
			parts.push_back(p);
		}
		if (blob_missing) {
			// blobNotFound is the defined answer, and naming it is what tells a
			// client to re-upload rather than retry the same id forever.
			not_created.Set(m.first, SetError("blobNotFound", "attachments name a blob that is not yours"));
			continue;
		}

		if (parts.empty()) {
			mime::BuildPart p;
			p.content_type = "text/plain";
			p.content = "";
			parts.push_back(p);
		}

		Message msg;
		msg.author = ctx.username;
		msg.author_usernum = quackmail::citadel::GetOrAssignUserNum(ctx.con, ctx.username);
		msg.msgtime = (int64_t)std::time(nullptr);
		msg.subject = subject;
		msg.format_type = 4;
		msg.origin_room = room.display_name;
		msg.node = ConfigStr(ctx.con, "c_nodename", "");
		msg.raw = mime::BuildMessage(headers, parts);

		std::string store_err;
		int64_t msgnum = quackmail::citadel::InsertMessage(ctx.con, msg, {room.room_num}, store_err);
		if (msgnum < 0) {
			not_created.Set(m.first, SetError("serverFail", store_err));
			continue;
		}
		for (const auto &kv : spec["keywords"].members) {
			if (kv.second.AsBool(false)) {
				SetKeyword(ctx, msgnum, kv.first, true);
			}
		}

		js::Value made = js::Value::MakeObject();
		made.Set("id", IdOf(msgnum));
		made.Set("blobId", IdOf(msgnum));
		made.Set("threadId", ThreadIdFor(msg, NodeName(jc.ctx)));
		made.Set("size", (int64_t)msg.raw.size());
		created.Set(m.first, made);
	}

	// ---- update ----------------------------------------------------------
	for (const auto &m : args["update"].members) {
		int64_t msgnum = IdNum(m.first);
		Message msg;
		int64_t room_num = -1;
		if (msgnum < 0 || !LoadEmailFor(ctx, msgnum, msg, room_num)) {
			not_updated.Set(m.first, SetError("notFound"));
			continue;
		}
		std::string why;
		if (!ApplyEmailPatch(ctx, msgnum, m.second, why)) {
			not_updated.Set(m.first, SetError("invalidProperties", why));
			continue;
		}
		updated.Set(m.first, js::Value());
	}

	// ---- destroy ---------------------------------------------------------
	const js::Value &to_destroy = args["destroy"];
	for (size_t i = 0; i < to_destroy.Size(); i++) {
		std::string id = to_destroy.At(i).AsString();
		int64_t msgnum = IdNum(id);
		Message msg;
		int64_t room_num = -1;
		if (msgnum < 0 || !LoadEmailFor(ctx, msgnum, msg, room_num)) {
			not_destroyed.Set(id, SetError("notFound"));
			continue;
		}
		// Unlinked from every mailbox this user can see, which is what
		// "destroy" means to a client. A copy in a room they cannot see is not
		// theirs to remove.
		bool any = false;
		std::string store_err;
		for (const auto &room : JmapMailboxes(ctx)) {
			if (!quackmail::citadel::MessageInRoom(ctx.con, room.room_num, msgnum)) {
				continue;
			}
			if (!quackmail::citadel::CanPost(ctx.con, ctx.username, room)) {
				continue;
			}
			if (quackmail::citadel::DeleteMessage(ctx.con, room.room_num, msgnum, store_err)) {
				any = true;
			}
		}
		if (any) {
			destroyed.Push(js::Value::MakeString(id));
		} else {
			not_destroyed.Set(id, SetError("forbidden", "you cannot remove that message"));
		}
	}

	js::Value out = js::Value::MakeObject();
	out.Set("accountId", jc.account);
	out.Set("oldState", old_state);
	out.Set("newState", AccountState(ctx));
	out.Set("created", created);
	out.Set("notCreated", not_created);
	out.Set("updated", updated);
	out.Set("notUpdated", not_updated);
	out.Set("destroyed", destroyed);
	out.Set("notDestroyed", not_destroyed);
	return out;
}

// ---- Thread --------------------------------------------------------------

js::Value ThreadGet(JmapCtx &jc, const js::Value &args) {
	js::Value err;
	if (!CheckAccount(jc, args, err)) {
		return err;
	}
	Ctx &ctx = jc.ctx;
	std::vector<std::string> ids;
	bool have_ids = false;
	if (!ResolveIds(jc, args, ids, have_ids)) {
		return MethodError("invalidResultReference");
	}
	if (!have_ids) {
		return MethodError("invalidArguments", "ids is required");
	}

	// A thread id is a function of a message's References header rather than
	// something stored, so membership is found by walking the account rather
	// than by an index. That is O(messages) per call and fine at BBS scale;
	// it is the price of not adding a column two other front-ends would have
	// to learn to maintain.
	js::Value list = js::Value::MakeArray();
	js::Value not_found = js::Value::MakeArray();
	auto all = ListEmails(ctx, -1);
	std::string node = NodeName(ctx);

	for (const auto &want : ids) {
		js::Value email_ids = js::Value::MakeArray();
		for (const auto &e : all) {
			Message msg;
			if (!quackmail::citadel::LoadMessage(ctx.con, e.msgnum, msg)) {
				continue;
			}
			if (ThreadIdFor(msg, node) == want) {
				email_ids.Push(js::Value::MakeString(IdOf(msg.msgnum)));
			}
		}
		if (!email_ids.Size()) {
			not_found.Push(js::Value::MakeString(want));
			continue;
		}
		js::Value t = js::Value::MakeObject();
		t.Set("id", want);
		t.Set("emailIds", email_ids);
		list.Push(t);
	}

	js::Value out = js::Value::MakeObject();
	out.Set("accountId", jc.account);
	out.Set("state", AccountState(ctx));
	out.Set("list", list);
	out.Set("notFound", not_found);
	return out;
}

// ---- Identity ------------------------------------------------------------

js::Value IdentityGet(JmapCtx &jc, const js::Value &args) {
	js::Value err;
	if (!CheckAccount(jc, args, err)) {
		return err;
	}
	Ctx &ctx = jc.ctx;
	quackmail::citadel::Registration reg;
	quackmail::citadel::GetRegistration(ctx.con, ctx.username, reg);

	js::Value id = js::Value::MakeObject();
	id.Set("id", "0");
	id.Set("name", reg.real_name.empty() ? ctx.username : reg.real_name);
	id.Set("email", ctx.username + "@" + ConfigStr(ctx.con, "c_fqdn", "localhost"));
	id.Set("replyTo", js::Value());
	id.Set("bcc", js::Value());
	id.Set("textSignature", "");
	id.Set("htmlSignature", "");
	id.Set("mayDelete", false);

	js::Value list = js::Value::MakeArray();
	list.Push(id);

	js::Value out = js::Value::MakeObject();
	out.Set("accountId", jc.account);
	out.Set("state", AccountState(ctx));
	out.Set("list", list);
	out.Set("notFound", js::Value::MakeArray());
	return out;
}

} // namespace

void RegisterMailMethods(std::vector<JmapEntry> &out) {
	out.push_back({"Mailbox/get", MailboxGet});
	out.push_back({"Mailbox/query", MailboxQuery});
	out.push_back({"Mailbox/changes", MailboxChanges});
	out.push_back({"Email/get", EmailGet});
	out.push_back({"Email/query", EmailQuery});
	out.push_back({"Email/changes", EmailChanges});
	out.push_back({"Email/set", EmailSet});
	out.push_back({"Thread/get", ThreadGet});
	out.push_back({"Identity/get", IdentityGet});
}

} // namespace qmweb
} // namespace duckdb
