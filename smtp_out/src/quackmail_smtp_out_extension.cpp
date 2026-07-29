#define DUCKDB_EXTENSION_MAIN

#include "quackmail_smtp_out_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/delivery.hpp"
#include "quackmail/dkim.hpp"
#include "quackmail/dns.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mailpolicy.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/sasl.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/smtp_client.hpp"
#include "quackmail/util.hpp"
#include "quackmail/worker.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <vector>

namespace duckdb {
namespace {

using namespace quackmail;

// Two submission listeners (STARTTLS on 2587, implicit-TLS on 2465). They differ
// only in TLS mode, so both run the same handler parameterized by their
// controller.
ServerController g_submission; // 2587, STARTTLS
ServerController g_smtps;      // 2465, implicit TLS

constexpr size_t kMaxMessageBytes = 25 * 1024 * 1024;

std::string ExtractPath(const std::string &arg) {
	auto lt = arg.find('<');
	auto gt = arg.find('>');
	if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
		return arg.substr(lt + 1, gt - lt - 1);
	}
	auto colon = arg.find(':');
	std::string rest = colon == std::string::npos ? arg : arg.substr(colon + 1);
	size_t b = rest.find_first_not_of(" \t");
	size_t e = rest.find_last_not_of(" \t");
	return b == std::string::npos ? "" : rest.substr(b, e - b + 1);
}

void SplitCommand(const std::string &line, std::string &verb, std::string &rest) {
	size_t sp = line.find(' ');
	if (sp == std::string::npos) {
		verb = util::Upper(line);
		rest.clear();
	} else {
		verb = util::Upper(line.substr(0, sp));
		rest = line.substr(sp + 1);
	}
}

std::string LowerStr(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string DomainOf(const std::string &addr) {
	auto at = addr.rfind('@');
	return at == std::string::npos ? "" : LowerStr(addr.substr(at + 1));
}

// Sign a submitted message with the key for its author domain, if we hold one.
// The From: header domain is what receivers align against for DMARC, so it is
// preferred over the envelope sender. Returns the message unchanged when no key
// is configured — an unsigned message is still deliverable.
std::string SignOutbound(Connection &con, const std::string &mail_from, const std::string &body) {
	auto parsed = mime::Parse(body);
	std::string domain;
	if (!parsed.from.empty()) {
		auto addrs = mime::ParseAddressList(parsed.from);
		for (const auto &a : addrs) {
			if (!a.addr.empty()) {
				domain = DomainOf(a.addr);
				break;
			}
		}
	}
	if (domain.empty()) {
		domain = DomainOf(mail_from);
	}
	if (domain.empty()) {
		return body;
	}

	policy::DkimKey key;
	if (!policy::DkimKeyFor(con, domain, key) || key.private_key.empty()) {
		return body;
	}
	std::string signed_body, err;
	if (!dkim::Sign(body, key.domain.empty() ? domain : key.domain, key.selector, key.private_key,
	                key.headers, signed_body, err)) {
		return body; // signing failure must not block the mail
	}
	return signed_body;
}

// Authenticated submission (RFC 6409): require SASL AUTH (only after TLS), then
// accept mail for any destination — deliver local recipients into their rooms
// and enqueue remote recipients for the relay drainer.
void HandleSubmission(DatabaseInstance &db, net::ClientStream &stream, ServerController &ctrl) {
	Connection con(db);
	store::EnsureSchema(con);

	bool tls_active = stream.IsTls();
	bool authed = false;
	std::string auth_user;
	std::string helo;
	std::string mail_from;
	std::vector<std::string> rcpts;
	bool have_mail = false;

	// The FQDN must lead the banner (RFC 5321 §4.2), so use the configured
	// c_fqdn rather than a literal.
	stream.WriteLine("220 " + citadel::GetConfig(con, "c_fqdn", "quackmail.test") +
	                 " ESMTP QuackCit submission ready.");

	std::string line;
	while (stream.ReadLine(line, 8192)) {
		std::string verb, rest;
		SplitCommand(line, verb, rest);

		if (verb == "EHLO") {
			helo = rest;
			stream.WriteLine("250-quackmail greets " + rest);
			if (!tls_active && ctrl.StartTlsEnabled()) {
				stream.WriteLine("250-STARTTLS");
			}
			if (tls_active) {
				stream.WriteLine("250-AUTH PLAIN LOGIN");
			}
			stream.WriteLine("250 SIZE " + std::to_string(kMaxMessageBytes));
		} else if (verb == "HELO") {
			helo = rest;
			stream.WriteLine("250 quackmail");
		} else if (verb == "STARTTLS") {
			if (tls_active) {
				stream.WriteLine("503 Already running TLS");
			} else if (!ctrl.StartTlsEnabled()) {
				stream.WriteLine("502 STARTTLS not available");
			} else {
				stream.WriteLine("220 Ready to start TLS");
				std::string terr;
				if (!stream.StartTls(ctrl.TlsCtx(), terr)) {
					return;
				}
				tls_active = true;
				authed = false;
				auth_user.clear();
				helo.clear();
				have_mail = false;
				mail_from.clear();
				rcpts.clear();
			}
		} else if (verb == "AUTH") {
			if (!tls_active) {
				stream.WriteLine("538 5.7.11 Encryption required for requested authentication mechanism");
			} else if (authed) {
				stream.WriteLine("503 Already authenticated");
			} else {
				std::string mech, initial;
				SplitCommand(rest, mech, initial);
				auto challenge = [&](const std::string &c, std::string &resp) -> bool {
					if (!stream.WriteLine("334 " + c)) {
						return false;
					}
					return stream.ReadLine(resp, 8192);
				};
				auto r = sasl::ServerAuth(con, mech, initial, challenge, auth_user);
				if (r == sasl::Result::Ok) {
					authed = true;
					stream.WriteLine("235 Authentication successful");
				} else if (r == sasl::Result::Unsupported) {
					stream.WriteLine("504 Unrecognized authentication mechanism");
				} else {
					stream.WriteLine("535 Authentication failed");
				}
			}
		} else if (verb == "MAIL") {
			if (!authed) {
				stream.WriteLine("530 5.7.0 Authentication required");
			} else {
				mail_from = ExtractPath(rest);
				rcpts.clear();
				have_mail = true;
				stream.WriteLine("250 OK");
			}
		} else if (verb == "RCPT") {
			if (!authed) {
				stream.WriteLine("530 5.7.0 Authentication required");
			} else if (!have_mail) {
				stream.WriteLine("503 Need MAIL before RCPT");
			} else {
				rcpts.push_back(ExtractPath(rest));
				stream.WriteLine("250 OK");
			}
		} else if (verb == "DATA") {
			if (!authed) {
				stream.WriteLine("530 5.7.0 Authentication required");
				continue;
			}
			if (!have_mail || rcpts.empty()) {
				stream.WriteLine("503 Need MAIL and RCPT before DATA");
				continue;
			}
			// Quota is checked before the message is read, so a user over their
			// limit is turned away without transferring megabytes first. One
			// unit is charged per envelope recipient.
			auto quota = policy::CheckRate(con, auth_user, (int64_t)rcpts.size());
			if (!quota.allowed) {
				// 4xx, not 5xx: the message is fine, the user is merely early.
				stream.WriteLine("451 4.7.1 " + quota.reason + "; retry in " +
				                 std::to_string(quota.retry_after) + " seconds");
				have_mail = false;
				mail_from.clear();
				rcpts.clear();
				continue;
			}

			stream.WriteLine("354 End data with <CR><LF>.<CR><LF>");
			std::string body;
			if (!stream.ReadDotStuffed(body, kMaxMessageBytes)) {
				stream.WriteLine("552 Message too large or read error");
				return;
			}

			// Stamp the submission, then sign. Signing last means the
			// DKIM-Signature sits at the very top, and both the locally
			// delivered copy and every queued copy carry the same signature.
			std::string received = "Received: from " + (helo.empty() ? std::string("unknown") : helo) +
			                       " (submission, authenticated as " + auth_user + ")\r\n\tby " +
			                       citadel::GetConfig(con, "c_fqdn", "quackmail.test") +
			                       " (QuackCit) with " + (tls_active ? "ESMTPSA" : "ESMTPA") + ";\r\n\t" +
			                       util::RfcDate() + "\r\n";
			body = SignOutbound(con, mail_from, received + body);

			// Local recipients are delivered directly; remote ones are queued.
			std::vector<std::string> local_rcpts;
			for (auto &r : rcpts) {
				if (citadel::IsLocalUser(con, r)) {
					local_rcpts.push_back(r);
				} else {
					store::EnqueueOutbound(con, mail_from, r, body);
				}
			}
			std::string err;
			bool ok = true;
			if (!local_rcpts.empty()) {
				ok = deliver::LocalDeliver(con, mail_from, local_rcpts, body, err);
			}
			if (ok) {
				// Charge the quota only for a message we actually accepted.
				for (auto &r : rcpts) {
					policy::RecordSend(con, auth_user, r, 1);
				}
				policy::PruneSendLog(con);
			}
			stream.WriteLine(ok ? "250 OK: message accepted" : "451 Local storage error");
			have_mail = false;
			mail_from.clear();
			rcpts.clear();
		} else if (verb == "RSET") {
			have_mail = false;
			mail_from.clear();
			rcpts.clear();
			stream.WriteLine("250 OK");
		} else if (verb == "NOOP") {
			stream.WriteLine("250 OK");
		} else if (verb == "QUIT") {
			stream.WriteLine("221 quackmail closing connection");
			return;
		} else {
			stream.WriteLine("500 Unknown command");
		}
	}
}

void HandleMsa(DatabaseInstance &db, net::ClientStream &stream) {
	HandleSubmission(db, stream, g_submission);
}
void HandleSmtps(DatabaseInstance &db, net::ClientStream &stream) {
	HandleSubmission(db, stream, g_smtps);
}

// ---------------------------------------------------------------------------
// Relay drainer: a background thread that delivers queued outbound mail via a
// smarthost (if configured) or direct-to-MX.
// ---------------------------------------------------------------------------

class RelayManager {
public:
	~RelayManager() {
		std::string e;
		Stop(e);
	}

	bool Start(DatabaseInstance &db, int poll_secs, const std::string &sh_host, int sh_port,
	           const std::string &sh_user, const std::string &sh_pass, std::string &err) {
		if (worker_.IsRunning()) {
			err = "relay already running";
			return false;
		}
		sh_host_ = sh_host;
		sh_port_ = sh_port > 0 ? sh_port : 25;
		sh_user_ = sh_user;
		sh_pass_ = sh_pass;
		return worker_.Start(db, poll_secs > 0 ? poll_secs : 30, [this](Connection &con) { Drain(con); },
		                     err);
	}

	bool Stop(std::string &err) {
		return worker_.Stop(err);
	}

	bool IsRunning() {
		return worker_.IsRunning();
	}
	int PollSecs() {
		return worker_.PollSecs();
	}
	std::string Smarthost() {
		return sh_host_;
	}

private:
	void Drain(Connection &con) {
		for (auto &it : store::ClaimOutboundDue(con, 20)) {
			ProcessOne(con, it);
		}
	}

	void ProcessOne(Connection &con, const store::OutboundItem &it) {
		smtp::ClientOpts opts;
		smtp::SendResult res;
		if (!sh_host_.empty()) {
			opts.auth_user = sh_user_;
			opts.auth_pass = sh_pass_;
			res = smtp::Deliver(sh_host_, sh_port_, it.from_addr, it.rcpt, it.raw, opts);
		} else {
			auto at = it.rcpt.find('@');
			std::string domain = at == std::string::npos ? "" : it.rcpt.substr(at + 1);
			std::vector<dns::MxHost> mx;
			if (domain.empty() || !dns::LookupMX(domain, mx) || mx.empty()) {
				store::MarkFailed(con, it.id, "no MX for domain '" + domain + "'");
				return;
			}
			res.status = smtp::SendStatus::Transient;
			res.info = "no MX host reachable";
			for (auto &h : mx) {
				res = smtp::Deliver(h.host, 25, it.from_addr, it.rcpt, it.raw, opts);
				if (res.status == smtp::SendStatus::Sent || res.status == smtp::SendStatus::Permanent) {
					break;
				}
			}
		}

		switch (res.status) {
		case smtp::SendStatus::Sent:
			store::MarkSent(con, it.id);
			break;
		case smtp::SendStatus::Permanent:
			store::MarkFailed(con, it.id, res.info);
			break;
		case smtp::SendStatus::Transient: {
			int attempts = it.attempts + 1;
			if (attempts >= 10) {
				store::MarkFailed(con, it.id, "gave up after " + std::to_string(attempts) + " attempts: " + res.info);
			} else {
				store::MarkRetry(con, it.id, attempts, 60 * attempts, res.info);
			}
			break;
		}
		}
	}

	PeriodicWorker worker_;
	std::string sh_host_;
	int sh_port_ = 25;
	std::string sh_user_;
	std::string sh_pass_;
};

RelayManager g_relay;

// ---- relay control table functions (qm_smtp_relay_start/_stop/_status) ------

enum class RelayAction { START, STOP, STATUS };

struct RelayInfo : public TableFunctionInfo {
	RelayAction action = RelayAction::STATUS;
	std::string name;
};

struct RelayBindData : public FunctionData {
	RelayInfo *info = nullptr;
	int poll_secs = 30;
	std::string sh_host;
	int sh_port = 25;
	std::string sh_user;
	std::string sh_pass;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<RelayBindData>(*this);
	}
	bool Equals(const FunctionData &) const override {
		return false;
	}
};

struct RelayGlobalState : public GlobalTableFunctionState {
	bool emitted = false;
	std::string action;
	bool running = false;
	std::string smarthost;
	int64_t poll = 0;
	std::string note;
};

unique_ptr<FunctionData> RelayBind(ClientContext &, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<string> &names) {
	auto info = reinterpret_cast<RelayInfo *>(input.info.get());
	auto result = make_uniq<RelayBindData>();
	result->info = info;
	if (info->action == RelayAction::START) {
		for (auto &kv : input.named_parameters) {
			auto key = StringUtil::Lower(kv.first);
			if (key == "poll_secs") {
				result->poll_secs = kv.second.GetValue<int32_t>();
			} else if (key == "smarthost_host") {
				result->sh_host = kv.second.ToString();
			} else if (key == "smarthost_port") {
				result->sh_port = kv.second.GetValue<int32_t>();
			} else if (key == "smarthost_user") {
				result->sh_user = kv.second.ToString();
			} else if (key == "smarthost_pass") {
				result->sh_pass = kv.second.ToString();
			}
		}
	}
	names = {"action", "running", "smarthost", "poll_secs", "note"};
	return_types = {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::BIGINT,
	                LogicalType::VARCHAR};
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> RelayInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<RelayBindData>();
	auto info = bind.info;
	auto gstate = make_uniq<RelayGlobalState>();
	auto &db = *context.db;

	std::string note;
	switch (info->action) {
	case RelayAction::START: {
		std::string err;
		bool ok = g_relay.Start(db, bind.poll_secs, bind.sh_host, bind.sh_port, bind.sh_user, bind.sh_pass, err);
		note = ok ? "started" : ("error: " + err);
		break;
	}
	case RelayAction::STOP: {
		std::string err;
		g_relay.Stop(err);
		note = "stopped";
		break;
	}
	case RelayAction::STATUS:
		note = g_relay.IsRunning() ? "running" : "stopped";
		break;
	}

	gstate->action = info->name;
	gstate->running = g_relay.IsRunning();
	gstate->smarthost = g_relay.Smarthost();
	gstate->poll = g_relay.PollSecs();
	gstate->note = note;
	return std::move(gstate);
}

void RelayFunc(ClientContext &, TableFunctionInput &data, DataChunk &output) {
	auto &g = data.global_state->Cast<RelayGlobalState>();
	if (g.emitted) {
		output.SetCardinality(0);
		return;
	}
	output.SetCardinality(1);
	output.SetValue(0, 0, Value(g.action));
	output.SetValue(1, 0, Value::BOOLEAN(g.running));
	output.SetValue(2, 0, Value(g.smarthost));
	output.SetValue(3, 0, Value::BIGINT(g.poll));
	output.SetValue(4, 0, Value(g.note));
	g.emitted = true;
}

void RegisterRelayOne(ExtensionLoader &loader, const std::string &name, RelayAction action, bool with_params) {
	TableFunction f(name, {}, RelayFunc, RelayBind, RelayInit);
	if (with_params) {
		f.named_parameters["poll_secs"] = LogicalType::INTEGER;
		f.named_parameters["smarthost_host"] = LogicalType::VARCHAR;
		f.named_parameters["smarthost_port"] = LogicalType::INTEGER;
		f.named_parameters["smarthost_user"] = LogicalType::VARCHAR;
		f.named_parameters["smarthost_pass"] = LogicalType::VARCHAR;
	}
	auto info = make_shared_ptr<RelayInfo>();
	info->action = action;
	info->name = name;
	f.function_info = std::move(info);
	loader.RegisterFunction(f);
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	// Two submission endpoints (see deploy launcher for the TLS-mode wiring).
	RegisterServerControls(loader, "qm_smtp_submission", 2587, g_submission, HandleMsa);
	RegisterServerControls(loader, "qm_smtp_smtps", 2465, g_smtps, HandleSmtps);
	// Outbound relay drainer.
	RegisterRelayOne(loader, "qm_smtp_relay_start", RelayAction::START, true);
	RegisterRelayOne(loader, "qm_smtp_relay_stop", RelayAction::STOP, false);
	RegisterRelayOne(loader, "qm_smtp_relay_status", RelayAction::STATUS, false);
}

} // namespace

void QuackmailSmtpOutExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailSmtpOutExtension::Name() {
	return "quackmail_smtp_out";
}
std::string QuackmailSmtpOutExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_SMTP_OUT
	return EXT_VERSION_QUACKMAIL_SMTP_OUT;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_smtp_out, loader) {
	duckdb::LoadInternal(loader);
}
}
