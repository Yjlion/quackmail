#define DUCKDB_EXTENSION_MAIN

#include "quackmail_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include <functional>

#include "quackmail/auth.hpp"
#include "quackmail/citadel_store.hpp"
#include "quackmail/davxml.hpp"
#include "quackmail/dkim.hpp"
#include "quackmail/dmarc.hpp"
#include "quackmail/citadel_msg.hpp"
#include "quackmail/feed.hpp"
#include "quackmail/fetch.hpp"
#include "quackmail/html_sanitize.hpp"
#include "quackmail/http.hpp"
#include "quackmail/ical.hpp"
#include "quackmail/itip.hpp"
#include "quackmail/json.hpp"
#include "quackmail/listserv.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mailpolicy.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/psl.hpp"
#include "quackmail/rbl.hpp"
#include "quackmail/sieve.hpp"
#include "quackmail/spf.hpp"
#include "quackmail/tz.hpp"
#include "quackmail/util.hpp"
#include "quackmail/vcard.hpp"
#include "quackmail/vnote.hpp"

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
	CIT_ROOM_KILL,
	CIT_FLOOR_ADD,
	CIT_ROOM_ACL,
	CIT_ROOM_ACL_SET,
	CIT_ROOM_RIGHTS,
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
	// Remote message pulls.
	FEED_LIST,
	FEED_ADD,
	FEED_SET,
	FEED_REMOVE,
	FEED_TEST,
	FEED_PARSE,
	FEED_RENDER,
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
		gstate->rows.push_back({Value("QuackCit 0.6.0")});
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
	case UmbrellaKind::CIT_ROOM_KILL: {
		quackmail::citadel::Room room;
		std::string err;
		if (!quackmail::citadel::ResolveRoom(con, "", bind.args[0], room)) {
			gstate->rows.push_back({Value::BOOLEAN(false), Value("no such public room")});
			break;
		}
		bool ok = quackmail::citadel::KillRoom(con, room.room_num, err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "room deleted" : err)});
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
	case UmbrellaKind::CIT_ROOM_RIGHTS: {
		// The *derived* view, which cit_room_acl cannot show: rights come from
		// the room's own attributes unioned with the stored grants, so the
		// stored table alone never answers "may this person do this".
		quackmail::citadel::Room room;
		if (!quackmail::citadel::ResolveRoom(con, bind.args[1], bind.args[0], room)) {
			break; // no such room visible to that user: an empty listing
		}
		std::string rights = quackmail::citadel::EffectiveRights(con, bind.args[1], room);
		gstate->rows.push_back(
		    {Value(room.display_name), Value(bind.args[1]), Value(rights),
		     Value::BOOLEAN(quackmail::citadel::CanPost(con, bind.args[1], room)),
		     Value::BOOLEAN(quackmail::citadel::CanAdminister(con, bind.args[1], room))});
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

	// ---- remote message pulls -------------------------------------------
	case UmbrellaKind::FEED_LIST: {
		for (auto &f : quackmail::fetch::ListFeeds(con)) {
			std::string target = f.target_user.empty() ? ("room " + std::to_string(f.target_room))
			                                           : ("user " + f.target_user);
			quackmail::citadel::Room room;
			if (f.target_user.empty() && quackmail::citadel::GetRoomByNum(con, f.target_room, room)) {
				target = room.display_name;
			}
			std::string source = f.kind == quackmail::fetch::Kind::Rss
			                         ? f.url
			                         : (f.username + "@" + f.host +
			                            (f.port > 0 ? (":" + std::to_string(f.port)) : ""));
			gstate->rows.push_back({Value(f.name), Value(quackmail::fetch::KindName(f.kind)),
			                        Value::BOOLEAN(f.enabled), Value(source), Value(target),
			                        Value::BIGINT(f.interval_secs), Value::BIGINT(f.last_run_at),
			                        Value(f.last_status), Value(f.last_error),
			                        Value::BIGINT(f.messages_pulled)});
		}
		break;
	}
	case UmbrellaKind::FEED_ADD: {
		// (name, kind, source, target). `source` is a URL for rss and
		// user:password@host[:port] for a mailbox; `target` is a room name, or
		// "user:<name>" to route through that user's Sieve script instead.
		quackmail::fetch::Feed f;
		std::string err;
		f.name = bind.args[0];
		f.kind = quackmail::fetch::ParseKind(bind.args[1]);
		std::string source = bind.args[2];
		if (f.kind == quackmail::fetch::Kind::Rss) {
			f.url = source;
		} else {
			auto at = source.rfind('@');
			std::string creds = at == std::string::npos ? "" : source.substr(0, at);
			std::string hostpart = at == std::string::npos ? source : source.substr(at + 1);
			auto colon = creds.find(':');
			f.username = colon == std::string::npos ? creds : creds.substr(0, colon);
			f.password = colon == std::string::npos ? "" : creds.substr(colon + 1);
			auto pcolon = hostpart.rfind(':');
			if (pcolon != std::string::npos) {
				f.host = hostpart.substr(0, pcolon);
				f.port = std::atoll(hostpart.c_str() + pcolon + 1);
			} else {
				f.host = hostpart;
			}
		}
		std::string target = bind.args[3];
		if (target.compare(0, 5, "user:") == 0) {
			f.target_user = target.substr(5);
		} else {
			quackmail::citadel::Room room;
			if (!quackmail::citadel::ResolveRoom(con, "", target, room)) {
				gstate->rows.push_back({Value::BOOLEAN(false), Value("no such public room")});
				break;
			}
			f.target_room = room.room_num;
		}
		bool ok = quackmail::fetch::SetFeed(con, f, err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "feed added" : err)});
		break;
	}
	case UmbrellaKind::FEED_SET: {
		std::string err;
		bool ok = quackmail::fetch::SetField(con, bind.args[0], bind.args[1], bind.args[2], err);
		// The password is never echoed back, here or on the web page.
		std::string shown = quackmail::util::Lower(bind.args[1]) == "password" ? "********" : bind.args[2];
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? (bind.args[1] + " = " + shown) : err)});
		break;
	}
	case UmbrellaKind::FEED_REMOVE: {
		std::string err;
		bool ok = quackmail::fetch::RemoveFeed(con, bind.args[0], err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? "feed removed" : err)});
		break;
	}
	case UmbrellaKind::FEED_TEST: {
		std::string info, err;
		bool ok = quackmail::fetch::TestFeed(con, bind.args[0], info, err);
		gstate->rows.push_back({Value::BOOLEAN(ok), Value(ok ? info : err)});
		break;
	}
	case UmbrellaKind::FEED_PARSE: {
		// A pure parse of a feed document. Offline, no network: this is what
		// makes every shape of RSS/Atom assertable from sqllogictest.
		quackmail::feed::Feed parsed;
		if (!quackmail::feed::Parse(bind.args[0], parsed)) {
			break; // not a feed: an empty listing
		}
		for (auto &it : parsed.items) {
			gstate->rows.push_back({Value(parsed.title), Value(it.guid), Value(it.title), Value(it.link),
			                        Value(it.author), Value::BIGINT(it.published),
			                        Value::BOOLEAN(it.html),
			                        Value(it.content.empty() ? it.summary : it.content)});
		}
		break;
	}
	case UmbrellaKind::FEED_RENDER: {
		// The message one feed item becomes, again with no network involved.
		quackmail::feed::Feed parsed;
		if (!quackmail::feed::Parse(bind.args[0], parsed) || parsed.items.empty()) {
			gstate->rows.push_back({Value(LogicalType::VARCHAR), Value("not a feed, or it has no items")});
			break;
		}
		int64_t idx = std::atoll(bind.args[1].c_str());
		if (idx < 0 || idx >= (int64_t)parsed.items.size()) {
			gstate->rows.push_back({Value(LogicalType::VARCHAR), Value("no item at that index")});
			break;
		}
		std::string fqdn = quackmail::citadel::GetConfig(con, "c_fqdn", "localhost");
		gstate->rows.push_back(
		    {Value(quackmail::feed::ToRfc822(parsed, parsed.items[(size_t)idx], "testfeed", fqdn, "", "")),
		     Value("ok")});
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

unique_ptr<FunctionData> CitRoomKillBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::CIT_ROOM_KILL;
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

unique_ptr<FunctionData> CitRoomRightsBind(ClientContext &, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	auto b = make_uniq<RowsBindData>();
	b->kind = UmbrellaKind::CIT_ROOM_RIGHTS;
	b->args = {input.inputs[0].ToString(), input.inputs[1].ToString()};
	names = {"room", "username", "rights", "can_post", "can_administer"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BOOLEAN, LogicalType::BOOLEAN};
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

// The DAV resource-name encoding. Exposed for the same reason the two above
// are: it is the function standing between an euid containing '/' or ".." and a
// path the router would either split in half or reject outright, and a
// round-trip bug in it silently makes an object unreachable.
void DavNameScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t in) {
		return StringVector::AddString(result, quackmail::dav::NameForEuid(in.GetString()));
	});
}

void DavEuidScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t in) {
		return StringVector::AddString(result, quackmail::dav::EuidForName(in.GetString()));
	});
}

// The JSON codec JMAP is built on. A request body is attacker-supplied, so what
// the parser *refuses* is as much of a contract as what it accepts — and all of
// it is assertable from sqllogictest with no socket in the loop.
//
// qm_json_valid(text) -> does it parse at all?
void JsonValidScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](string_t in) {
		quackmail::json::Value v;
		return quackmail::json::Parse(in.GetString(), v);
	});
}

// qm_json_error(text) -> why it did not parse, "" when it did.
void JsonErrorScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t in) {
		quackmail::json::Value v;
		std::string err;
		quackmail::json::Parse(in.GetString(), v, err);
		return StringVector::AddString(result, err);
	});
}

// qm_json_normalize(text) -> parse and re-serialize. A round trip through the
// whole codec in one expression, which is what makes escaping, number
// formatting and member order testable as a table of cases.
void JsonNormalizeScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t in) {
		quackmail::json::Value v;
		if (!quackmail::json::Parse(in.GetString(), v)) {
			return StringVector::AddString(result, "");
		}
		return StringVector::AddString(result, quackmail::json::Serialize(v));
	});
}

// qm_json_get(text, path) -> the value at a dotted path, serialized. A string
// comes back bare rather than quoted, because comparing it to a SQL literal is
// the whole point.
void JsonGetScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t in, string_t path) {
		    quackmail::json::Value root;
		    if (!quackmail::json::Parse(in.GetString(), root)) {
			    return StringVector::AddString(result, "");
		    }
		    const quackmail::json::Value *cur = &root;
		    std::string p = path.GetString();
		    size_t pos = 0;
		    while (pos <= p.size() && cur) {
			    size_t dot = p.find('.', pos);
			    std::string seg = p.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
			    if (!seg.empty()) {
				    if (!seg.empty() && seg.find_first_not_of("0123456789") == std::string::npos &&
				        cur->type == quackmail::json::Value::Array) {
					    size_t idx = (size_t)std::strtoull(seg.c_str(), nullptr, 10);
					    cur = idx < cur->Size() ? &cur->At(idx) : nullptr;
				    } else {
					    cur = cur->Get(seg);
				    }
			    }
			    if (dot == std::string::npos) {
				    break;
			    }
			    pos = dot + 1;
		    }
		    if (!cur) {
			    return StringVector::AddString(result, "");
		    }
		    if (cur->type == quackmail::json::Value::String) {
			    return StringVector::AddString(result, cur->str);
		    }
		    return StringVector::AddString(result, quackmail::json::Serialize(*cur));
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

// qm_http_keepalive(version, connection_header) -> may the connection be reused?
//
// Whether a socket is held open or dropped is a security-relevant decision — it
// is what stops an unconsumed request body from being read as the next request
// — and it is a pure function of two strings. Asserting the truth table here
// costs nothing; proving the same thing over real sockets costs a fixture per
// case.
void HttpKeepAliveScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, bool>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t version, string_t connection) {
		    quackmail::http::Request req;
		    req.version = version.GetString();
		    if (!connection.GetString().empty()) {
			    req.headers.emplace_back("Connection", connection.GetString());
		    }
		    return quackmail::http::ShouldKeepAlive(req);
	    });
}

// ---- vCard ---------------------------------------------------------------
// Contacts are stored as a text/vcard part of an ordinary message, so the
// parser is what stands between a phone's address book and this one. Exposed as
// scalars so the escaping, folding and round-trip rules are assertable from
// sqllogictest with no room and no session in the way.

