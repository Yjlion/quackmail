#include "jmap.hpp"

#include "quackmail/citadel_msg.hpp"
#include "quackmail/mailpolicy.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/submission.hpp"
#include "quackmail/util.hpp"

#include <cstdlib>

namespace duckdb {
namespace qmweb {

using quackmail::citadel::Message;
using quackmail::citadel::Room;

namespace {

namespace mime = quackmail::mime;

// The message a blob id names, and optionally one part of it.
//
// A blob id is either "<msgnum>" for the whole RFC822 message or
// "<msgnum>.<section>" for one MIME part, which is what EmailToJson hands a
// client in an attachment's blobId. Both go through the same ownership check:
// a bare message number from a URL is a direct IDOR otherwise.
bool ResolveBlob(Ctx &ctx, const std::string &blob_id, std::string &body, std::string &type,
                 std::string &filename) {
	// An uploaded blob first: it has no message behind it, so none of the
	// message-store lookups below would find it.
	if (IsUploadBlobId(blob_id)) {
		if (!LoadBlob(ctx, blob_id, body, type)) {
			return false;
		}
		filename.clear();
		return true;
	}

	std::string num_part = blob_id;
	std::string section;
	size_t dot = blob_id.find('.');
	if (dot != std::string::npos) {
		num_part = blob_id.substr(0, dot);
		section = blob_id.substr(dot + 1);
	}
	int64_t msgnum = IdNum(num_part);
	if (msgnum < 0) {
		return false;
	}

	bool visible = false;
	for (const auto &room : JmapMailboxes(ctx)) {
		if (quackmail::citadel::MessageInRoom(ctx.con, room.room_num, msgnum)) {
			visible = true;
			break;
		}
	}
	if (!visible) {
		return false;
	}
	Message msg;
	if (!quackmail::citadel::LoadMessage(ctx.con, msgnum, msg)) {
		return false;
	}

	std::string rfc822 =
	    quackmail::citadel::RenderRfc822(msg, quackmail::citadel::GetConfig(ctx.con, "c_nodename", "quackcit"));
	if (section.empty()) {
		body = rfc822;
		type = "message/rfc822";
		filename = "message.eml";
		return true;
	}

	mime::MimeEntity entity = mime::ParseEntity(rfc822);
	auto parts = mime::FlattenParts(entity);
	for (size_t i = 0; i < parts.size(); i++) {
		std::string id = parts[i].section.empty() ? std::to_string(i) : parts[i].section;
		if (id != section) {
			continue;
		}
		body = parts[i].content;
		type = parts[i].content_type;
		filename = parts[i].filename;
		return true;
	}
	return false;
}

// GET /jmap/download/{accountId}/{blobId}/{name}
void JmapDownload(Ctx &ctx) {
	if (ctx.Cap(0) != ctx.username) {
		ctx.resp.status = 404;
		ctx.resp.body.clear();
		return;
	}
	std::string body, type, filename;
	if (!ResolveBlob(ctx, ctx.Cap(1), body, type, filename)) {
		ctx.resp.status = 404;
		ctx.resp.body.clear();
		return;
	}

	// Never the sender's Content-Type, and never inline. A blob served as
	// text/html from our own origin would be a stored XSS with a download link —
	// the same reasoning web_mail.cpp's attachment route already applies, and
	// the reason `accept` in the downloadUrl template is not honoured.
	(void)type;
	std::string name = http::SanitizeFilename(filename.empty() ? ctx.Cap(2) : filename);
	SecurityHeaders(ctx, "default-src 'none'; sandbox");
	ctx.resp.Bytes(body, "application/octet-stream");
	ctx.resp.SetHeader("Content-Disposition",
	                   "attachment; filename=\"" + name + "\"; filename*=UTF-8''" +
	                       http::PercentEncode(name));
}

// The envelope a submission uses: what the client asked for, or one derived
// from the message's own headers when it said nothing.
bool BuildEnvelope(Ctx &ctx, const js::Value &spec, const Message &msg, const std::string &rfc822,
                   std::string &mail_from, std::vector<std::string> &rcpts, std::string &why) {
	(void)msg;
	const js::Value &env = spec["envelope"];
	if (env.type == js::Value::Object) {
		mail_from = env["mailFrom"]["email"].AsString();
		const js::Value &to = env["rcptTo"];
		for (size_t i = 0; i < to.Size(); i++) {
			std::string addr = to.At(i)["email"].AsString();
			if (!addr.empty()) {
				rcpts.push_back(addr);
			}
		}
	}
	if (mail_from.empty()) {
		mail_from = ctx.username + "@" + ConfigStr(ctx.con, "c_fqdn", "localhost");
	}
	if (rcpts.empty()) {
		// Derived from the message, which is what a client that sent no envelope
		// expects. Bcc counts: it is in the stored draft and is exactly the
		// recipient that would otherwise be silently dropped.
		mime::MimeEntity entity = mime::ParseEntity(rfc822);
		for (const auto &h : entity.headers) {
			std::string name = quackmail::util::Lower(h.first);
			if (name != "to" && name != "cc" && name != "bcc") {
				continue;
			}
			for (const auto &a : mime::ParseAddressList(h.second)) {
				if (!a.addr.empty()) {
					rcpts.push_back(a.addr);
				}
			}
		}
	}
	if (rcpts.empty()) {
		why = "the message names no recipients";
		return false;
	}
	return true;
}

js::Value EmailSubmissionSet(JmapCtx &jc, const js::Value &args) {
	js::Value err;
	if (!CheckAccount(jc, args, err)) {
		return err;
	}
	Ctx &ctx = jc.ctx;
	std::string old_state = AccountState(ctx);
	if (args.Has("ifInState") && args["ifInState"].AsString() != old_state) {
		return MethodError("stateMismatch");
	}

	js::Value created = js::Value::MakeObject();
	js::Value not_created = js::Value::MakeObject();

	for (const auto &m : args["create"].members) {
		const js::Value &spec = m.second;
		std::string email_id = spec["emailId"].AsString();
		// A back-reference to an Email/set in the same request, which is how a
		// client composes and sends in one round trip. The path is a JSON
		// pointer ("/created/<creationId>/id"), so it goes through the same
		// resolver "#ids" does rather than a substring guess — which gets the
		// creation id wrong in a way that looks exactly like the message not
		// existing.
		if (email_id.empty() && spec.Has("#emailId")) {
			std::vector<std::string> refs;
			if (ResolveReference(jc, spec["#emailId"], refs) && !refs.empty()) {
				email_id = refs[0];
			}
		}

		int64_t msgnum = IdNum(email_id);
		if (msgnum < 0) {
			not_created.Set(m.first, SetError("invalidProperties", "emailId names no message"));
			continue;
		}

		// Ownership, before anything else: an emailId is a number out of a
		// request body, and sending a message the user cannot see would be both
		// a disclosure and a way to relay through someone else's draft.
		Room found;
		bool visible = false;
		for (const auto &room : JmapMailboxes(ctx)) {
			if (quackmail::citadel::MessageInRoom(ctx.con, room.room_num, msgnum)) {
				found = room;
				visible = true;
				break;
			}
		}
		Message msg;
		if (!visible || !quackmail::citadel::LoadMessage(ctx.con, msgnum, msg)) {
			not_created.Set(m.first, SetError("notFound"));
			continue;
		}

		std::string rfc822 = quackmail::citadel::RenderRfc822(
		    msg, quackmail::citadel::GetConfig(ctx.con, "c_nodename", "quackcit"));

		std::string mail_from;
		std::vector<std::string> rcpts;
		std::string why;
		if (!BuildEnvelope(ctx, spec, msg, rfc822, mail_from, rcpts, why)) {
			not_created.Set(m.first, SetError("invalidProperties", why));
			continue;
		}

		// The same per-user quota the submission listener charges, checked
		// before the message goes anywhere. A second door onto the same mail
		// path that skipped it would make the limit advisory.
		auto quota = quackmail::policy::CheckRate(ctx.con, ctx.username, (int64_t)rcpts.size());
		if (!quota.allowed) {
			not_created.Set(m.first, SetError("overQuota", quota.reason));
			continue;
		}

		std::string received =
		    quackmail::submission::ReceivedHeader(ctx.con, "jmap", ctx.username, ctx.tls);
		quackmail::submission::Result sent;
		if (!quackmail::submission::Send(ctx.con, mail_from, rcpts, received, rfc822, sent)) {
			not_created.Set(m.first, SetError("serverFail", sent.err));
			continue;
		}
		for (const auto &r : rcpts) {
			quackmail::policy::RecordSend(ctx.con, ctx.username, r, 1);
		}

		js::Value delivered = js::Value::MakeObject();
		for (const auto &r : sent.delivered) {
			js::Value d = js::Value::MakeObject();
			d.Set("smtpReply", "250 2.0.0 stored");
			d.Set("delivered", "yes");
			d.Set("displayed", false);
			delivered.Set(r, d);
		}
		for (const auto &r : sent.queued) {
			js::Value d = js::Value::MakeObject();
			d.Set("smtpReply", "250 2.0.0 queued");
			// "queued" and not "yes": the relay worker has not run yet, and
			// telling a client a message was delivered before it left the
			// building is the sort of lie it will show to a user.
			d.Set("delivered", "queued");
			d.Set("displayed", false);
			delivered.Set(r, d);
		}

		js::Value sub = js::Value::MakeObject();
		sub.Set("id", "S" + IdOf(msgnum));
		sub.Set("emailId", email_id);
		sub.Set("threadId", ThreadIdFor(msg));
		sub.Set("identityId", spec["identityId"].AsString().empty()
		                          ? std::string("0")
		                          : spec["identityId"].AsString());
		sub.Set("sendAt", js::Value());
		sub.Set("undoStatus", "final"); // maxDelayedSend is 0; nothing is held
		sub.Set("deliveryStatus", delivered);
		created.Set(m.first, sub);

		// onSuccessUpdateEmail: file the sent message and mark it, which is how
		// a client moves a draft to Sent in the round trip it sends it in.
		//
		// Through ApplyEmailPatch rather than by walking the patch here: it is
		// the same shape Email/set takes, and it is where the rule lives that
		// additions happen before removals. A second copy of that would be a
		// second chance to unlink a message's only pointer and lose it.
		(void)found;
		const js::Value &on_success = args["onSuccessUpdateEmail"];
		std::string key = "#" + m.first;
		if (on_success.Has(key)) {
			std::string patch_why;
			ApplyEmailPatch(ctx, msgnum, on_success[key], patch_why);
		}
	}

	js::Value out = js::Value::MakeObject();
	out.Set("accountId", jc.account);
	out.Set("oldState", old_state);
	out.Set("newState", AccountState(ctx));
	out.Set("created", created);
	out.Set("notCreated", not_created);
	// Nothing is held, so nothing can be cancelled or resent.
	out.Set("updated", js::Value::MakeObject());
	out.Set("notUpdated", js::Value::MakeObject());
	out.Set("destroyed", js::Value::MakeArray());
	out.Set("notDestroyed", js::Value::MakeObject());
	return out;
}

js::Value EmailSubmissionGet(JmapCtx &jc, const js::Value &args) {
	js::Value err;
	if (!CheckAccount(jc, args, err)) {
		return err;
	}
	// Submissions are not journalled: maxDelayedSend is 0, so a submission has
	// no life after the call that made it and there is nothing to look up. An
	// empty list with every requested id in notFound is the honest answer.
	std::vector<std::string> ids;
	bool have_ids = false;
	ResolveIds(jc, args, ids, have_ids);
	js::Value not_found = js::Value::MakeArray();
	for (const auto &id : ids) {
		not_found.Push(js::Value::MakeString(id));
	}

	js::Value out = js::Value::MakeObject();
	out.Set("accountId", jc.account);
	out.Set("state", AccountState(jc.ctx));
	out.Set("list", js::Value::MakeArray());
	out.Set("notFound", not_found);
	return out;
}

} // namespace

void RegisterSubmissionMethods(std::vector<JmapEntry> &out) {
	out.push_back({"EmailSubmission/set", EmailSubmissionSet});
	out.push_back({"EmailSubmission/get", EmailSubmissionGet});
}

void RegisterJmapDownloadRoute(std::vector<Route> &out) {
	out.push_back({"GET", "/jmap/download/:account/:blob/:name", Role::Api, JmapDownload});
}

} // namespace qmweb
} // namespace duckdb
