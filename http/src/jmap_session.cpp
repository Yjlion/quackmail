#include "jmap.hpp"

#include "quackmail/util.hpp"

#include <cstdlib>

namespace duckdb {
namespace qmweb {

namespace {

// The capability URNs this server implements. A capability we advertise but do
// not honour is one a client builds a whole feature on and then fails.
const char *const kCoreCapability = "urn:ietf:params:jmap:core";
const char *const kMailCapability = "urn:ietf:params:jmap:mail";
const char *const kSubmissionCapability = "urn:ietf:params:jmap:submission";

// How much of one request we will do. RFC 8620 requires these to be advertised
// in the Session object rather than discovered by hitting them.
// The same ceiling http::Limits already enforces, advertised so a client can
// refuse an oversized attachment itself rather than discovering it as a 413.
constexpr int64_t kMaxSizeUpload = 10 * 1024 * 1024;
constexpr int64_t kMaxConcurrentUpload = 4;
constexpr int64_t kMaxSizeRequest = 10 * 1024 * 1024;
constexpr int64_t kMaxConcurrentRequests = 4;
constexpr int64_t kMaxCallsInRequest = 64;
constexpr int64_t kMaxObjectsInGet = 1024;
constexpr int64_t kMaxObjectsInSet = 1024;

const std::vector<JmapEntry> &Methods() {
	static const std::vector<JmapEntry> table = [] {
		std::vector<JmapEntry> t;
		RegisterCoreMethods(t);
		RegisterMailMethods(t);
		RegisterSubmissionMethods(t);
		return t;
	}();
	return table;
}

JmapMethod Lookup(const std::string &name) {
	for (const auto &e : Methods()) {
		if (name == e.name) {
			return e.fn;
		}
	}
	return nullptr;
}

void SendJson(Ctx &ctx, const js::Value &v, int status) {
	ctx.resp.Bytes(js::Serialize(v), "application/json; charset=utf-8");
	ctx.resp.status = status;
	ctx.resp.SetHeader("Cache-Control", "no-store");
	ctx.resp.SetHeader("X-Content-Type-Options", "nosniff");
}

// A request-level problem, as RFC 8620 §3.6.1 defines it: an RFC 7807 problem
// document rather than a method-level error, because there is no method call to
// attach it to.
void ProblemDetails(Ctx &ctx, const char *type, int status, const std::string &detail) {
	js::Value v = js::Value::MakeObject();
	v.Set("type", std::string("urn:ietf:params:jmap:error:") + type);
	v.Set("status", (int64_t)status);
	if (!detail.empty()) {
		v.Set("detail", detail);
	}
	ctx.resp.Bytes(js::Serialize(v), "application/problem+json; charset=utf-8");
	ctx.resp.status = status;
	ctx.resp.SetHeader("Cache-Control", "no-store");
	ctx.resp.SetHeader("X-Content-Type-Options", "nosniff");
}

// The Session resource. A client is given only a URL and a credential; this is
// what tells it everything else, so anything absent here is a feature the
// client will never look for.
void GetSession(Ctx &ctx) {
	std::string state = AccountState(ctx);
	std::string account = ctx.username;

	js::Value core = js::Value::MakeObject();
	core.Set("maxSizeUpload", kMaxSizeUpload);
	core.Set("maxConcurrentUpload", kMaxConcurrentUpload);
	core.Set("maxSizeRequest", kMaxSizeRequest);
	core.Set("maxConcurrentRequests", kMaxConcurrentRequests);
	core.Set("maxCallsInRequest", kMaxCallsInRequest);
	core.Set("maxObjectsInGet", kMaxObjectsInGet);
	core.Set("maxObjectsInSet", kMaxObjectsInSet);
	core.Set("collationAlgorithms", js::Value::MakeArray());

	js::Value mail = js::Value::MakeObject();
	mail.Set("maxMailboxesPerEmail", js::Value());   // null: no limit
	mail.Set("maxMailboxDepth", js::Value());        // rooms do not nest
	mail.Set("maxSizeMailboxName", (int64_t)128);
	mail.Set("maxSizeAttachmentsPerEmail", (int64_t)(10 * 1024 * 1024));
	mail.Set("emailQuerySortOptions", js::Value::MakeArray());
	// Rooms have no parent, so every mailbox is at the top level. Saying so
	// stops a client from drawing a tree it will never be able to reshape.
	mail.Set("mayCreateTopLevelMailbox", true);

	js::Value submission = js::Value::MakeObject();
	submission.Set("maxDelayedSend", (int64_t)0); // sent when asked, never held
	submission.Set("submissionExtensions", js::Value::MakeObject());

	js::Value capabilities = js::Value::MakeObject();
	capabilities.Set(kCoreCapability, core);
	capabilities.Set(kMailCapability, mail);
	capabilities.Set(kSubmissionCapability, submission);

	js::Value account_caps = js::Value::MakeObject();
	account_caps.Set(kMailCapability, mail);
	account_caps.Set(kSubmissionCapability, submission);

	js::Value acct = js::Value::MakeObject();
	acct.Set("name", account);
	acct.Set("isPersonal", true);
	acct.Set("isReadOnly", false);
	acct.Set("accountCapabilities", account_caps);

	js::Value accounts = js::Value::MakeObject();
	accounts.Set(account, acct);

	js::Value primary = js::Value::MakeObject();
	primary.Set(kMailCapability, account);
	primary.Set(kSubmissionCapability, account);

	js::Value session = js::Value::MakeObject();
	session.Set("capabilities", capabilities);
	session.Set("accounts", accounts);
	session.Set("primaryAccounts", primary);
	session.Set("username", account);
	session.Set("apiUrl", "/jmap/api");
	session.Set("downloadUrl", "/jmap/download/{accountId}/{blobId}/{name}?accept={type}");
	session.Set("uploadUrl", "/jmap/upload/{accountId}");
	// No push. A client polls Email/changes instead, which is what the state
	// strings are for; saying so is how it is told not to open a stream that
	// would never carry anything.
	session.Set("eventSourceUrl", "");
	session.Set("state", state);

	SendJson(ctx, session, 200);
}

// Resolve a back-reference: {"resultOf": "c0", "name": "Email/query", "path":
// "/ids"} pointing at an earlier call's result.
//
// The path is a JSON pointer, and only the shapes JMAP actually uses are
// supported: a member name, an array index, and the "/*/id" form that plucks one
// property out of every element. Anything else is a failure rather than a guess,
// because a silently-empty id list looks to a client like an empty mailbox.
} // namespace

bool ResolveReference(JmapCtx &jc, const js::Value &ref, std::vector<std::string> &out) {
	std::string result_of = ref["resultOf"].AsString();
	std::string name = ref["name"].AsString();
	std::string path = ref["path"].AsString();
	if (result_of.empty() || name.empty() || path.empty()) {
		return false;
	}

	const js::Value *found = nullptr;
	for (size_t i = 0; i < jc.responses.Size(); i++) {
		const js::Value &entry = jc.responses.At(i);
		if (entry.At(0).AsString() == name && entry.At(2).AsString() == result_of) {
			found = &entry.At(1);
		}
	}
	if (!found) {
		return false;
	}

	// Walk the pointer.
	const js::Value *cur = found;
	size_t pos = 0;
	bool wildcard = false;
	std::string after_wildcard;
	while (pos < path.size()) {
		if (path[pos] != '/') {
			return false;
		}
		pos++;
		size_t next = path.find('/', pos);
		std::string token = path.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
		pos = next == std::string::npos ? path.size() : next;
		if (token == "*") {
			wildcard = true;
			after_wildcard = pos < path.size() ? path.substr(pos + 1) : std::string();
			break;
		}
		if (!token.empty() && token.find_first_not_of("0123456789") == std::string::npos &&
		    cur->type == js::Value::Array) {
			size_t idx = (size_t)std::strtoull(token.c_str(), nullptr, 10);
			if (idx >= cur->Size()) {
				return false;
			}
			cur = &cur->At(idx);
			continue;
		}
		const js::Value *next_v = cur->Get(token);
		if (!next_v) {
			return false;
		}
		cur = next_v;
	}

	if (wildcard) {
		if (cur->type != js::Value::Array) {
			return false;
		}
		for (size_t i = 0; i < cur->Size(); i++) {
			const js::Value &item = cur->At(i);
			const js::Value *leaf = after_wildcard.empty() ? &item : item.Get(after_wildcard);
			if (!leaf || leaf->type != js::Value::String) {
				return false;
			}
			out.push_back(leaf->str);
		}
		return true;
	}
	if (cur->type == js::Value::String) {
		out.push_back(cur->str);
		return true;
	}
	if (cur->type != js::Value::Array) {
		return false;
	}
	for (size_t i = 0; i < cur->Size(); i++) {
		if (cur->At(i).type != js::Value::String) {
			return false;
		}
		out.push_back(cur->At(i).str);
	}
	return true;
}

namespace {

void PostApi(Ctx &ctx) {
	if (ctx.req.method != "POST") {
		ctx.resp.SetHeader("Allow", "POST");
		ctx.resp.status = 405;
		ctx.resp.body.clear();
		return;
	}
	if ((int64_t)ctx.req.body.size() > kMaxSizeRequest) {
		ProblemDetails(ctx, "limit", 400, "maxSizeRequest exceeded");
		return;
	}

	js::Value req;
	std::string err;
	if (!js::Parse(ctx.req.body, req, err)) {
		// notJSON and notRequest are different problems, and a client uses the
		// difference to decide whether to retry or to fix itself.
		ProblemDetails(ctx, "notJSON", 400, err);
		return;
	}
	const js::Value &calls = req["methodCalls"];
	if (calls.type != js::Value::Array) {
		ProblemDetails(ctx, "notRequest", 400, "methodCalls must be an array");
		return;
	}
	if ((int64_t)calls.Size() > kMaxCallsInRequest) {
		ProblemDetails(ctx, "limit", 400, "maxCallsInRequest exceeded");
		return;
	}
	// `using` is required, and a client that omits the capability a method
	// belongs to is told so rather than served anyway — otherwise the field is
	// decoration and every client learns to skip it.
	const js::Value &using_v = req["using"];
	bool has_core = false;
	for (size_t i = 0; i < using_v.Size(); i++) {
		if (using_v.At(i).AsString() == kCoreCapability) {
			has_core = true;
		}
	}
	if (!has_core) {
		ProblemDetails(ctx, "unknownCapability", 400,
		               std::string("the request must use ") + kCoreCapability);
		return;
	}

	JmapCtx jc(ctx);
	jc.account = ctx.username;

	for (size_t i = 0; i < calls.Size(); i++) {
		const js::Value &call = calls.At(i);
		std::string name = call.At(0).AsString();
		const js::Value &args = call.At(1);
		std::string call_id = call.At(2).AsString();

		js::Value entry = js::Value::MakeArray();
		JmapMethod fn = Lookup(name);
		if (!fn || call.type != js::Value::Array || call.Size() < 3) {
			// The wrapper MethodError builds is an envelope detail; what goes on
			// the wire is the error object inside it.
			js::Value wrapped = MethodError("unknownMethod");
			entry.Push(js::Value::MakeString("error"));
			entry.Push(wrapped["__error"]);
			entry.Push(js::Value::MakeString(call_id));
			jc.responses.Push(entry);
			continue;
		}

		js::Value result = fn(jc, args);
		// A method returns either its own response object or an error object;
		// the error case is tagged so the envelope can name it "error".
		bool is_error = result.Has("__error");
		entry.Push(js::Value::MakeString(is_error ? "error" : name));
		entry.Push(is_error ? result["__error"] : result);
		entry.Push(js::Value::MakeString(call_id));
		jc.responses.Push(entry);
	}

	js::Value out = js::Value::MakeObject();
	out.Set("methodResponses", jc.responses);
	out.Set("sessionState", AccountState(ctx));
	SendJson(ctx, out, 200);
}

// ---- Core/echo -----------------------------------------------------------

js::Value CoreEcho(JmapCtx &, const js::Value &args) {
	return args;
}

} // namespace

// ---- shared helpers ------------------------------------------------------

std::string IdOf(int64_t n) {
	return std::to_string(n);
}

int64_t IdNum(const std::string &id) {
	// Strictly decimal. strtoll would read "12abc" as 12, and a client that can
	// steer an id into a different row by appending junk to it is an IDOR.
	if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) {
		return -1;
	}
	return (int64_t)std::strtoll(id.c_str(), nullptr, 10);
}

std::string AccountState(Ctx &ctx) {
	int64_t high = 0;
	for (const auto &room : JmapMailboxes(ctx)) {
		int64_t t = quackmail::citadel::RoomChangeToken(ctx.con, room.room_num);
		if (t > high) {
			high = t;
		}
	}
	return std::to_string(high);
}

int64_t AccountStateValue(const std::string &state) {
	if (state.empty() || state.find_first_not_of("0123456789") != std::string::npos) {
		return -1;
	}
	return (int64_t)std::strtoll(state.c_str(), nullptr, 10);
}

js::Value MethodError(const std::string &type, const std::string &description) {
	js::Value e = js::Value::MakeObject();
	e.Set("type", type);
	if (!description.empty()) {
		e.Set("description", description);
	}
	// Wrapped, so the envelope can tell an error apart from a response without
	// every method having to say which it returned.
	js::Value wrapper = js::Value::MakeObject();
	wrapper.Set("__error", e);
	return wrapper;
}

js::Value SetError(const std::string &type, const std::string &description) {
	js::Value e = js::Value::MakeObject();
	e.Set("type", type);
	if (!description.empty()) {
		e.Set("description", description);
	}
	return e;
}

bool CheckAccount(JmapCtx &jc, const js::Value &args, js::Value &err) {
	std::string want = args["accountId"].AsString();
	// An absent accountId means the primary account, which is the only one.
	if (want.empty() || want == jc.account) {
		return true;
	}
	err = MethodError("accountNotFound");
	return false;
}

bool ResolveIds(JmapCtx &jc, const js::Value &args, std::vector<std::string> &out, bool &present) {
	out.clear();
	present = false;
	if (args.Has("ids")) {
		present = true;
		const js::Value &ids = args["ids"];
		if (ids.IsNull()) {
			present = false;
			return true;
		}
		if (ids.type != js::Value::Array) {
			return false;
		}
		for (size_t i = 0; i < ids.Size(); i++) {
			out.push_back(ids.At(i).AsString());
		}
		return true;
	}
	if (args.Has("#ids")) {
		present = true;
		return ResolveReference(jc, args["#ids"], out);
	}
	return true;
}

void RegisterCoreMethods(std::vector<JmapEntry> &out) {
	out.push_back({"Core/echo", CoreEcho});
}

void RegisterJmapRoutes(std::vector<Route> &out) {
	// The Session resource is behind authentication: it names the account and
	// its state, so serving it anonymously would be a user-enumeration oracle.
	// RFC 8620 expects exactly this — a client authenticates, *then* discovers.
	out.push_back({"GET", "/.well-known/jmap", Role::Api, GetSession});
	out.push_back({"GET", "/jmap/session", Role::Api, GetSession});
	out.push_back({"*", "/jmap/api", Role::Api, PostApi});
	RegisterJmapDownloadRoute(out);
	RegisterJmapUploadRoute(out);
}

} // namespace qmweb
} // namespace duckdb