// qm_vcard_count(text) -> how many cards parsed; 0 when the input is not vCard.
void VcardCountScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, int64_t>(args.data[0], result, args.size(), [&](string_t in) {
		std::vector<quackmail::vcard::Card> cards;
		quackmail::vcard::Parse(in.GetString(), cards);
		return (int64_t)cards.size();
	});
}

// qm_vcard_get(text, property) -> the property's value, NULL when absent.
// Structured properties come back with their components joined by ';'.
void VcardGetScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::ExecuteWithNulls<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t text, string_t prop, ValidityMask &mask, idx_t idx) {
		    quackmail::vcard::Card card;
		    if (!quackmail::vcard::ParseOne(text.GetString(), card)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    const quackmail::vcard::Property *p = card.Find(prop.GetString());
		    if (!p) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, p->Value());
	    });
}

// qm_vcard_fn(text) -> the display name, however it has to be derived.
void VcardFnScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t in, ValidityMask &mask, idx_t idx) {
		    quackmail::vcard::Card card;
		    if (!quackmail::vcard::ParseOne(in.GetString(), card)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, card.Fn());
	    });
}

// qm_vcard_euid(text) -> the euid the card would be stored under.
void VcardEuidScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t in, ValidityMask &mask, idx_t idx) {
		    quackmail::vcard::Card card;
		    if (!quackmail::vcard::ParseOne(in.GetString(), card)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, quackmail::vcard::EuidFor(card));
	    });
}

// qm_vcard_emit(text, version) -> parse and re-serialize. This is the function
// that makes the round-trip testable: emit(parse(x)) has to preserve every
// property, including ones this code does not model.
void VcardEmitScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::ExecuteWithNulls<string_t, int64_t, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t text, int64_t version, ValidityMask &mask, idx_t idx) {
		    quackmail::vcard::Card card;
		    if (!quackmail::vcard::ParseOne(text.GetString(), card)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(
		        result, quackmail::vcard::Emit(card, version ? (int)version : card.version));
	    });
}

// ---- Sieve rules ---------------------------------------------------------
// The rule view over a script. The script text stays the single source of
// truth — see the note in core/include/quackmail/sieve.hpp — so these are the
// two halves of deriving rules from it and writing them back.

// qm_sieve_rules(script) -> a compact description of the rules, or NULL when the
// script says something the rule view cannot show.
void SieveRulesScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t in, ValidityMask &mask, idx_t idx) {
		    std::vector<quackmail::sieve::Rule> rules;
		    std::string why;
		    if (!quackmail::sieve::Decompose(in.GetString(), rules, why)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    // One line per rule: name|all/any|test,test|action,action[|stop]
		    std::string out;
		    for (auto &r : rules) {
			    if (!out.empty()) {
				    out += "\n";
			    }
			    out += r.name + "|" + (r.all ? "all" : "any") + "|";
			    for (size_t i = 0; i < r.tests.size(); i++) {
				    if (i) {
					    out += ",";
				    }
				    if (r.tests[i].negate) {
					    out += "!";
				    }
				    out += r.tests[i].field + " " + r.tests[i].op + " " + r.tests[i].value;
			    }
			    out += "|";
			    for (size_t i = 0; i < r.actions.size(); i++) {
				    if (i) {
					    out += ",";
				    }
				    switch (r.actions[i].type) {
				    case quackmail::sieve::Action::FILEINTO:
					    out += "fileinto " + r.actions[i].folder;
					    break;
				    case quackmail::sieve::Action::REDIRECT:
					    out += "redirect " + r.actions[i].address;
					    break;
				    case quackmail::sieve::Action::REJECT:
					    out += "reject " + r.actions[i].reason;
					    break;
				    case quackmail::sieve::Action::DISCARD:
					    out += "discard";
					    break;
				    default:
					    out += "keep";
					    break;
				    }
			    }
			    if (r.stop) {
				    out += "|stop";
			    }
		    }
		    return StringVector::AddString(result, out);
	    });
}

// qm_sieve_rules_why(script) -> why the rule view cannot show it, or NULL when it
// can. The UI shows this sentence instead of a builder.
void SieveRulesWhyScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t in, ValidityMask &mask, idx_t idx) {
		    std::vector<quackmail::sieve::Rule> rules;
		    std::string why;
		    if (quackmail::sieve::Decompose(in.GetString(), rules, why)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, why);
	    });
}

// qm_sieve_recompose(script) -> Decompose then Compose. The round trip has to be
// semantically stable: whatever this produces must decompose to the same rules.
void SieveRecomposeScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t in, ValidityMask &mask, idx_t idx) {
		    std::vector<quackmail::sieve::Rule> rules;
		    std::string why;
		    if (!quackmail::sieve::Decompose(in.GetString(), rules, why)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, quackmail::sieve::Compose(rules));
	    });
}

// ---- HTML sanitizing -----------------------------------------------------
// Two profiles for two jobs: a deny-list for rendering someone else's mail
// behind a sandboxed frame, and an allow-list for HTML a local user composed,
// which will be stored and re-served from our origin to other people. See
// core/include/quackmail/html_sanitize.hpp for why the shapes differ.

void HtmlSanitizeComposeScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t in) {
		return StringVector::AddString(result, quackmail::html::SanitizeForCompose(in.GetString()));
	});
}

void HtmlSanitizeDisplayScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t in) {
		return StringVector::AddString(result, quackmail::html::SanitizeForDisplay(in.GetString()));
	});
}

void HtmlToTextScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t in) {
		return StringVector::AddString(result, quackmail::html::ToPlainText(in.GetString()));
	});
}

void HtmlRewriteCidScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t in, string_t prefix) {
		    return StringVector::AddString(
		        result, quackmail::html::RewriteCidUrls(in.GetString(), prefix.GetString()));
	    });
}

// ---- MIME building -------------------------------------------------------
// The nesting a set of parts turns into is a decision with several wrong
// answers, so `qm_mime_shape` reports it directly as a readable tree rather than
// leaving a test to hunt for boundaries in the bytes.

