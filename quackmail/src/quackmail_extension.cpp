#define DUCKDB_EXTENSION_MAIN

#include "quackmail_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include "quackmail/auth.hpp"
#include "quackmail/citadel_store.hpp"
#include "quackmail/dkim.hpp"
#include "quackmail/dmarc.hpp"
#include "quackmail/citadel_msg.hpp"
#include "quackmail/http.hpp"
#include "quackmail/listserv.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mailpolicy.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/psl.hpp"
#include "quackmail/rbl.hpp"
#include "quackmail/sieve.hpp"
#include "quackmail/spf.hpp"
#include "quackmail/util.hpp"

#include <cstdlib>
#include <ctime>
#include <vector>

namespace duckdb {

namespace {

enum class UmbrellaKind {
	VERSION,
	USER_ADD,
	USER_REMOVE,
	STATUS,
	MIME_HEADERS,
	MIME_DECODE_HEADER,
	MIME_DECODE,
	MIME_ADDRESSES,
	PARSE_DATE,
	MIME_PARTS,
	CIT_ROOM_ADD,
	CIT_FLOOR_ADD,
	CIT_ROOM_ACL,
	CIT_ROOM_ACL_SET,
	// Site policy administration — what the deploy/quackcitadm.sh CLI drives.
	DOMAIN_ADD,
	DOMAIN_REMOVE,
	DOMAIN_LIST,
	ALIAS_ADD,
	ALIAS_REMOVE,
	ALIAS_LIST,
	ACL_ADD,
	ACL_REMOVE,
	ACL_LIST,
	RBL_ADD,
	RBL_REMOVE,
	RBL_LIST,
	RBL_CHECK,
	DKIM_KEYGEN,
	DKIM_KEY_ADD,
	DKIM_KEY_REMOVE,
	DKIM_KEY_LIST,
	DKIM_VERIFY,
	RATELIMIT_SET,
	RATELIMIT_LIST,
	RATE_STATUS,
	SPF_CHECK,
	DMARC_CHECK,
	SIEVE_CHECK,
	CONFIG_GET,
	CONFIG_SET,
	CONFIG_LIST,
	// Mailing lists.
	LIST_LIST,
	LIST_CREATE,
	LIST_SET,
	LIST_REMOVE,
	LIST_SUBS,
	LIST_SUB_ADD,
	LIST_SUB_REMOVE,
	LIST_HELD,
	LIST_APPROVE,
	LIST_REJECT,
	LIST_RENDER,
};

struct RowsBindData : public FunctionData {
	UmbrellaKind kind = UmbrellaKind::VERSION;
	vector<string> args;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<RowsBindData>(*this);
	}
	bool Equals(const FunctionData &) const override {
		return false;
	}
};

struct RowsGlobalState : public GlobalTableFunctionState {
	vector<vector<Value>> rows;
	idx_t idx = 0;
};

// ---- mailing-list helpers --------------------------------------------------
// The enums are internal to listserv; these are the names the SQL surface uses.

const char *ListModeName(quackmail::listserv::Mode m) {
	switch (m) {
	case quackmail::listserv::Mode::Digest:
		return "digest";
	case quackmail::listserv::Mode::Both:
		return "both";
	default:
		return "post";
	}
}

const char *ListPolicyName(quackmail::listserv::PostPolicy p) {
	switch (p) {
	case quackmail::listserv::PostPolicy::Anyone:
		return "anyone";
	case quackmail::listserv::PostPolicy::Moderated:
		return "moderated";
	default:
		return "subscribers";
	}
}

const char *ListStateName(quackmail::listserv::SubState s) {
	switch (s) {
	case quackmail::listserv::SubState::Active:
		return "active";
	case quackmail::listserv::SubState::UnsubPending:
		return "unsub_pending";
	default:
		return "pending";
	}
}

// Resolve the room-name argument these functions take into the list on it.
bool ResolveListArg(Connection &con, const std::string &room_name, quackmail::listserv::List &out,
                    std::string &err) {
	quackmail::citadel::Room room;
	if (!quackmail::citadel::ResolveRoom(con, "", room_name, room)) {
		err = "no such public room";
		return false;
	}
	if (!quackmail::listserv::GetList(con, room.room_num, out)) {
		err = "'" + room.display_name + "' is not a mailing list";
		return false;
	}
	return true;
}

unique_ptr<GlobalTableFunctionState> RowsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<RowsBindData>();
	auto gstate = make_uniq<RowsGlobalState>();
	Connection con(*context.db);
	quackmail::store::EnsureSchema(con);

