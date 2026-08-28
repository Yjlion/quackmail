#include "jmap.hpp"

#include "quackmail/quota.hpp"

namespace duckdb {
namespace qmweb {

// RFC 9425, the JMAP Quota extension.
//
// One object, id "storage", counted in *octets* — `resourceType: "octets"` says
// so, and this is the one place the IMAP kibibyte conversion must not be reused.
//
// Quota/query and Quota/queryChanges are deliberately absent. RFC 9425 §2
// requires only /get and /changes; query is optional, and a filter grammar and
// sort order over a one-element collection is machinery no client would ever
// use.

namespace {

const char *const kQuotaId = "storage";

// The Quota object, or a null value when this account has no ceiling.
//
// An unlimited account returns *no* Quota objects at all rather than one with
// some sentinel limit: `hardLimit` is a mandatory UnsignedInt in RFC 9425 with
// no "unlimited" encoding, so inventing one (0? absent?) makes every client's
// percentage arithmetic wrong. Absence is the accurate statement that there is
// no quota, and it is the thing most likely to be "fixed" later by somebody who
// has not read this paragraph.
bool QuotaObject(Ctx &ctx, const js::Value &properties, js::Value &out) {
	auto info = quackmail::quota::Usage(ctx.con, ctx.username);
	if (!info.limited) {
		return false;
	}

	js::Value full = js::Value::MakeObject();
	full.Set("id", js::Value::MakeString(kQuotaId));
	full.Set("resourceType", js::Value::MakeString("octets"));
	full.Set("used", info.used_bytes);
	full.Set("hardLimit", info.limit_bytes);
	full.Set("scope", js::Value::MakeString("account"));
	full.Set("name", js::Value::MakeString(kQuotaId));
	js::Value types = js::Value::MakeArray();
	types.Push(js::Value::MakeString("Mail"));
	full.Set("types", types);
	full.Set("description",
	         js::Value::MakeString("Bytes of stored messages, calendars, contacts and notes"));
	// warnLimit and softLimit are omitted because this server has neither, and
	// a client is entitled to treat their absence as "no warning threshold".

	if (properties.type != js::Value::Array) {
		out = full;
		return true;
	}
	// RFC 8620 §5.1: `properties` narrows the object, but `id` is always there.
	js::Value narrowed = js::Value::MakeObject();
	narrowed.Set("id", js::Value::MakeString(kQuotaId));
	for (size_t i = 0; i < properties.Size(); i++) {
		std::string name = properties.At(i).AsString();
		if (!name.empty() && full.Has(name)) {
			narrowed.Set(name, full[name]);
		}
	}
	out = narrowed;
	return true;
}

// The Quota state: the account state, which moves when stored bytes move,
// joined to the limit generation, which moves when an admin edits the ceiling.
// Neither half alone can answer Quota/changes — usage changes without the limit,
// and the limit changes without a single message being touched.
std::string QuotaState(Ctx &ctx) {
	return AccountState(ctx) + "." +
	       std::to_string(quackmail::quota::LimitGeneration(ctx.con, ctx.username));
}

js::Value QuotaGet(JmapCtx &jc, const js::Value &args) {
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
	js::Value obj;
	bool exists = QuotaObject(ctx, args["properties"], obj);

	if (!have_ids) {
		if (exists) {
			list.Push(obj);
		}
	} else {
		for (const auto &id : ids) {
			if (id == kQuotaId && exists) {
				list.Push(obj);
			} else {
				not_found.Push(js::Value::MakeString(id));
			}
		}
	}

	js::Value out = js::Value::MakeObject();
	out.Set("accountId", jc.account);
	out.Set("state", QuotaState(ctx));
	out.Set("list", list);
	out.Set("notFound", not_found);
	return out;
}

js::Value QuotaChanges(JmapCtx &jc, const js::Value &args) {
	js::Value err;
	if (!CheckAccount(jc, args, err)) {
		return err;
	}
	Ctx &ctx = jc.ctx;
	const js::Value &since = args["sinceState"];
	if (since.type != js::Value::String || since.AsString().empty()) {
		return MethodError("cannotCalculateChanges", "sinceState is required");
	}
	std::string old_state = since.AsString();
	std::string new_state = QuotaState(ctx);

	// With exactly one object the whole calculation is "did anything move, and
	// does the object exist at each end". The limit generation is the second
	// half of the state, so a state that predates the first SetQuota has 0 there
	// and the object is a creation rather than an update.
	auto generation_of = [](const std::string &state) -> int64_t {
		auto dot = state.rfind('.');
		return dot == std::string::npos ? 0 : std::atoll(state.c_str() + dot + 1);
	};

	js::Value created = js::Value::MakeArray();
	js::Value updated = js::Value::MakeArray();
	js::Value destroyed = js::Value::MakeArray();

	if (old_state != new_state) {
		js::Value obj;
		bool exists = QuotaObject(ctx, js::Value(), obj);
		if (!exists) {
			destroyed.Push(js::Value::MakeString(kQuotaId));
		} else if (generation_of(old_state) == 0) {
			created.Push(js::Value::MakeString(kQuotaId));
		} else {
			updated.Push(js::Value::MakeString(kQuotaId));
		}
	}

	js::Value out = js::Value::MakeObject();
	out.Set("accountId", jc.account);
	out.Set("oldState", js::Value::MakeString(old_state));
	out.Set("newState", js::Value::MakeString(new_state));
	out.Set("hasMoreChanges", false);
	out.Set("created", created);
	out.Set("updated", updated);
	out.Set("destroyed", destroyed);
	return out;
}

} // namespace

void RegisterQuotaMethods(std::vector<JmapEntry> &out) {
	out.push_back({"Quota/get", QuotaGet});
	out.push_back({"Quota/changes", QuotaChanges});
}

} // namespace qmweb
} // namespace duckdb