namespace {

// Build a message from the four kinds of part, then describe what came out.
std::string BuildShapeOrBytes(const std::string &plain, const std::string &html, int64_t inlines,
                              int64_t attachments, bool want_shape) {
	std::vector<quackmail::mime::BuildPart> parts;
	if (!plain.empty()) {
		quackmail::mime::BuildPart p;
		p.content_type = "text/plain";
		p.content = plain;
		parts.push_back(p);
	}
	if (!html.empty()) {
		quackmail::mime::BuildPart p;
		p.content_type = "text/html";
		p.content = html;
		parts.push_back(p);
	}
	for (int64_t i = 0; i < inlines && i < 8; i++) {
		quackmail::mime::BuildPart p;
		p.content_type = "image/png";
		p.content = "PNG fake";
		p.content_id = "img" + std::to_string(i) + "@test";
		parts.push_back(p);
	}
	for (int64_t i = 0; i < attachments && i < 8; i++) {
		quackmail::mime::BuildPart p;
		p.content_type = "application/pdf";
		p.content = "%PDF fake";
		p.filename = "file" + std::to_string(i) + ".pdf";
		parts.push_back(p);
	}

	quackmail::mime::HeaderList headers = {{"From", "a@test"}, {"Subject", "shape"}};
	std::string msg = quackmail::mime::BuildMessage(headers, parts);
	if (!want_shape) {
		return msg;
	}

	// Describe the tree the parser finds, which is the real question: did the
	// parts end up somewhere a mail client will look for them?
	std::function<std::string(const quackmail::mime::MimeEntity &)> describe =
	    [&](const quackmail::mime::MimeEntity &e) -> std::string {
		std::string mime = e.content_type.Mime();
		if (!e.IsMultipart()) {
			return mime;
		}
		std::string out = e.content_type.subtype + "(";
		for (size_t i = 0; i < e.children.size(); i++) {
			if (i) {
				out += ",";
			}
			out += describe(e.children[i]);
		}
		return out + ")";
	};
	return describe(quackmail::mime::ParseEntity(msg));
}

} // namespace

// ---- password storage ----------------------------------------------------
//
// A password KDF is the one piece of this server whose failure is silent and
// permanent: nothing misbehaves, nobody notices, and the damage only shows up
// after a database leak. So the two things that matter are asserted in SQL —
// that the legacy scheme still verifies (an upgrade that locked every existing
// user out would be worse than the weakness it fixed), and that a row hashed
// the old way is reported as needing a rewrite.

// qm_password_check(password, algo, salt, hash) -> does it match?
void PasswordCheckScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnifiedVectorFormat pf, af, sf, hf;
	args.data[0].ToUnifiedFormat(args.size(), pf);
	args.data[1].ToUnifiedFormat(args.size(), af);
	args.data[2].ToUnifiedFormat(args.size(), sf);
	args.data[3].ToUnifiedFormat(args.size(), hf);
	auto pd = UnifiedVectorFormat::GetData<string_t>(pf);
	auto ad = UnifiedVectorFormat::GetData<string_t>(af);
	auto sd = UnifiedVectorFormat::GetData<string_t>(sf);
	auto hd = UnifiedVectorFormat::GetData<string_t>(hf);
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto out = FlatVector::GetData<bool>(result);
	for (idx_t i = 0; i < args.size(); i++) {
		quackmail::auth::Stored stored;
		stored.algo = ad[af.sel->get_index(i)].GetString();
		stored.salt = sd[sf.sel->get_index(i)].GetString();
		stored.hash = hd[hf.sel->get_index(i)].GetString();
		out[i] = quackmail::auth::CheckPassword(pd[pf.sel->get_index(i)].GetString(), stored);
	}
}

// qm_password_needs_rehash(algo) -> is this row stored below the current bar?
void PasswordNeedsRehashScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](string_t algo) {
		return quackmail::auth::NeedsRehash(algo.GetString());
	});
}

// qm_password_hash(password) -> "<algo>:<salt>:<hash>", a freshly salted hash at
// the current work factor. Non-deterministic by construction, so a test asserts
// that it round-trips rather than that it equals anything.
void PasswordHashScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t pw) {
		auto s = quackmail::auth::HashPassword(pw.GetString());
		return StringVector::AddString(result, s.algo + ":" + s.salt + ":" + s.hash);
	});
}

// qm_mime_shape(plain, html, inline_count, attachment_count) -> "mixed(...)"
void MimeShapeScalar(DataChunk &args, ExpressionState &, Vector &result) {
	auto &plain = args.data[0];
	auto &html = args.data[1];
	auto &inl = args.data[2];
	auto &att = args.data[3];
	UnifiedVectorFormat pf, hf, inf, af;
	plain.ToUnifiedFormat(args.size(), pf);
	html.ToUnifiedFormat(args.size(), hf);
	inl.ToUnifiedFormat(args.size(), inf);
	att.ToUnifiedFormat(args.size(), af);
	auto pd = UnifiedVectorFormat::GetData<string_t>(pf);
	auto hd = UnifiedVectorFormat::GetData<string_t>(hf);
	auto ind = UnifiedVectorFormat::GetData<int64_t>(inf);
	auto ad = UnifiedVectorFormat::GetData<int64_t>(af);
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto out = FlatVector::GetData<string_t>(result);
	for (idx_t i = 0; i < args.size(); i++) {
		std::string shape = BuildShapeOrBytes(pd[pf.sel->get_index(i)].GetString(),
		                                      hd[hf.sel->get_index(i)].GetString(),
		                                      ind[inf.sel->get_index(i)], ad[af.sel->get_index(i)], true);
		out[i] = StringVector::AddString(result, shape);
	}
}

// qm_mime_build(plain, html, inline_count, attachment_count) -> the raw message.
void MimeBuildScalar(DataChunk &args, ExpressionState &, Vector &result) {
	auto &plain = args.data[0];
	auto &html = args.data[1];
	auto &inl = args.data[2];
	auto &att = args.data[3];
	UnifiedVectorFormat pf, hf, inf, af;
	plain.ToUnifiedFormat(args.size(), pf);
	html.ToUnifiedFormat(args.size(), hf);
	inl.ToUnifiedFormat(args.size(), inf);
	att.ToUnifiedFormat(args.size(), af);
	auto pd = UnifiedVectorFormat::GetData<string_t>(pf);
	auto hd = UnifiedVectorFormat::GetData<string_t>(hf);
	auto ind = UnifiedVectorFormat::GetData<int64_t>(inf);
	auto ad = UnifiedVectorFormat::GetData<int64_t>(af);
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto out = FlatVector::GetData<string_t>(result);
	for (idx_t i = 0; i < args.size(); i++) {
		std::string msg = BuildShapeOrBytes(pd[pf.sel->get_index(i)].GetString(),
		                                    hd[hf.sel->get_index(i)].GetString(),
		                                    ind[inf.sel->get_index(i)], ad[af.sel->get_index(i)], false);
		out[i] = StringVector::AddString(result, msg);
	}
}