	switch (bind.kind) {
	case UmbrellaKind::VERSION:
		gstate->rows.push_back({Value("QuackCit 0.4.0")});
		break;
	case UmbrellaKind::USER_ADD: {
		std::string err;
		bool ok = quackmail::auth::AddUser(con, bind.args[0], bind.args[1], err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "user added" : err)});
		break;
	}
	case UmbrellaKind::USER_REMOVE: {
		std::string err;
		bool ok = quackmail::auth::RemoveUser(con, bind.args[0], err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "user removed" : err)});
		break;
	}
	case UmbrellaKind::STATUS: {
		auto add_metric = [&](const std::string &name, const std::string &sql) {
			auto r = con.Query(sql);
			int64_t v = (!r->HasError() && r->RowCount() == 1) ? r->GetValue(0, 0).GetValue<int64_t>() : -1;
			gstate->rows.push_back({Value(name), Value::BIGINT(v)});
		};
		add_metric("users", "SELECT count(*) FROM quackmail_users");
		add_metric("floors", "SELECT count(*) FROM citadel_floors");
		add_metric("rooms", "SELECT count(*) FROM citadel_rooms");
		add_metric("messages", "SELECT count(*) FROM citadel_messages");
		add_metric("outbound_queued", "SELECT count(*) FROM quackmail_outbound WHERE status = 'queued'");
		break;
	}
	case UmbrellaKind::CIT_ROOM_ADD: {
		std::string err;
		int64_t num = quackmail::citadel::CreateRoom(con, bind.args[0], 0, 0, "", 0, err);
		gstate->rows.push_back(
		    {Value::BOOLEAN(num >= 0), Value(num >= 0 ? ("room " + std::to_string(num) + " created") : err)});
		break;
	}
	case UmbrellaKind::CIT_ROOM_ACL: {
		quackmail::citadel::Room room;
		if (!quackmail::citadel::ResolveRoom(con, "", bind.args[0], room)) {
			break; // no such public room: an empty listing
		}
		for (auto &e : quackmail::citadel::ListRights(con, room)) {
			gstate->rows.push_back({Value(room.display_name), Value(e.first), Value(e.second)});
		}
		break;
	}
	case UmbrellaKind::CIT_ROOM_ACL_SET: {
		quackmail::citadel::Room room;
		std::string err;
		if (!quackmail::citadel::ResolveRoom(con, "", bind.args[0], room)) {
			gstate->rows.push_back({Value::BOOLEAN(false), Value("no such public room")});
			break;
		}
		bool ok = quackmail::citadel::SetRights(con, room, bind.args[1], bind.args[2], err);
		gstate->rows.push_back(
		    {Value::BOOLEAN(ok),
		     Value(ok ? (bind.args[2].empty() ? ("removed " + bind.args[1])
		                                      : (bind.args[1] + " = " + bind.args[2]))
		              : err)});
		break;
	}
	case UmbrellaKind::CIT_FLOOR_ADD: {
		std::string err;
		int64_t num = quackmail::citadel::CreateFloor(con, bind.args[0], err);
		gstate->rows.push_back(
		    {Value::BOOLEAN(num >= 0), Value(num >= 0 ? ("floor " + std::to_string(num) + " created") : err)});
		break;
	}
	case UmbrellaKind::MIME_HEADERS: {
		auto parsed = quackmail::mime::Parse(bind.args[0]);
		for (auto &h : parsed.headers) {
			gstate->rows.push_back({Value(h.first), Value(h.second)});
		}
		break;
	}
	case UmbrellaKind::MIME_DECODE_HEADER: {
		gstate->rows.push_back({Value(quackmail::mime::DecodeEncodedWords(bind.args[0]))});
		break;
	}
	case UmbrellaKind::MIME_DECODE: {
		gstate->rows.push_back(
		    {Value(quackmail::mime::DecodeContentTransferEncoding(bind.args[0], bind.args[1]))});
		break;
	}
	case UmbrellaKind::MIME_ADDRESSES: {
		for (auto &a : quackmail::mime::ParseAddressList(bind.args[0])) {
			gstate->rows.push_back({Value(a.name), Value(a.addr)});
		}
		break;
	}
	case UmbrellaKind::PARSE_DATE: {
		int64_t epoch = 0;
		if (quackmail::mime::ParseDate(bind.args[0], epoch)) {
			std::time_t t = static_cast<std::time_t>(epoch);
			std::tm tm_utc{};
#if defined(_WIN32)
			gmtime_s(&tm_utc, &t);
#else
			gmtime_r(&t, &tm_utc);
#endif
			char buf[32];
			std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
			gstate->rows.push_back({Value::BIGINT(epoch), Value(std::string(buf))});
		} else {
			gstate->rows.push_back({Value(LogicalType::BIGINT), Value(LogicalType::VARCHAR)});
		}
		break;
	}
	case UmbrellaKind::MIME_PARTS: {
		auto root = quackmail::mime::ParseEntity(bind.args[0]);
		for (auto &p : quackmail::mime::FlattenParts(root)) {
			gstate->rows.push_back({Value(p.section), Value(p.content_type), Value(p.charset),
			                        Value(p.encoding), Value::BIGINT(p.size_bytes), Value(p.filename),
			                        Value(p.content)});
		}
		break;
	}

	// ---- domains --------------------------------------------------------
	case UmbrellaKind::DOMAIN_ADD: {
		std::string err;
		bool ok = quackmail::policy::AddDomain(con, bind.args[0], bind.args[1], err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "domain added" : err)});
		break;
	}
	case UmbrellaKind::DOMAIN_REMOVE: {
		std::string err;
		bool ok = quackmail::policy::RemoveDomain(con, bind.args[0], err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "domain removed" : err)});
		break;
	}
	case UmbrellaKind::DOMAIN_LIST: {
		for (auto &d : quackmail::policy::ListDomains(con)) {
			gstate->rows.push_back({Value(d.domain), Value(d.kind), Value::BOOLEAN(d.enabled),
			                        Value(d.dkim_selector), Value(d.note)});
		}
		break;
	}

	// ---- aliases --------------------------------------------------------
	case UmbrellaKind::ALIAS_ADD: {
		std::string err;
		bool ok = quackmail::policy::AddAlias(con, bind.args[0], bind.args[1], err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "alias added" : err)});
		break;
	}
	case UmbrellaKind::ALIAS_REMOVE: {
		std::string err;
		bool ok = quackmail::policy::RemoveAlias(con, bind.args[0], bind.args[1], err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "alias removed" : err)});
		break;
	}
	case UmbrellaKind::ALIAS_LIST: {
		auto r = con.Query("SELECT alias, destination, enabled FROM quackmail_aliases ORDER BY alias");
		if (!r->HasError()) {
			for (idx_t i = 0; i < r->RowCount(); i++) {
				gstate->rows.push_back({r->GetValue(0, i), r->GetValue(1, i), r->GetValue(2, i)});
			}
		}
		break;
	}

	// ---- access control -------------------------------------------------
	case UmbrellaKind::ACL_ADD: {
		std::string err;
		bool ok = quackmail::policy::AddAcl(con, bind.args[0], bind.args[1], bind.args[2],
		                                    bind.args.size() > 3 ? bind.args[3] : "", err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "rule added" : err)});
		break;
	}
	case UmbrellaKind::ACL_REMOVE: {
		std::string err;
		bool ok = quackmail::policy::RemoveAcl(con, std::atoll(bind.args[0].c_str()), err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "rule removed" : err)});
		break;
	}
	case UmbrellaKind::ACL_LIST: {
		auto r = con.Query("SELECT id, scope, pattern, action, enabled, note FROM quackmail_acl "
		                   "ORDER BY id");
		if (!r->HasError()) {
			for (idx_t i = 0; i < r->RowCount(); i++) {
				gstate->rows.push_back({r->GetValue(0, i), r->GetValue(1, i), r->GetValue(2, i),
				                        r->GetValue(3, i), r->GetValue(4, i), r->GetValue(5, i)});
			}
		}
		break;
	}

	// ---- DNSBL ----------------------------------------------------------
	case UmbrellaKind::RBL_ADD: {
		std::string err;
		bool ok = quackmail::policy::AddRblZone(con, bind.args[0], err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "zone added" : err)});
		break;
	}
	case UmbrellaKind::RBL_REMOVE: {
		std::string err;
		bool ok = quackmail::policy::RemoveRblZone(con, bind.args[0], err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "zone removed" : err)});
		break;
	}
	case UmbrellaKind::RBL_LIST: {
		for (auto &z : quackmail::policy::RblZones(con)) {
			gstate->rows.push_back({Value(z)});
		}
		break;
	}
	case UmbrellaKind::RBL_CHECK: {
		quackmail::rbl::Hit hit;
		quackmail::rbl::Check(bind.args[0], quackmail::policy::RblZones(con), hit);
		gstate->rows.push_back(
		    {Value::BOOLEAN(hit.listed), Value(hit.zone), Value(hit.code), Value(hit.reason)});
		break;
	}

	// ---- DKIM -----------------------------------------------------------
	case UmbrellaKind::DKIM_KEYGEN: {
		std::string dns_record, err;
		int bits = bind.args.size() > 2 ? std::atoi(bind.args[2].c_str()) : 2048;
		bool ok = quackmail::policy::GenerateDkimKey(con, bind.args[0], bind.args[1], bits, dns_record,
		                                             err);
		// The DNS name is returned alongside the record so the value can be
		// pasted straight into a zone file.
		gstate->rows.push_back({Value::BOOLEAN(ok),
		                        Value(bind.args[1] + "._domainkey." + bind.args[0]),
		                        Value(ok ? dns_record : err)});
		break;
	}
	case UmbrellaKind::DKIM_KEY_ADD: {
		std::string err;
		bool ok = quackmail::policy::AddDkimKey(con, bind.args[0], bind.args[1], bind.args[2], err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "key stored" : err)});
		break;
	}
	case UmbrellaKind::DKIM_KEY_REMOVE: {
		std::string err;
		bool ok = quackmail::policy::RemoveDkimKey(con, bind.args[0], bind.args[1], err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "key removed" : err)});
		break;
	}
	case UmbrellaKind::DKIM_KEY_LIST: {
		// The private half is deliberately never returned here.
		for (auto &k : quackmail::policy::ListDkimKeys(con)) {
			gstate->rows.push_back({Value(k.domain), Value(k.selector), Value(k.algo),
			                        Value::BOOLEAN(k.enabled),
			                        Value(quackmail::dkim::DnsRecord(k.public_key))});
		}
		break;
	}
	case UmbrellaKind::DKIM_VERIFY: {
		auto results = quackmail::dkim::Verify(bind.args[0], quackmail::policy::DkimKeyLookup(con));
		if (results.empty()) {
			gstate->rows.push_back({Value("none"), Value(""), Value(""), Value("no DKIM-Signature")});
		}
		for (auto &r : results) {
			gstate->rows.push_back({Value(quackmail::dkim::ResultName(r.result)), Value(r.domain),
			                        Value(r.selector), Value(r.info)});
		}
		break;
	}

	// ---- rate limits ----------------------------------------------------
	case UmbrellaKind::RATELIMIT_SET: {
		std::string err;
		bool ok = quackmail::policy::SetRateLimit(con, bind.args[0], std::atoll(bind.args[1].c_str()),
		                                          std::atoll(bind.args[2].c_str()),
		                                          std::atoll(bind.args[3].c_str()), err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "rate limit set" : err)});
		break;
	}
	case UmbrellaKind::RATELIMIT_LIST: {
		for (auto &rl : quackmail::policy::ListRateLimits(con)) {
			gstate->rows.push_back({Value(rl.username.empty() ? "(default)" : rl.username),
			                        Value::BIGINT(rl.burst_max), Value::BIGINT(rl.burst_secs),
			                        Value::BIGINT(rl.daily_max), Value::BOOLEAN(rl.enabled)});
		}
		break;
	}
	case UmbrellaKind::RATE_STATUS: {
		// CheckRate always charges at least one message, so `allowed` answers
		// "could this user send right now?" rather than "are they exactly at
		// the limit?" — which is the question an admin is actually asking.
		auto v = quackmail::policy::CheckRate(con, bind.args[0], 0);
		gstate->rows.push_back({Value(bind.args[0]), Value::BIGINT(v.burst_used),
		                        Value::BIGINT(v.limit.burst_max), Value::BIGINT(v.daily_used),
		                        Value::BIGINT(v.limit.daily_max), Value::BOOLEAN(v.allowed),
		                        Value(v.reason)});
		break;
	}

	// ---- diagnostics ----------------------------------------------------
	case UmbrellaKind::SPF_CHECK: {
		quackmail::spf::Eval eval;
		quackmail::spf::Check(bind.args[0], bind.args[1], bind.args[2], eval);
		gstate->rows.push_back({Value(quackmail::spf::ResultName(eval.result)), Value(eval.domain),
		                        Value(eval.explanation), Value(eval.record)});
		break;
	}
	case UmbrellaKind::DMARC_CHECK: {
		// No SPF/DKIM input: this reports the published policy for a domain,
		// which is what an admin wants when checking their own DNS.
		std::vector<quackmail::dkim::VerifyResult> none;
		auto eval = quackmail::dmarc::Evaluate(bind.args[0], "", quackmail::spf::Result::None, none);
		gstate->rows.push_back({Value(quackmail::dmarc::ResultName(eval.result)),
		                        Value(quackmail::dmarc::PolicyName(eval.policy)),
		                        Value(eval.policy_domain), Value(eval.record), Value(eval.info)});
		break;
	}
	case UmbrellaKind::SIEVE_CHECK: {
		std::string err;
		bool ok = quackmail::sieve::Check(bind.args[0], err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "script is valid" : err)});
		break;
	}

	// ---- config ---------------------------------------------------------
	case UmbrellaKind::CONFIG_GET: {
		gstate->rows.push_back(
		    {Value(bind.args[0]), Value(quackmail::citadel::GetConfig(con, bind.args[0], ""))});
		break;
	}
	case UmbrellaKind::CONFIG_SET: {
		auto stmt = con.Prepare("INSERT INTO citadel_config (name, value) VALUES ($1, $2) "
		                        "ON CONFLICT (name) DO UPDATE SET value = excluded.value");
		bool ok = !stmt->HasError();
		if (ok) {
			vector<Value> params = {Value(bind.args[0]), Value(bind.args[1])};
			auto r = stmt->Execute(params, false);
			ok = !r->HasError();
		}
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "config set" : "could not set config")});
		break;
	}
	case UmbrellaKind::CONFIG_LIST: {
		auto r = con.Query("SELECT name, value FROM citadel_config ORDER BY name");
		if (!r->HasError()) {
			for (idx_t i = 0; i < r->RowCount(); i++) {
				gstate->rows.push_back({r->GetValue(0, i), r->GetValue(1, i)});
			}
		}
		break;
	}

	// ---- mailing lists --------------------------------------------------
	//
	// Lists are named by room in these functions, because that is what a list
	// *is*. The room is given by display name so the CLI reads the same way
	// `cit_room_acl` does.
	case UmbrellaKind::LIST_LIST: {
		for (auto &l : quackmail::listserv::ListLists(con)) {
			int64_t active = 0, pending = 0;
			for (auto &s : quackmail::listserv::Subscribers(con, l.room_num, "")) {
				(s.state == quackmail::listserv::SubState::Active ? active : pending)++;
			}
			gstate->rows.push_back(
			    {Value::BIGINT(l.room_num), Value(l.display_name),
			     Value(quackmail::listserv::ListAddress(con, l)), Value::BOOLEAN(l.enabled),
			     Value(ListModeName(l.mode)), Value(ListPolicyName(l.post_policy)),
			     Value::BIGINT(active), Value::BIGINT(pending), Value::BIGINT(l.last_sent)});
		}
		break;
	}
	case UmbrellaKind::LIST_CREATE: {
		quackmail::citadel::Room room;
		std::string err;
		if (!quackmail::citadel::ResolveRoom(con, "", bind.args[0], room)) {
			gstate->rows.push_back({Value::BOOLEAN(false), Value("no such public room")});
			break;
		}
		quackmail::listserv::List l;
		l.room_num = room.room_num;
		l.address = bind.args.size() > 1 ? bind.args[1] : "";
		bool ok = quackmail::listserv::SetList(con, l, err);
		if (ok) {
			quackmail::listserv::GetList(con, room.room_num, l);
		}
		gstate->rows.push_back(
		    {Value::BOOLEAN(ok),
		     Value(ok ? ("list created at " + quackmail::listserv::ListAddress(con, l)) : err)});
		break;
	}
	case UmbrellaKind::LIST_SET: {
		quackmail::citadel::Room room;
		std::string err;
		if (!quackmail::citadel::ResolveRoom(con, "", bind.args[0], room)) {
			gstate->rows.push_back({Value::BOOLEAN(false), Value("no such public room")});
			break;
		}
		bool ok = quackmail::listserv::SetField(con, room.room_num, bind.args[1], bind.args[2], err);
		gstate->rows.push_back(
		    {Value::BOOLEAN(ok), Value(ok ? (bind.args[1] + " = " + bind.args[2]) : err)});
		break;
	}
	case UmbrellaKind::LIST_REMOVE: {
		quackmail::citadel::Room room;
		std::string err;
		if (!quackmail::citadel::ResolveRoom(con, "", bind.args[0], room)) {
			gstate->rows.push_back({Value::BOOLEAN(false), Value("no such public room")});
			break;
		}
		bool ok = quackmail::listserv::RemoveList(con, room.room_num, err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "list removed" : err)});
		break;
	}
	case UmbrellaKind::LIST_SUBS: {
		quackmail::citadel::Room room;
		if (!quackmail::citadel::ResolveRoom(con, "", bind.args[0], room)) {
			break; // no such room: an empty listing
		}
		for (auto &s : quackmail::listserv::Subscribers(con, room.room_num, "")) {
			gstate->rows.push_back({Value(s.address),
			                        Value(s.kind == quackmail::listserv::SubKind::Digest ? "digest" : "post"),
			                        Value(ListStateName(s.state)), Value::BIGINT(s.created_at),
			                        Value::BIGINT(s.confirmed_at)});
		}
		break;
	}
	case UmbrellaKind::LIST_SUB_ADD: {
		quackmail::listserv::List l;
		std::string err;
		if (!ResolveListArg(con, bind.args[0], l, err)) {
			gstate->rows.push_back({Value::BOOLEAN(false), Value(err)});
			break;
		}
		auto kind = (bind.args.size() > 2 && quackmail::util::Lower(bind.args[2]) == "digest")
		                ? quackmail::listserv::SubKind::Digest
		                : quackmail::listserv::SubKind::Post;
		std::string token;
		// An aide adding a subscriber confirms it outright: they are asserting
		// the address belongs on the list, which is exactly what the mail
		// confirmation loop exists to establish for anyone else.
		bool ok = quackmail::listserv::Subscribe(con, l, bind.args[1], kind, true, token, err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? (bind.args[1] + " subscribed") : err)});
		break;
	}
	case UmbrellaKind::LIST_SUB_REMOVE: {
		quackmail::listserv::List l;
		std::string err;
		if (!ResolveListArg(con, bind.args[0], l, err)) {
			gstate->rows.push_back({Value::BOOLEAN(false), Value(err)});
			break;
		}
		std::string token;
		bool ok = quackmail::listserv::Unsubscribe(con, l, bind.args[1], true, token, err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? (bind.args[1] + " unsubscribed") : err)});
		break;
	}
	case UmbrellaKind::LIST_HELD: {
		int64_t room_num = -1;
		quackmail::citadel::Room room;
		if (!bind.args[0].empty() && quackmail::citadel::ResolveRoom(con, "", bind.args[0], room)) {
			room_num = room.room_num;
		}
		for (auto &h : quackmail::listserv::HeldMessages(con, room_num, "held")) {
			gstate->rows.push_back({Value::BIGINT(h.id), Value::BIGINT(h.room_num), Value(h.mail_from),
			                        Value(h.subject), Value::BIGINT(h.received_at), Value(h.state)});
		}
		break;
	}
	case UmbrellaKind::LIST_APPROVE: {
		std::string err;
		bool ok = quackmail::listserv::Approve(con, std::atoll(bind.args[0].c_str()), err);
		gstate->rows.push_back(
		    {Value::BOOLEAN(ok),
		     Value(ok ? "posted to the room; the spooler will distribute it" : err)});
		break;
	}
	case UmbrellaKind::LIST_REJECT: {
		std::string err;
		bool ok = quackmail::listserv::Reject(con, std::atoll(bind.args[0].c_str()), err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "rejected" : err)});
		break;
	}
	case UmbrellaKind::LIST_RENDER: {
		// The copy a subscriber would receive for one stored message. Exists so
		// the header rewriting can be asserted from sqllogictest without a
		// socket, a subscriber or a queue.
		quackmail::listserv::List l;
		std::string err;
		quackmail::citadel::Message msg;
		if (!ResolveListArg(con, bind.args[0], l, err)) {
			gstate->rows.push_back({Value(LogicalType::VARCHAR), Value(err)});
			break;
		}
		int64_t msgnum = std::atoll(bind.args[1].c_str());
		if (!quackmail::citadel::LoadMessage(con, msgnum, msg)) {
			gstate->rows.push_back({Value(LogicalType::VARCHAR), Value("no such message")});
			break;
		}
		gstate->rows.push_back({Value(quackmail::listserv::RenderForList(con, l, msg)), Value("ok")});
		break;
	}
	}
	return std::move(gstate);
}

