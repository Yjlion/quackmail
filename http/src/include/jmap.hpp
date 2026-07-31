#pragma once

#include "web.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/json.hpp"

namespace duckdb {
namespace qmweb {

// JMAP: RFC 8620 (core) and RFC 8621 (mail).
//
// The same projection IMAP already makes, in JSON instead of on a socket — a
// Mailbox is a room, an Email is a message in one, and a keyword is a row in
// citadel_msg_flags. Nothing here is a new store, and where IMAP and JMAP could
// disagree the store is asked rather than either front-end deciding.
//
//   /.well-known/jmap        the Session resource
//   /jmap/api                the method-call endpoint (POST only)
//   /jmap/download/...       blob download
//
// Deliberately absent: the JMAP Calendars and Contacts bindings, which are
// drafts nothing ships against — CalDAV and CardDAV are what a phone actually
// connects with, and they are served from /dav/ over these same rooms.

namespace js = quackmail::json;

// One request's worth of state. Method calls share it so a later call can refer
// back to an earlier one's result.
struct JmapCtx {
	Ctx &ctx;
	// The account id, which is the username: this server has exactly one account
	// per user and no shared or delegated ones.
	std::string account;
	// Every (name, callId, result) triple emitted so far, for back-references.
	js::Value responses;

	explicit JmapCtx(Ctx &c) : ctx(c), responses(js::Value::MakeArray()) {
	}
};

// ---- ids -----------------------------------------------------------------
// JMAP ids are opaque strings. Ours are decimal, because the underlying keys
// already are and inventing an encoding would only make a packet capture harder
// to read. They are still strings on the wire: RFC 8620 is explicit that a
// client must not do arithmetic on one.

std::string IdOf(int64_t n);
// -1 when the string is not one of ours. Used everywhere a client-supplied id
// reaches a lookup, so a non-numeric id is "not found" rather than a 0.
int64_t IdNum(const std::string &id);

// ---- state ---------------------------------------------------------------

// The account's state string: the largest change token across every room the
// user can see. Changes when anything they can reach changes, which is what
// makes /changes answerable. Built on citadel::RoomChangeToken, the same
// counter CalDAV's sync-token uses.
std::string AccountState(Ctx &ctx);
int64_t AccountStateValue(const std::string &state);

// ---- errors --------------------------------------------------------------

// A method-level error: ["error", {"type": ...}, callId]. Returned in place of
// the method's own response, which is how JMAP reports a failure that is about
// one call rather than about the request.
js::Value MethodError(const std::string &type, const std::string &description = std::string());
// A SetError, for one item inside an Email/set or EmailSubmission/set.
js::Value SetError(const std::string &type, const std::string &description = std::string());

// ---- mailboxes and messages ----------------------------------------------

// The rooms this account sees as mailboxes. The same set IMAP lists, for the
// same reason: a room is a mailbox, and hiding the BBS ones would make the two
// front-ends disagree about what the account contains.
std::vector<quackmail::citadel::Room> JmapMailboxes(Ctx &ctx);
// Resolve one by JMAP id, applying the visibility rules. False when the id is
// not ours, not a room, or not one this user may see.
bool ResolveMailbox(Ctx &ctx, const std::string &id, quackmail::citadel::Room &out);

// The well-known role a room plays ("inbox", "sent", "drafts", "trash"), or ""
// for an ordinary one. A client uses this to decide where a reply is filed.
std::string MailboxRole(Ctx &ctx, const quackmail::citadel::Room &room);

// The JMAP keywords on a message, translated from the IMAP flags the store
// holds. The two vocabularies differ only in spelling ("\\Seen" / "$seen"), and
// translating rather than storing both is what keeps IMAP and JMAP from
// drifting into two different truths about whether a message is read.
std::vector<std::string> KeywordsFor(Ctx &ctx, int64_t msgnum);
void SetKeyword(Ctx &ctx, int64_t msgnum, const std::string &keyword, bool on);
// The IMAP flag a JMAP keyword corresponds to, or "" when it has no equivalent
// (a user-defined keyword, which is stored as itself).
std::string ImapFlagFor(const std::string &keyword);

// The thread a message belongs to. Derived from its References header: a reply
// joins the thread its root started, and a message with no references is a
// thread of one. Stable across restarts because it is a function of the
// headers rather than of anything we store.
std::string ThreadIdFor(const quackmail::citadel::Message &msg);

// Every message the account can see, newest first, optionally restricted to one
// mailbox. This is the backing list for Email/query.
struct JmapEmail {
	int64_t msgnum = 0;
	int64_t room_num = 0;
	int64_t received = 0;
};
std::vector<JmapEmail> ListEmails(Ctx &ctx, int64_t only_room);

// ---- the method table ----------------------------------------------------
// Each file registers the methods it implements. Dispatch is a linear scan of a
// table built once, exactly as the HTTP router does it.

using JmapMethod = js::Value (*)(JmapCtx &, const js::Value &args);

struct JmapEntry {
	const char *name;
	JmapMethod fn;
};

void RegisterCoreMethods(std::vector<JmapEntry> &out);
void RegisterMailMethods(std::vector<JmapEntry> &out);
void RegisterSubmissionMethods(std::vector<JmapEntry> &out);
// The blob-download route, contributed by jmap_submission.cpp because that is
// where the blob resolver lives.
void RegisterJmapDownloadRoute(std::vector<Route> &out);

// Shared argument reading, so every method treats a missing accountId the same
// way. False when the account is not this user's, with `err` set to the
// response the caller should return instead.
bool CheckAccount(JmapCtx &jc, const js::Value &args, js::Value &err);

// The `ids` argument, resolved through a back-reference when the client sent
// one. Returns false when the reference could not be resolved, which JMAP
// reports as invalidResultReference.
bool ResolveIds(JmapCtx &jc, const js::Value &args, std::vector<std::string> &out, bool &present);

// Resolve one {resultOf, name, path} back-reference against the calls already
// answered in this request. The path is a JSON pointer; a string result yields
// one element and an array yields all of them.
//
// Shared rather than re-derived per call site: EmailSubmission's "#emailId"
// points at "/created/<creationId>/id", which a substring guess gets wrong in a
// way that looks exactly like the message not existing.
bool ResolveReference(JmapCtx &jc, const js::Value &ref, std::vector<std::string> &out);

// Apply one Email/update patch: keywords and mailboxIds. Shared with
// EmailSubmission's onSuccessUpdateEmail, which sends exactly the same shape and
// must not grow a second copy of the "additions before removals" rule that makes
// a move a move rather than a way to lose the message.
bool ApplyEmailPatch(Ctx &ctx, int64_t msgnum, const js::Value &patch, std::string &why);

// Serialize one Email into the JMAP shape, honouring a `properties` filter.
js::Value EmailToJson(Ctx &ctx, const quackmail::citadel::Message &msg, int64_t room_num,
                      const js::Value &properties, const js::Value &body_properties,
                      bool fetch_text, bool fetch_html);

} // namespace qmweb
} // namespace duckdb