// qm_mime_encoding(content_type, content) -> "8bit" | "quoted-printable" | "base64"
void MimeEncodingScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t type, string_t content) {
		    quackmail::mime::BuildPart p;
		    p.content_type = type.GetString();
		    p.content = content.GetString();
		    return StringVector::AddString(result, quackmail::mime::ChooseEncoding(p));
	    });
}

// ---- vNote ---------------------------------------------------------------
// The format Citadel stores a sticky note in. Same grammar as vCard, different
// property names — see core/vnote.cpp for why it is not VJOURNAL.

void VnoteCountScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, int64_t>(args.data[0], result, args.size(), [&](string_t in) {
		std::vector<quackmail::vnote::Note> notes;
		quackmail::vnote::Parse(in.GetString(), notes);
		return (int64_t)notes.size();
	});
}

// qm_vnote_get(text, field) -> "summary" | "body" | "uid" | "color", or any
// property name for the ones this does not model.
void VnoteGetScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::ExecuteWithNulls<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t text, string_t field, ValidityMask &mask, idx_t idx) {
		    quackmail::vnote::Note note;
		    if (!quackmail::vnote::ParseOne(text.GetString(), note)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    std::string want = quackmail::util::Upper(field.GetString());
		    if (want == "SUMMARY") {
			    return StringVector::AddString(result, note.summary);
		    }
		    if (want == "BODY") {
			    return StringVector::AddString(result, note.body);
		    }
		    if (want == "UID") {
			    return StringVector::AddString(result, note.uid);
		    }
		    if (want == "COLOR" || want == "X-OUTLOOK-COLOR") {
			    return StringVector::AddString(result, note.color);
		    }
		    for (auto &kv : note.props) {
			    if (kv.first == want) {
				    return StringVector::AddString(result, kv.second);
			    }
		    }
		    mask.SetInvalid(idx);
		    return string_t();
	    });
}

void VnoteTitleScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t in, ValidityMask &mask, idx_t idx) {
		    quackmail::vnote::Note note;
		    if (!quackmail::vnote::ParseOne(in.GetString(), note)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, quackmail::vnote::TitleOf(note));
	    });
}

void VnoteEmitScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t in, ValidityMask &mask, idx_t idx) {
		    quackmail::vnote::Note note;
		    if (!quackmail::vnote::ParseOne(in.GetString(), note)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, quackmail::vnote::Emit(note));
	    });
}

// ---- iCalendar -----------------------------------------------------------
// Events, tasks and journal entries, stored as a text/calendar part of an
// ordinary message. Exposed as scalars so recurrence expansion — the part with
// the most ways to be subtly wrong — is assertable without a calendar room.

void IcalCountScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, int64_t>(args.data[0], result, args.size(), [&](string_t in) {
		std::vector<quackmail::ical::Item> items;
		quackmail::ical::ParseItems(in.GetString(), items);
		return (int64_t)items.size();
	});
}

// qm_ical_get(text, component, property) -> a property of the first component
// of that type. "VEVENT", "SUMMARY".
void IcalGetScalar(DataChunk &args, ExpressionState &, Vector &result) {
	TernaryExecutor::ExecuteWithNulls<string_t, string_t, string_t, string_t>(
	    args.data[0], args.data[1], args.data[2], result, args.size(),
	    [&](string_t text, string_t comp, string_t prop, ValidityMask &mask, idx_t idx) {
		    quackmail::ical::Component root;
		    if (!quackmail::ical::Parse(text.GetString(), root)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    const quackmail::ical::Component *c = root.Child(comp.GetString());
		    if (!c) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    const quackmail::ical::Property *p = c->Find(prop.GetString());
		    if (!p) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, p->value);
	    });
}

// qm_ical_start(text) -> the first item's start instant, NULL when it has none.
void IcalStartScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, int64_t>(
	    args.data[0], result, args.size(), [&](string_t in, ValidityMask &mask, idx_t idx) -> int64_t {
		    std::vector<quackmail::ical::Item> items;
		    if (!quackmail::ical::ParseItems(in.GetString(), items) || items.empty() ||
		        !items[0].start.valid) {
			    mask.SetInvalid(idx);
			    return 0;
		    }
		    return items[0].start.epoch;
	    });
}

void IcalEuidScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t in, ValidityMask &mask, idx_t idx) {
		    std::vector<quackmail::ical::Item> items;
		    if (!quackmail::ical::ParseItems(in.GetString(), items) || items.empty()) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, quackmail::ical::EuidFor(items[0]));
	    });
}

// qm_ical_expand_count(text, from, to) -> how many occurrences fall in the
// window. The cheap way to assert a recurrence rule.
void IcalExpandCountScalar(DataChunk &args, ExpressionState &, Vector &result) {
	TernaryExecutor::ExecuteWithNulls<string_t, int64_t, int64_t, int64_t>(
	    args.data[0], args.data[1], args.data[2], result, args.size(),
	    [&](string_t text, int64_t from, int64_t to, ValidityMask &mask, idx_t idx) -> int64_t {
		    std::vector<quackmail::ical::Item> items;
		    if (!quackmail::ical::ParseItems(text.GetString(), items) || items.empty()) {
			    mask.SetInvalid(idx);
			    return 0;
		    }
		    return (int64_t)quackmail::ical::Expand(items[0], from, to).size();
	    });
}