void RowsFunc(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &g = data.global_state->Cast<RowsGlobalState>();
	idx_t count = 0;
	while (g.idx < g.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = g.rows[g.idx];
		for (idx_t c = 0; c < row.size(); c++) {
			output.SetValue(c, count, row[c]);
		}
		g.idx++;
		count++;
	}
	output.SetCardinality(count);
}

unique_ptr<FunctionData> VersionBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                     vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::VERSION;
	names = {"version"};
	return_types = {LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> UserAddBind(ClientContext &, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::USER_ADD;
	b->args = {input.inputs[0].ToString(), input.inputs[1].ToString()};
	names = {"ok", "note"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> UserRemoveBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::USER_REMOVE;
	b->args = {input.inputs[0].ToString()};
	names = {"ok", "note"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> StatusBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                    vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::STATUS;
	names = {"metric", "value"};
	return_types = {LogicalType::VARCHAR, LogicalType::BIGINT};
	return std::move(b);
}

unique_ptr<FunctionData> MimeHeadersBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::MIME_HEADERS;
	b->args = {input.inputs[0].ToString()};
	names = {"name", "value"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> MimeDecodeHeaderBind(ClientContext &, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::MIME_DECODE_HEADER;
	b->args = {input.inputs[0].ToString()};
	names = {"decoded"};
	return_types = {LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> MimeDecodeBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::MIME_DECODE;
	b->args = {input.inputs[0].ToString(), input.inputs[1].ToString()};
	names = {"decoded"};
	return_types = {LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> MimeAddressesBind(ClientContext &, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::MIME_ADDRESSES;
	b->args = {input.inputs[0].ToString()};
	names = {"name", "address"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> ParseDateBind(ClientContext &, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::PARSE_DATE;
	b->args = {input.inputs[0].ToString()};
	names = {"epoch", "iso"};
	return_types = {LogicalType::BIGINT, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> MimePartsBind(ClientContext &, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::MIME_PARTS;
	b->args = {input.inputs[0].ToString()};
	names = {"section", "content_type", "charset", "encoding", "size_bytes", "filename", "content"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> CitRoomAddBind(ClientContext &, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::CIT_ROOM_ADD;
	b->args = {input.inputs[0].ToString()};
	names = {"ok", "note"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> CitRoomAclBind(ClientContext &, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::CIT_ROOM_ACL;
	b->args = {input.inputs[0].ToString()};
	names = {"room", "identifier", "rights"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> CitRoomAclSetBind(ClientContext &, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::CIT_ROOM_ACL_SET;
	b->args = {input.inputs[0].ToString(), input.inputs[1].ToString(), input.inputs[2].ToString()};
	names = {"ok", "note"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR};
	return std::move(b);
}

unique_ptr<FunctionData> CitFloorAddBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::CIT_FLOOR_ADD;
	b->args = {input.inputs[0].ToString()};
	names = {"ok", "note"};
	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR};
	return std::move(b);
}

// ---------------------------------------------------------------------------
// Policy administration: one generic bind for all of it.
//
// These functions differ only in their kind and their result columns, so the
// shape is carried on the TableFunction's function_info rather than repeated in
// a bind function each. Every positional argument becomes a string in `args`.
// ---------------------------------------------------------------------------

struct PolicyInfo : public TableFunctionInfo {
	UmbrellaKind kind = UmbrellaKind::VERSION;
	vector<string> column_names;
	vector<LogicalType> column_types;
};

unique_ptr<FunctionData> PolicyBind(ClientContext &, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto info = reinterpret_cast<PolicyInfo *>(input.info.get());
	auto b = make_uniq<RowsBindData>();
	b->kind = info->kind;
	for (auto &in : input.inputs) {
		b->args.push_back(in.IsNull() ? std::string() : in.ToString());
	}
	names = info->column_names;
	return_types = info->column_types;
	return std::move(b);
}

// Register one policy function with explicit parameter types. Numeric
// arguments are declared BIGINT rather than VARCHAR so that hand-written SQL
// reads naturally -- DuckDB will not implicitly convert an INTEGER literal to
// VARCHAR, so a VARCHAR signature would force qm_ratelimit_set('a','5','60').
// The shell CLI casts its bound string parameters at the call site.
void RegisterPolicyFn(ExtensionLoader &loader, const std::string &name, UmbrellaKind kind,
                      vector<LogicalType> params, vector<string> column_names,
                      vector<LogicalType> column_types) {
	TableFunction f(name, params, RowsFunc, PolicyBind, RowsInit);
	auto info = make_shared_ptr<PolicyInfo>();
	info->kind = kind;
	info->column_names = std::move(column_names);
	info->column_types = std::move(column_types);
	f.function_info = std::move(info);
	loader.RegisterFunction(f);
}

// The two column shapes most of these share.
const vector<string> kOkNote = {"ok", "note"};
const vector<LogicalType> kOkNoteTypes = {LogicalType::BOOLEAN, LogicalType::VARCHAR};

// ---------------------------------------------------------------------------
// Scalar DKIM helpers.
//
// DuckDB requires table-function arguments to be constant-foldable, so the
// table-function forms cannot be composed — `qm_dkim_verify(qm_dkim_sign(...))`
// is only expressible if both are scalar. These are the composable versions;
// qm_dkim_verify_detail below still gives the per-signature breakdown.
// ---------------------------------------------------------------------------

// These need a database connection to reach the key table. The handle is
// captured at bind time and read back through the bound expression, which is
// the stable way for a scalar function to get at it.
struct DbBindData : public FunctionData {
	DatabaseInstance *db = nullptr;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<DbBindData>(*this);
	}
	bool Equals(const FunctionData &other) const override {
		return db == other.Cast<DbBindData>().db;
	}
};

unique_ptr<FunctionData> DbBind(ClientContext &context, ScalarFunction &,
                                vector<unique_ptr<Expression>> &) {
	auto b = make_uniq<DbBindData>();
	b->db = context.db.get();
	return std::move(b);
}

DatabaseInstance &BoundDb(ExpressionState &state) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	return *func_expr.bind_info->Cast<DbBindData>().db;
}

// qm_dkim_sign(message, domain) -> the message with a DKIM-Signature prepended,
// or NULL when no key is configured for the domain or signing fails.
void DkimSignScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	// Read-only: the schema already exists (LoadInternal creates it), and
	// running DDL from inside an executing query would start a write
	// transaction underneath the one already in flight.
	Connection con(BoundDb(state));

	BinaryExecutor::ExecuteWithNulls<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t raw, string_t domain, ValidityMask &mask, idx_t idx) {
		    std::string message = raw.GetString();
		    std::string dom = domain.GetString();
		    quackmail::policy::DkimKey key;
		    if (!quackmail::policy::DkimKeyFor(con, dom, key) || key.private_key.empty()) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    std::string out, err;
		    if (!quackmail::dkim::Sign(message, dom, key.selector, key.private_key, key.headers, out,
		                               err)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, out);
	    });
}

// qm_dkim_verify(message) -> the overall result keyword. A message with several
// signatures passes if any one of them does, which is what RFC 6376 §6.1 says
// a verifier should conclude.
void DkimVerifyScalar(DataChunk &args, ExpressionState &state, Vector &result) {
	Connection con(BoundDb(state)); // read-only; see DkimSignScalar
	auto lookup = quackmail::policy::DkimKeyLookup(con);

	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t raw) {
		auto results = quackmail::dkim::Verify(raw.GetString(), lookup);
		if (results.empty()) {
			return StringVector::AddString(result, "none");
		}
		for (auto &r : results) {
			if (r.result == quackmail::dkim::Result::Pass) {
				return StringVector::AddString(result, "pass");
			}
		}
		return StringVector::AddString(result, quackmail::dkim::ResultName(results[0].result));
	});
}

// qm_sieve_valid(script) -> whether the script parses. Scalar so it composes
// with a table of scripts.
void SieveValidScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](string_t script) {
		std::string err;
		return quackmail::sieve::Check(script.GetString(), err);
	});
}

// qm_psl_org_domain(domain) -> the DMARC organizational (registrable) domain,
// from the bundled Public Suffix List. Scalar and touching no tables, so it is
// assertable straight from sqllogictest with no DNS in the loop — which is the
// only test coverage DMARC alignment can practically get offline.
void PslOrgDomainScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t in) {
		return StringVector::AddString(result, quackmail::dmarc::OrganizationalDomain(in.GetString()));
	});
}

// qm_psl_suffix(domain) -> just the public suffix ("co.uk" for "bbc.co.uk").
void PslSuffixScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t in) {
		return StringVector::AddString(result, quackmail::psl::PublicSuffix(in.GetString()));
	});
}