// qm_ical_expand_starts(text, from, to) -> the occurrence instants, comma
// separated. Counting is not enough for the assertion that matters: a weekly
// meeting has to keep its *local* hour across a DST change, and only the
// instants show that.
void IcalExpandStartsScalar(DataChunk &args, ExpressionState &, Vector &result) {
	TernaryExecutor::ExecuteWithNulls<string_t, int64_t, int64_t, string_t>(
	    args.data[0], args.data[1], args.data[2], result, args.size(),
	    [&](string_t text, int64_t from, int64_t to, ValidityMask &mask, idx_t idx) {
		    std::vector<quackmail::ical::Item> items;
		    if (!quackmail::ical::ParseItems(text.GetString(), items) || items.empty()) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    std::string out;
		    for (auto &o : quackmail::ical::Expand(items[0], from, to)) {
			    if (!out.empty()) {
				    out += ",";
			    }
			    out += std::to_string(o.start);
		    }
		    return StringVector::AddString(result, out);
	    });
}

// qm_ical_freebusy(text, from, to) -> a VFREEBUSY over the window.
//
// Free/busy is where the *absences* are the contract — TRANSP:TRANSPARENT and
// STATUS:CANCELLED must contribute nothing, adjacent meetings must merge into
// one interval, and a recurring meeting must be busy every week. All of that is
// a pure function of the text, so it is assertable from SQL with no socket,
// which is the only way to pin it down cheaply enough to keep pinning it.
void IcalFreebusyScalar(DataChunk &args, ExpressionState &, Vector &result) {
	TernaryExecutor::ExecuteWithNulls<string_t, int64_t, int64_t, string_t>(
	    args.data[0], args.data[1], args.data[2], result, args.size(),
	    [&](string_t text, int64_t from, int64_t to, ValidityMask &mask, idx_t idx) {
		    quackmail::ical::Busy busy;
		    quackmail::ical::CollectBusy(text.GetString(), from, to, busy);
		    return StringVector::AddString(
		        result, quackmail::ical::EmitFreeBusy(from, to, busy, "", "", ""));
	    });
}

// ---- iTIP ----------------------------------------------------------------
//
// Scheduling is where the rules are easy to state and easy to get subtly
// wrong — a CANCEL that does not bump SEQUENCE is dropped by every client, a
// REPLY that carries the other attendees reads as answering on their behalf,
// and a stale REPLY that overwrites a newer answer loses information nobody
// can recover. All of it is a pure function of the calendar text, so all of it
// is pinned here rather than behind a mail server.

// qm_itip_request(ics) / qm_itip_cancel(ics)
void ItipRequestScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t text, ValidityMask &mask, idx_t idx) {
		    std::string out = quackmail::itip::BuildRequest(text.GetString());
		    if (out.empty()) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, out);
	    });
}

void ItipCancelScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t text, ValidityMask &mask, idx_t idx) {
		    std::string out = quackmail::itip::BuildCancel(text.GetString());
		    if (out.empty()) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, out);
	    });
}

// qm_itip_reply(ics, attendee, partstat) -> the REPLY that attendee sends.
void ItipReplyScalar(DataChunk &args, ExpressionState &, Vector &result) {
	TernaryExecutor::ExecuteWithNulls<string_t, string_t, string_t, string_t>(
	    args.data[0], args.data[1], args.data[2], result, args.size(),
	    [&](string_t text, string_t who, string_t partstat, ValidityMask &mask, idx_t idx) {
		    std::string out =
		        quackmail::itip::BuildReply(text.GetString(), who.GetString(), partstat.GetString());
		    if (out.empty()) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, out);
	    });
}

// qm_itip_apply_reply(stored, reply) -> the organizer's copy with the PARTSTAT
// folded in, or NULL when the reply changes nothing. NULL is the interesting
// answer here: a stale or unrelated reply must not silently rewrite an event.
void ItipApplyReplyScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::ExecuteWithNulls<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t stored, string_t reply, ValidityMask &mask, idx_t idx) {
		    std::string out;
		    if (!quackmail::itip::ApplyReply(stored.GetString(), reply.GetString(), out)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, out);
	    });
}

// qm_itip_recipients(ics, organizer) -> who gets told, comma separated.
void ItipRecipientsScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::ExecuteWithNulls<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t text, string_t org, ValidityMask &, idx_t) {
		    std::string out;
		    for (const auto &r : quackmail::itip::Recipients(text.GetString(), org.GetString())) {
			    out += out.empty() ? "" : ",";
			    out += r;
		    }
		    return StringVector::AddString(result, out);
	    });
}

// qm_itip_method(raw_message) -> the METHOD of its text/calendar part, or NULL
// when the message carries no scheduling content at all.
void ItipMethodScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t raw, ValidityMask &mask, idx_t idx) {
		    std::string method;
		    std::string part = quackmail::itip::CalendarPart(raw.GetString(), method);
		    if (part.empty()) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, method);
	    });
}

// qm_ical_set_summary(text, summary) -> the calendar with the first item's
// SUMMARY changed, everything else untouched.
//
// A small operation that happens to exercise the whole reason there are two
// representations: alarms, X- properties and attendee parameters have to come
// back out, because losing the reminder someone set on their phone is the worst
// thing a calendar store can do.
void IcalSetSummaryScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::ExecuteWithNulls<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t text, string_t summary, ValidityMask &mask, idx_t idx) {
		    quackmail::ical::Component root;
		    std::vector<quackmail::ical::Item> items;
		    if (!quackmail::ical::Parse(text.GetString(), root) ||
		        !quackmail::ical::ParseItems(text.GetString(), items) || items.empty()) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    quackmail::ical::Item it = items[0];
		    it.summary = summary.GetString();
		    it.sequence++;
		    if (!quackmail::ical::ApplyItem(root, it)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, quackmail::ical::Emit(root));
	    });
}

// qm_ical_vtimezone(tzid, from_year, to_year) -> a VTIMEZONE from the bundled
// database.
void IcalVtimezoneScalar(DataChunk &args, ExpressionState &, Vector &result) {
	TernaryExecutor::ExecuteWithNulls<string_t, int64_t, int64_t, string_t>(
	    args.data[0], args.data[1], args.data[2], result, args.size(),
	    [&](string_t tzid, int64_t y0, int64_t y1, ValidityMask &mask, idx_t idx) {
		    std::string out = quackmail::ical::EmitVtimezone(tzid.GetString(), (int)y0, (int)y1);
		    if (out.empty()) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, out);
	    });
}

// ---- time zones ----------------------------------------------------------
// The bundled IANA database, exposed so the offset tables can be asserted from
// sqllogictest. Every date a calendar renders passes through these, and a
// half-hour zone or a southern-hemisphere DST rule getting it wrong is the kind
// of bug that only shows up in someone else's timezone.

void TzVersionScalar(DataChunk &args, ExpressionState &, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] =
	    StringVector::AddString(result, quackmail::tz::Version());
	(void)args;
}

// qm_tz_canonical(tzid) -> the modern spelling, or NULL for an unknown zone.
void TzCanonicalScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t in, ValidityMask &mask, idx_t idx) {
		    std::string out = quackmail::tz::Canonical(in.GetString());
		    if (out.empty()) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, out);
	    });
}

// qm_tz_offset(tzid, epoch) -> seconds east of UTC, NULL for an unknown zone.
void TzOffsetScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::ExecuteWithNulls<string_t, int64_t, int64_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t zone, int64_t utc, ValidityMask &mask, idx_t idx) -> int64_t {
		    int off = 0;
		    bool dst = false;
		    std::string abbrev;
		    if (!quackmail::tz::OffsetAt(zone.GetString(), utc, off, dst, abbrev)) {
			    mask.SetInvalid(idx);
			    return 0;
		    }
		    return off;
	    });
}

// qm_tz_abbrev(tzid, epoch) -> "EST", "AEDT", "+0530".
void TzAbbrevScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::ExecuteWithNulls<string_t, int64_t, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t zone, int64_t utc, ValidityMask &mask, idx_t idx) {
		    int off = 0;
		    bool dst = false;
		    std::string abbrev;
		    if (!quackmail::tz::OffsetAt(zone.GetString(), utc, off, dst, abbrev)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    return StringVector::AddString(result, abbrev);
	    });
}

// qm_tz_to_utc(tzid, wall) -> the instant, NULL for an unknown zone. An
// ambiguous wall time resolves to the earlier of its two instants and a
// nonexistent one shifts past the gap; qm_tz_to_utc_kind reports which
// happened, because a caller that silently gets one of two answers has no way
// to tell it was asked an unanswerable question.
void TzToUtcScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::ExecuteWithNulls<string_t, int64_t, int64_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t zone, int64_t wall, ValidityMask &mask, idx_t idx) -> int64_t {
		    int64_t utc = 0;
		    bool ambiguous = false, nonexistent = false;
		    if (!quackmail::tz::ToUtc(zone.GetString(), wall, utc, ambiguous, nonexistent)) {
			    mask.SetInvalid(idx);
			    return 0;
		    }
		    return utc;
	    });
}