// The web front-end's pure codecs, exposed so they can be asserted from
// sqllogictest with no socket in the loop. These are the functions an escaping
// or decoding bug would turn into an XSS or a path traversal, so they are worth
// table-driven tests of their own.
void UrlEncodeScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t in) {
		return StringVector::AddString(result, quackmail::http::PercentEncode(in.GetString()));
	});
}

void UrlDecodeScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::Execute<string_t, bool, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t in, bool plus_as_space) {
		    return StringVector::AddString(result,
		                                   quackmail::http::PercentDecode(in.GetString(), plus_as_space));
	    });
}

void HtmlEscapeScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t in) {
		return StringVector::AddString(result, quackmail::http::EscapeHtml(in.GetString()));
	});
}

// qm_url_path(target) -> the normalized path, or NULL if the path is one we
// refuse to serve (traversal, control characters, no leading slash).
void UrlPathScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t in, ValidityMask &mask, idx_t idx) {
		    std::string out;
		    if (!quackmail::http::NormalizePath(quackmail::http::PercentDecode(in.GetString(), false), out)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, out);
	    });
}

void LoadInternal(ExtensionLoader &loader) {
	// Ensure the shared schema exists as soon as the umbrella loads.
	Connection con(loader.GetDatabaseInstance());
	quackmail::store::EnsureSchema(con);

	loader.RegisterFunction(TableFunction("qm_version", {}, RowsFunc, VersionBind, RowsInit));
	loader.RegisterFunction(
	    TableFunction("qm_user_add", {LogicalType::VARCHAR, LogicalType::VARCHAR}, RowsFunc, UserAddBind, RowsInit));
	loader.RegisterFunction(
	    TableFunction("qm_user_remove", {LogicalType::VARCHAR}, RowsFunc, UserRemoveBind, RowsInit));
	loader.RegisterFunction(TableFunction("qm_status", {}, RowsFunc, StatusBind, RowsInit));

	// Citadel admin: create a public room / a floor.
	loader.RegisterFunction(
	    TableFunction("cit_room_add", {LogicalType::VARCHAR}, RowsFunc, CitRoomAddBind, RowsInit));
	loader.RegisterFunction(
	    TableFunction("cit_floor_add", {LogicalType::VARCHAR}, RowsFunc, CitFloorAddBind, RowsInit));

	// Room access control (RFC 4314). Granting "anyone" the `p` right is what
	// makes a room reachable at room_<name>@<fqdn>.
	loader.RegisterFunction(
	    TableFunction("cit_room_acl", {LogicalType::VARCHAR}, RowsFunc, CitRoomAclBind, RowsInit));
	loader.RegisterFunction(TableFunction("cit_room_acl_set",
	                                      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                      RowsFunc, CitRoomAclSetBind, RowsInit));

	// MIME / message-format helpers (RFC 2045-2049, 822/2822/5322).
	loader.RegisterFunction(
	    TableFunction("qm_mime_headers", {LogicalType::VARCHAR}, RowsFunc, MimeHeadersBind, RowsInit));
	loader.RegisterFunction(TableFunction("qm_mime_decode_header", {LogicalType::VARCHAR}, RowsFunc,
	                                      MimeDecodeHeaderBind, RowsInit));
	loader.RegisterFunction(TableFunction("qm_mime_decode", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                      RowsFunc, MimeDecodeBind, RowsInit));
	loader.RegisterFunction(
	    TableFunction("qm_mime_addresses", {LogicalType::VARCHAR}, RowsFunc, MimeAddressesBind, RowsInit));
	loader.RegisterFunction(
	    TableFunction("qm_parse_date", {LogicalType::VARCHAR}, RowsFunc, ParseDateBind, RowsInit));
	loader.RegisterFunction(
	    TableFunction("qm_mime_parts", {LogicalType::VARCHAR}, RowsFunc, MimePartsBind, RowsInit));

	// ---- site policy administration (driven by deploy/quackcitadm.sh) ----
	const auto V = LogicalType::VARCHAR;
	const auto B = LogicalType::BOOLEAN;
	const auto I = LogicalType::BIGINT;

	// Hosted domains.
	RegisterPolicyFn(loader, "qm_domain_add", UmbrellaKind::DOMAIN_ADD, {V, V}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_domain_remove", UmbrellaKind::DOMAIN_REMOVE, {V}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_domains", UmbrellaKind::DOMAIN_LIST, {},
	                 {"domain", "kind", "enabled", "dkim_selector", "note"}, {V, V, B, V, V});

	// Aliases and catch-alls.
	RegisterPolicyFn(loader, "qm_alias_add", UmbrellaKind::ALIAS_ADD, {V, V}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_alias_remove", UmbrellaKind::ALIAS_REMOVE, {V, V}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_aliases", UmbrellaKind::ALIAS_LIST, {},
	                 {"alias", "destination", "enabled"}, {V, V, B});

	// Allow / block lists.
	RegisterPolicyFn(loader, "qm_acl_add", UmbrellaKind::ACL_ADD, {V, V, V, V}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_acl_remove", UmbrellaKind::ACL_REMOVE, {I}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_acl", UmbrellaKind::ACL_LIST, {},
	                 {"id", "scope", "pattern", "action", "enabled", "note"}, {I, V, V, V, B, V});

	// DNSBL zones.
	RegisterPolicyFn(loader, "qm_rbl_add", UmbrellaKind::RBL_ADD, {V}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_rbl_remove", UmbrellaKind::RBL_REMOVE, {V}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_rbl_zones", UmbrellaKind::RBL_LIST, {}, {"zone"}, {V});
	RegisterPolicyFn(loader, "qm_rbl_check", UmbrellaKind::RBL_CHECK, {V},
	                 {"listed", "zone", "code", "reason"}, {B, V, V, V});

	// DKIM keys and diagnostics.
	RegisterPolicyFn(loader, "qm_dkim_keygen", UmbrellaKind::DKIM_KEYGEN, {V, V, I},
	                 {"ok", "dns_name", "dns_record"}, {B, V, V});
	RegisterPolicyFn(loader, "qm_dkim_key_add", UmbrellaKind::DKIM_KEY_ADD, {V, V, V}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_dkim_key_remove", UmbrellaKind::DKIM_KEY_REMOVE, {V, V}, kOkNote,
	                 kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_dkim_keys", UmbrellaKind::DKIM_KEY_LIST, {},
	                 {"domain", "selector", "algo", "enabled", "dns_record"}, {V, V, V, B, V});
	RegisterPolicyFn(loader, "qm_dkim_verify_detail", UmbrellaKind::DKIM_VERIFY, {V},
	                 {"result", "domain", "selector", "info"}, {V, V, V, V});

	// Scalar forms, so signing and verifying can be composed in one query.
	loader.RegisterFunction(ScalarFunction("qm_dkim_sign", {V, V}, V, DkimSignScalar, DbBind));
	loader.RegisterFunction(ScalarFunction("qm_dkim_verify", {V}, V, DkimVerifyScalar, DbBind));
	// Parsing needs no database access.
	loader.RegisterFunction(ScalarFunction("qm_sieve_valid", {V}, B, SieveValidScalar));

	// Public Suffix List lookups (pure).
	loader.RegisterFunction(ScalarFunction("qm_psl_org_domain", {V}, V, PslOrgDomainScalar));
	loader.RegisterFunction(ScalarFunction("qm_psl_suffix", {V}, V, PslSuffixScalar));

	// Web codecs (pure; see the scalars above).
	loader.RegisterFunction(ScalarFunction("qm_url_encode", {V}, V, UrlEncodeScalar));
	loader.RegisterFunction(ScalarFunction("qm_url_decode", {V, B}, V, UrlDecodeScalar));
	loader.RegisterFunction(ScalarFunction("qm_html_escape", {V}, V, HtmlEscapeScalar));
	loader.RegisterFunction(ScalarFunction("qm_url_path", {V}, V, UrlPathScalar));

	// Per-user send quotas.
	RegisterPolicyFn(loader, "qm_ratelimit_set", UmbrellaKind::RATELIMIT_SET, {V, I, I, I}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_ratelimits", UmbrellaKind::RATELIMIT_LIST, {},
	                 {"username", "burst_max", "burst_secs", "daily_max", "enabled"},
	                 {V, I, I, I, B});
	RegisterPolicyFn(loader, "qm_rate_status", UmbrellaKind::RATE_STATUS, {V},
	                 {"username", "burst_used", "burst_max", "daily_used", "daily_max", "allowed",
	                  "note"},
	                 {V, I, I, I, I, B, V});

	// Diagnostics an admin runs against live DNS.
	RegisterPolicyFn(loader, "qm_spf_check", UmbrellaKind::SPF_CHECK, {V, V, V},
	                 {"result", "domain", "explanation", "record"}, {V, V, V, V});
	RegisterPolicyFn(loader, "qm_dmarc_check", UmbrellaKind::DMARC_CHECK, {V},
	                 {"result", "policy", "policy_domain", "record", "info"}, {V, V, V, V, V});
	RegisterPolicyFn(loader, "qm_sieve_check", UmbrellaKind::SIEVE_CHECK, {V}, kOkNote, kOkNoteTypes);

	// Server config (c_fqdn, the qm_*_reject enforcement toggles, ...).
	RegisterPolicyFn(loader, "qm_config_get", UmbrellaKind::CONFIG_GET, {V}, {"name", "value"}, {V, V});
	RegisterPolicyFn(loader, "qm_config_set", UmbrellaKind::CONFIG_SET, {V, V}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_config", UmbrellaKind::CONFIG_LIST, {}, {"name", "value"}, {V, V});

	// Mailing lists. A list is a room, so every one of these names its room the
	// same way cit_room_acl does — by display name.
	RegisterPolicyFn(loader, "qm_lists", UmbrellaKind::LIST_LIST, {},
	                 {"room_num", "room", "address", "enabled", "mode", "post_policy", "subscribers",
	                  "pending", "last_sent"},
	                 {I, V, V, B, V, V, I, I, I});
	RegisterPolicyFn(loader, "qm_list_create", UmbrellaKind::LIST_CREATE, {V, V}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_list_set", UmbrellaKind::LIST_SET, {V, V, V}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_list_remove", UmbrellaKind::LIST_REMOVE, {V}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_list_subs", UmbrellaKind::LIST_SUBS, {V},
	                 {"address", "kind", "state", "created_at", "confirmed_at"}, {V, V, V, I, I});
	RegisterPolicyFn(loader, "qm_list_sub_add", UmbrellaKind::LIST_SUB_ADD, {V, V, V}, kOkNote,
	                 kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_list_sub_remove", UmbrellaKind::LIST_SUB_REMOVE, {V, V}, kOkNote,
	                 kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_list_held", UmbrellaKind::LIST_HELD, {V},
	                 {"id", "room_num", "mail_from", "subject", "received_at", "state"},
	                 {I, I, V, V, I, V});
	RegisterPolicyFn(loader, "qm_list_approve", UmbrellaKind::LIST_APPROVE, {I}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_list_reject", UmbrellaKind::LIST_REJECT, {I}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_list_render", UmbrellaKind::LIST_RENDER, {V, I}, {"message", "note"},
	                 {V, V});
}

} // namespace

void QuackmailExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailExtension::Name() {
	return "quackmail";
}
std::string QuackmailExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL
	return EXT_VERSION_QUACKMAIL;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail, loader) {
	duckdb::LoadInternal(loader);
}
}