// qm_tz_to_utc_kind(tzid, wall) -> 'normal' | 'ambiguous' | 'nonexistent'.
void TzToUtcKindScalar(DataChunk &args, ExpressionState &, Vector &result) {
	BinaryExecutor::ExecuteWithNulls<string_t, int64_t, string_t>(
	    args.data[0], args.data[1], result, args.size(),
	    [&](string_t zone, int64_t wall, ValidityMask &mask, idx_t idx) {
		    int64_t utc = 0;
		    bool ambiguous = false, nonexistent = false;
		    if (!quackmail::tz::ToUtc(zone.GetString(), wall, utc, ambiguous, nonexistent)) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }
		    const char *kind = ambiguous ? "ambiguous" : (nonexistent ? "nonexistent" : "normal");
		    return StringVector::AddString(result, kind);
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

	// Citadel admin: create a public room / a floor, and delete a room. Deleting
	// takes everything keyed by the room with it — message pointers, read state,
	// access list, and any mailing list or feed configured against it.
	loader.RegisterFunction(
	    TableFunction("cit_room_add", {LogicalType::VARCHAR}, RowsFunc, CitRoomAddBind, RowsInit));
	loader.RegisterFunction(
	    TableFunction("cit_room_kill", {LogicalType::VARCHAR}, RowsFunc, CitRoomKillBind, RowsInit));
	loader.RegisterFunction(
	    TableFunction("cit_floor_add", {LogicalType::VARCHAR}, RowsFunc, CitFloorAddBind, RowsInit));

	// Room access control (RFC 4314). Granting "anyone" the `p` right is what
	// makes a room reachable at room_<name>@<fqdn>.
	loader.RegisterFunction(
	    TableFunction("cit_room_acl", {LogicalType::VARCHAR}, RowsFunc, CitRoomAclBind, RowsInit));
	loader.RegisterFunction(TableFunction("cit_room_acl_set",
	                                      {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                      RowsFunc, CitRoomAclSetBind, RowsInit));
	// What one person may actually do in one room, derived rather than stored —
	// the predicate the front-ends ask, exposed so it can be asserted in SQL.
	loader.RegisterFunction(TableFunction("cit_room_rights", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                      RowsFunc, CitRoomRightsBind, RowsInit));

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
	loader.RegisterFunction(ScalarFunction("qm_http_keepalive", {V, V}, B, HttpKeepAliveScalar));
	loader.RegisterFunction(ScalarFunction("qm_dav_name", {V}, V, DavNameScalar));
	loader.RegisterFunction(ScalarFunction("qm_dav_euid", {V}, V, DavEuidScalar));

	// JSON (pure), the codec JMAP is built on.
	loader.RegisterFunction(ScalarFunction("qm_json_valid", {V}, B, JsonValidScalar));
	loader.RegisterFunction(ScalarFunction("qm_json_error", {V}, V, JsonErrorScalar));
	loader.RegisterFunction(ScalarFunction("qm_json_normalize", {V}, V, JsonNormalizeScalar));
	loader.RegisterFunction(ScalarFunction("qm_json_get", {V, V}, V, JsonGetScalar));

	loader.RegisterFunction(ScalarFunction("qm_vcard_count", {V}, I, VcardCountScalar));
	loader.RegisterFunction(ScalarFunction("qm_vcard_get", {V, V}, V, VcardGetScalar));
	loader.RegisterFunction(ScalarFunction("qm_vcard_fn", {V}, V, VcardFnScalar));
	loader.RegisterFunction(ScalarFunction("qm_vcard_euid", {V}, V, VcardEuidScalar));
	loader.RegisterFunction(ScalarFunction("qm_vcard_emit", {V, I}, V, VcardEmitScalar));

	loader.RegisterFunction(ScalarFunction("qm_sieve_rules", {V}, V, SieveRulesScalar));
	loader.RegisterFunction(ScalarFunction("qm_sieve_rules_why", {V}, V, SieveRulesWhyScalar));
	loader.RegisterFunction(ScalarFunction("qm_sieve_recompose", {V}, V, SieveRecomposeScalar));

	loader.RegisterFunction(ScalarFunction("qm_html_compose", {V}, V, HtmlSanitizeComposeScalar));
	loader.RegisterFunction(ScalarFunction("qm_html_display", {V}, V, HtmlSanitizeDisplayScalar));
	loader.RegisterFunction(ScalarFunction("qm_html_to_text", {V}, V, HtmlToTextScalar));
	loader.RegisterFunction(ScalarFunction("qm_html_rewrite_cid", {V, V}, V, HtmlRewriteCidScalar));

	loader.RegisterFunction(
	    ScalarFunction("qm_password_check", {V, V, V, V}, B, PasswordCheckScalar));
	loader.RegisterFunction(
	    ScalarFunction("qm_password_needs_rehash", {V}, B, PasswordNeedsRehashScalar));
	loader.RegisterFunction(ScalarFunction("qm_password_hash", {V}, V, PasswordHashScalar));

	loader.RegisterFunction(ScalarFunction("qm_mime_shape", {V, V, I, I}, V, MimeShapeScalar));
	loader.RegisterFunction(ScalarFunction("qm_mime_build", {V, V, I, I}, V, MimeBuildScalar));
	loader.RegisterFunction(ScalarFunction("qm_mime_encoding", {V, V}, V, MimeEncodingScalar));

	loader.RegisterFunction(ScalarFunction("qm_vnote_count", {V}, I, VnoteCountScalar));
	loader.RegisterFunction(ScalarFunction("qm_vnote_get", {V, V}, V, VnoteGetScalar));
	loader.RegisterFunction(ScalarFunction("qm_vnote_title", {V}, V, VnoteTitleScalar));
	loader.RegisterFunction(ScalarFunction("qm_vnote_emit", {V}, V, VnoteEmitScalar));

	loader.RegisterFunction(ScalarFunction("qm_ical_count", {V}, I, IcalCountScalar));
	loader.RegisterFunction(ScalarFunction("qm_ical_get", {V, V, V}, V, IcalGetScalar));
	loader.RegisterFunction(ScalarFunction("qm_ical_start", {V}, I, IcalStartScalar));
	loader.RegisterFunction(ScalarFunction("qm_ical_euid", {V}, V, IcalEuidScalar));
	loader.RegisterFunction(ScalarFunction("qm_ical_expand_count", {V, I, I}, I, IcalExpandCountScalar));
	loader.RegisterFunction(ScalarFunction("qm_ical_expand_starts", {V, I, I}, V, IcalExpandStartsScalar));
	loader.RegisterFunction(ScalarFunction("qm_ical_freebusy", {V, I, I}, V, IcalFreebusyScalar));
	loader.RegisterFunction(ScalarFunction("qm_itip_request", {V}, V, ItipRequestScalar));
	loader.RegisterFunction(ScalarFunction("qm_itip_cancel", {V}, V, ItipCancelScalar));
	loader.RegisterFunction(ScalarFunction("qm_itip_reply", {V, V, V}, V, ItipReplyScalar));
	loader.RegisterFunction(ScalarFunction("qm_itip_apply_reply", {V, V}, V, ItipApplyReplyScalar));
	loader.RegisterFunction(ScalarFunction("qm_itip_recipients", {V, V}, V, ItipRecipientsScalar));
	loader.RegisterFunction(ScalarFunction("qm_itip_method", {V}, V, ItipMethodScalar));
	loader.RegisterFunction(ScalarFunction("qm_ical_set_summary", {V, V}, V, IcalSetSummaryScalar));
	loader.RegisterFunction(ScalarFunction("qm_ical_vtimezone", {V, I, I}, V, IcalVtimezoneScalar));

	// Time zones. BIGINT rather than VARCHAR for the epoch arguments: DuckDB
	// will not implicitly convert an INTEGER literal to VARCHAR, so a VARCHAR
	// signature would force every caller to quote its numbers.
	loader.RegisterFunction(ScalarFunction("qm_tz_version", {}, V, TzVersionScalar));
	loader.RegisterFunction(ScalarFunction("qm_tz_canonical", {V}, V, TzCanonicalScalar));
	loader.RegisterFunction(ScalarFunction("qm_tz_offset", {V, I}, I, TzOffsetScalar));
	loader.RegisterFunction(ScalarFunction("qm_tz_abbrev", {V, I}, V, TzAbbrevScalar));
	loader.RegisterFunction(ScalarFunction("qm_tz_to_utc", {V, I}, I, TzToUtcScalar));
	loader.RegisterFunction(ScalarFunction("qm_tz_to_utc_kind", {V, I}, V, TzToUtcKindScalar));

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

	// Remote message pulls: POP3/IMAP mailboxes and RSS/Atom feeds, posted into
	// a room. Polling itself is the qm_fetch worker in quackmail_spool.
	RegisterPolicyFn(loader, "qm_feeds", UmbrellaKind::FEED_LIST, {},
	                 {"name", "kind", "enabled", "source", "target", "interval_secs", "last_run_at",
	                  "last_status", "last_error", "messages_pulled"},
	                 {V, V, B, V, V, I, I, V, V, I});
	RegisterPolicyFn(loader, "qm_feed_add", UmbrellaKind::FEED_ADD, {V, V, V, V}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_feed_set", UmbrellaKind::FEED_SET, {V, V, V}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_feed_remove", UmbrellaKind::FEED_REMOVE, {V}, kOkNote, kOkNoteTypes);
	RegisterPolicyFn(loader, "qm_feed_test", UmbrellaKind::FEED_TEST, {V}, kOkNote, kOkNoteTypes);

	// Pure, offline: parse a feed document and render one of its items. These
	// are what let every shape of RSS/Atom be asserted with no network in the
	// loop, the same way qm_dkim_verify works against locally stored keys.
	RegisterPolicyFn(loader, "qm_feed_parse", UmbrellaKind::FEED_PARSE, {V},
	                 {"feed_title", "guid", "title", "link", "author", "published", "html", "body"},
	                 {V, V, V, V, V, I, B, V});
	RegisterPolicyFn(loader, "qm_feed_render", UmbrellaKind::FEED_RENDER, {V, I}, {"message", "note"},
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
