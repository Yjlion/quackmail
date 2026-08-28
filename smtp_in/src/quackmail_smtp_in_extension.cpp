#define DUCKDB_EXTENSION_MAIN

#include "quackmail_smtp_in_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/delivery.hpp"
#include "quackmail/dkim.hpp"
#include "quackmail/dmarc.hpp"
#include "quackmail/listserv.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mailpolicy.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/quota.hpp"
#include "quackmail/rbl.hpp"
#include "quackmail/server_controller.hpp"
#include "quackmail/server_controls.hpp"
#include "quackmail/spf.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <vector>

namespace duckdb {
namespace {

using namespace quackmail;

// The public MX (25) and the local-injection LMTP socket (24). Two controllers
// over one implementation, parameterized by Mode — see CLAUDE.md.
ServerController g_smtp_in;
ServerController g_lmtp;

constexpr size_t kMaxMessageBytes = 25 * 1024 * 1024;

enum class Mode {
	Mx,  // public inbound SMTP: authenticate the sender, apply site policy
	Lmtp // trusted local injection: routing only, no filtering
};

// Parse "FROM:<addr>" / "TO:<addr>" out of a MAIL/RCPT argument.
std::string ExtractPath(const std::string &arg) {
	auto lt = arg.find('<');
	auto gt = arg.find('>');
	if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
		return arg.substr(lt + 1, gt - lt - 1);
	}
	// Fall back to the token after the ':'.
	auto colon = arg.find(':');
	std::string rest = colon == std::string::npos ? arg : arg.substr(colon + 1);
	// Trim spaces.
	size_t b = rest.find_first_not_of(" \t");
	size_t e = rest.find_last_not_of(" \t");
	return b == std::string::npos ? "" : rest.substr(b, e - b + 1);
}

// Split a command line into upper-cased verb + remainder.
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

// One accepted recipient: what the client asked for, and where it actually goes
// after alias expansion. LMTP must reply once per envelope recipient, so the
// envelope address is kept separate from the resolved targets.
struct Recipient {
	std::string envelope;                  // as given in RCPT TO
	std::vector<std::string> destinations; // resolved local users
	std::vector<std::string> forwards;     // off-site addresses an alias points at
	std::vector<int64_t> rooms;            // public rooms addressed as room_<name>@
	std::string detail;                    // RFC 5233 subaddress part, if any

	// Mailing lists. A command address (-subscribe, -confirm-<token>, ...) is
	// answered at DATA rather than delivered anywhere, and a post the list's
	// policy will not accept from this sender is parked for an aide instead of
	// being filed into the room.
	listserv::List list;
	listserv::Command list_cmd;
	bool list_hold = false;

	bool IsList() const {
		return list_cmd.kind != listserv::Command::None;
	}

	bool Deliverable() const {
		return !destinations.empty() || !forwards.empty() || !rooms.empty() || IsList();
	}

	Recipient() = default;
};

// Everything the per-connection handler carries across commands.
struct Session {
	Mode mode = Mode::Mx;
	std::string client_ip;
	std::string helo;
	std::string mail_from;
	std::vector<Recipient> rcpts;
	bool have_mail = false;
	bool tls_active = false;
	bool ip_allowlisted = false; // an explicit ACL allow skips the DNSBL

	// Results of the inbound checks, held for the trace headers.
	spf::Eval spf_eval;
	bool spf_done = false;
	rbl::Hit rbl_hit;
	bool rbl_checked = false;

	void ResetTransaction() {
		have_mail = false;
		mail_from.clear();
		rcpts.clear();
		spf_eval = spf::Eval();
		spf_done = false;
	}
};

// Build the Authentication-Results header (RFC 8601).
std::string AuthResults(const std::string &authserv, const Session &s,
                        const std::vector<dkim::VerifyResult> &dkim_results, const dmarc::Eval &dm) {
	std::string out = authserv;

	if (s.spf_done) {
		out += ";\r\n\tspf=" + spf::ResultName(s.spf_eval.result);
		if (!s.spf_eval.domain.empty()) {
			out += " smtp.mailfrom=" + s.spf_eval.domain;
		}
	}

	if (dkim_results.empty()) {
		out += ";\r\n\tdkim=none";
	} else {
		for (const auto &d : dkim_results) {
			out += ";\r\n\tdkim=" + dkim::ResultName(d.result);
			if (!d.domain.empty()) {
				out += " header.d=" + d.domain;
			}
			if (!d.selector.empty()) {
				out += " header.s=" + d.selector;
			}
		}
	}

	out += ";\r\n\tdmarc=" + dmarc::ResultName(dm.result);
	if (!dm.from_domain.empty()) {
		out += " header.from=" + dm.from_domain;
	}
	if (dm.result == dmarc::Result::Fail) {
		out += " (p=" + dmarc::PolicyName(dm.policy) + ")";
	}
	return out;
}

// The From: header domain, which is the identity DMARC aligns against.
std::string FromHeaderDomain(const mime::ParsedMessage &parsed) {
	if (parsed.from.empty()) {
		return "";
	}
	auto addrs = mime::ParseAddressList(parsed.from);
	for (const auto &a : addrs) {
		if (!a.addr.empty()) {
			return DomainOf(a.addr);
		}
	}
	return "";
}

// ---------------------------------------------------------------------------
// The handler, shared by the MX and LMTP listeners
// ---------------------------------------------------------------------------

void HandleInbound(DatabaseInstance &db, net::ClientStream &stream, ServerController &ctrl, Mode mode) {
	Connection con(db);
	store::EnsureSchema(con);

	Session s;
	s.mode = mode;
	s.tls_active = stream.IsTls();
	s.client_ip = stream.PeerIp();

	const bool lmtp = mode == Mode::Lmtp;
	auto enforcement = policy::GetEnforcement(con);
	std::string authserv = citadel::GetConfig(con, "c_fqdn", "quackmail.test");

	// Connection-level access control. LMTP is the trusted injection path, so it
	// is exempt: whoever can reach that socket is already inside the trust
	// boundary, and the caller usually connects from localhost with no useful IP.
	if (!lmtp && !s.client_ip.empty()) {
		std::string note;
		auto verdict = policy::CheckAcl(con, "ip", s.client_ip, note);
		if (verdict == policy::AclVerdict::Block) {
			stream.WriteLine("554 5.7.1 " + (note.empty() ? std::string("Access denied") : note));
			return;
		}
		s.ip_allowlisted = verdict == policy::AclVerdict::Allow;
	}

	// RFC 5321 §4.2 and Citadel's own serv_smtp.c both require the server's
	// FQDN to be the first token after the reply code, so it must be the
	// configured c_fqdn rather than a literal.
	stream.WriteLine("220 " + authserv + (lmtp ? " LMTP QuackCit server ready."
	                                           : " ESMTP QuackCit server ready."));

	std::string line;
	while (stream.ReadLine(line, 8192)) {
		std::string verb, rest;
		SplitCommand(line, verb, rest);

		// LMTP replaces EHLO with LHLO and must reject EHLO outright (RFC 2033
		// §4), so a misdirected SMTP client fails loudly instead of silently
		// bypassing the MX checks.
		if ((lmtp && verb == "LHLO") || (!lmtp && verb == "EHLO")) {
			if (!lmtp) {
				std::string note;
				if (policy::CheckAcl(con, "helo", rest, note) == policy::AclVerdict::Block) {
					stream.WriteLine("550 5.7.1 " + (note.empty() ? std::string("HELO rejected") : note));
					continue;
				}
			}
			s.helo = rest;
			stream.WriteLine("250-quackmail greets " + rest);
			if (!s.tls_active && ctrl.StartTlsEnabled()) {
				stream.WriteLine("250-STARTTLS");
			}
			stream.WriteLine("250-8BITMIME");
			stream.WriteLine("250-ENHANCEDSTATUSCODES");
			stream.WriteLine("250 SIZE " + std::to_string(kMaxMessageBytes));
		} else if (verb == "EHLO" && lmtp) {
			stream.WriteLine("500 5.5.1 Use LHLO on the LMTP service");
		} else if (verb == "LHLO" && !lmtp) {
			stream.WriteLine("500 5.5.1 Use EHLO on the SMTP service");
		} else if (verb == "HELO") {
			s.helo = rest;
			stream.WriteLine("250 quackmail");
		} else if (verb == "STARTTLS") {
			if (s.tls_active) {
				stream.WriteLine("503 Already running TLS");
			} else if (!ctrl.StartTlsEnabled()) {
				stream.WriteLine("502 STARTTLS not available");
			} else {
				stream.WriteLine("220 Ready to start TLS");
				std::string terr;
				if (!stream.StartTls(ctrl.TlsCtx(), terr)) {
					return; // handshake failed; drop connection
				}
				s.tls_active = true;
				s.ResetTransaction();
				s.helo.clear();
			}
		} else if (verb == "AUTH") {
			// Neither service authenticates senders: the MX is public, and LMTP
			// is already trusted. Authenticated submission lives in smtp_out.
			stream.WriteLine("503 5.7.0 AUTH not available; use the submission service");
		} else if (verb == "MAIL") {
			std::string from = ExtractPath(rest);

			if (!lmtp) {
				std::string note;
				if (policy::CheckAcl(con, "sender", from, note) == policy::AclVerdict::Block ||
				    policy::CheckAcl(con, "domain", DomainOf(from), note) == policy::AclVerdict::Block) {
					stream.WriteLine("550 5.7.1 " + (note.empty() ? std::string("Sender rejected") : note));
					continue;
				}
				// SPF is evaluated here so its verdict covers the whole
				// transaction and lands in the trace headers below.
				if (!s.client_ip.empty()) {
					spf::Check(s.client_ip, s.helo, from, s.spf_eval);
					s.spf_done = true;
					if (enforcement.spf_reject && s.spf_eval.result == spf::Result::Fail) {
						stream.WriteLine("550 5.7.23 SPF fail: " + s.spf_eval.explanation);
						continue;
					}
				}
			}

			s.mail_from = from;
			s.rcpts.clear();
			s.have_mail = true;
			stream.WriteLine("250 2.1.0 OK");
		} else if (verb == "RCPT") {
			if (!s.have_mail) {
				stream.WriteLine("503 5.5.1 Need MAIL before RCPT");
				continue;
			}
			std::string rcpt = ExtractPath(rest);
			std::string domain = DomainOf(rcpt);

			if (!lmtp) {
				std::string note;
				if (policy::CheckAcl(con, "rcpt", rcpt, note) == policy::AclVerdict::Block) {
					stream.WriteLine("550 5.7.1 " + (note.empty() ? std::string("Recipient rejected") : note));
					continue;
				}
				// The DNSBL check waits until a recipient is named so that an
				// allow-listed recipient (postmaster, abuse) stays reachable
				// even from a listed address.
				if (!s.rbl_checked && !s.ip_allowlisted && !s.client_ip.empty()) {
					auto zones = policy::RblZones(con);
					if (!zones.empty()) {
						rbl::Check(s.client_ip, zones, s.rbl_hit);
					}
					s.rbl_checked = true;
				}
				if (s.rbl_hit.listed && enforcement.rbl_reject) {
					policy::InboundVerdict v;
					v.client_ip = s.client_ip;
					v.helo = s.helo;
					v.mail_from = s.mail_from;
					v.rcpt = rcpt;
					v.rbl = s.rbl_hit.zone + " " + s.rbl_hit.code;
					v.disposition = "reject";
					v.detail = s.rbl_hit.reason;
					policy::LogInbound(con, v);
					stream.WriteLine("554 5.7.1 " + s.client_ip + " is listed by " + s.rbl_hit.zone +
					                 (s.rbl_hit.reason.empty() ? "" : ": " + s.rbl_hit.reason));
					continue;
				}
			}

			// Routing: a hosted domain, then aliases, then a plain local user.
			if (!domain.empty() && !policy::IsLocalDomain(con, domain)) {
				stream.WriteLine("550 5.7.1 Relaying denied");
				continue;
			}

			// Resolution order, in full: a mailing list, then a public room
			// address, then aliases, then a plain local user, and only then the
			// subaddress split. List and room first so a domain catch-all cannot
			// swallow them; subaddress last so an explicit alias for
			// "bob+sales@" still outranks the generic split of the same address.
			Recipient r;
			r.envelope = rcpt;

			bool listserv_on = citadel::GetConfig(con, "qm_listserv", "1") == "1";
			if (listserv_on && listserv::ResolveAddress(con, util::LocalPart(rcpt), r.list, r.list_cmd)) {
				if (r.list_cmd.kind == listserv::Command::Post) {
					// A list is an explicit opt-in by an aide, so posting to it
					// does not additionally need the room's "anyone p" grant that
					// room_<name>@ requires. Whether *this* sender may post is
					// the list's own policy, and is decided here rather than at
					// DATA so a refused post costs one RCPT rather than a body.
					bool may_post = r.list.post_policy == listserv::PostPolicy::Anyone ||
					                (r.list.post_policy == listserv::PostPolicy::Subscribers &&
					                 listserv::IsSubscriber(con, r.list.room_num, s.mail_from));
					if (may_post) {
						r.rooms.push_back(r.list.room_num);
					} else {
						r.list_hold = true;
					}
				}
			}

			if (!r.IsList()) {
				int64_t room = citadel::GetConfig(con, "qm_room_mail", "1") == "1"
				                   ? citadel::ResolveMailRoom(con, util::LocalPart(rcpt))
				                   : -1;
				if (room >= 0) {
					r.rooms.push_back(room);
				}
			}

			auto expand_into = [&](const std::string &addr) {
				auto expanded = policy::ExpandAlias(con, addr);
				for (const auto &dest : expanded) {
					if (citadel::IsLocalUser(con, dest)) {
						r.destinations.push_back(dest);
					} else {
						// An alias may point off-site. Forwarding it is not open
						// relay: the mail is addressed to a domain we host, and an
						// admin configured this destination explicitly.
						r.forwards.push_back(dest);
					}
				}
				if (expanded.empty() && citadel::IsLocalUser(con, addr)) {
					r.destinations.push_back(addr);
				}
				return !r.destinations.empty() || !r.forwards.empty();
			};

			if (r.rooms.empty() && !expand_into(rcpt)) {
				// RFC 5233: "bob+receipts@example.com" is mail for bob, with
				// "receipts" as a hint about where to file it.
				std::string sep = citadel::GetConfig(con, "qm_subaddress_sep", "+");
				std::string local = util::LocalPart(rcpt);
				auto cut = sep.empty() ? std::string::npos : local.find(sep);
				if (cut != std::string::npos) {
					std::string base = local.substr(0, cut);
					if (!domain.empty()) {
						base += "@" + domain;
					}
					if (expand_into(base)) {
						r.detail = local.substr(cut + sep.size());
					}
				}
			}
			if (!r.Deliverable()) {
				stream.WriteLine("550 5.1.1 No such user here");
				continue;
			}
			// Storage quota, at RCPT rather than at DATA: a full mailbox should
			// cost the sender one command, not a whole message body.
			//
			// IsOver and not WouldExceed, because this server advertises SIZE but
			// never reads the parameter back off MAIL FROM — there is no size to
			// test yet. The sized check is in LocalDeliver, which has the bytes.
			//
			// 452 4.2.2 and not 552: a mailbox that is full now may not be full in
			// an hour, and a transient code is what makes a sending MTA hold the
			// message and retry rather than bounce it to a stranger.
			//
			// Refused only when *every* destination is full — one envelope
			// recipient may expand through an alias to several users, and one of
			// them having room is reason enough to take the mail. This is the same
			// rule reject_reason() applies at DATA.
			if (!r.destinations.empty()) {
				bool all_full = true;
				for (const auto &dest : r.destinations) {
					if (!quota::IsOver(con, util::LocalPart(dest))) {
						all_full = false;
						break;
					}
				}
				if (all_full) {
					stream.WriteLine("452 4.2.2 Mailbox full");
					continue;
				}
			}
			s.rcpts.push_back(std::move(r));
			stream.WriteLine("250 2.1.5 OK");
		} else if (verb == "DATA") {
			if (!s.have_mail || s.rcpts.empty()) {
				stream.WriteLine("503 5.5.1 Need MAIL and RCPT before DATA");
				continue;
			}
			stream.WriteLine("354 End data with <CR><LF>.<CR><LF>");
			std::string body;
			if (!stream.ReadDotStuffed(body, kMaxMessageBytes)) {
				stream.WriteLine("552 5.3.4 Message too large or read error");
				return;
			}

			auto parsed = mime::Parse(body);
			std::string from_domain = FromHeaderDomain(parsed);

			// --- authentication, MX only -----------------------------------
			std::vector<dkim::VerifyResult> dkim_results;
			dmarc::Eval dm;
			std::string trace;
			if (!lmtp) {
				// Verify over the bytes exactly as received: the trace headers
				// below are prepended afterwards so they cannot disturb a
				// signature that covers the existing header set.
				dkim_results = dkim::Verify(body, policy::DkimKeyLookup(con));
				dm = dmarc::Evaluate(from_domain, s.spf_eval.domain, s.spf_eval.result, dkim_results,
				                     parsed.message_id);

				bool dkim_failed = false;
				for (const auto &d : dkim_results) {
					if (d.result == dkim::Result::Fail) {
						dkim_failed = true;
					}
				}
				if (enforcement.dkim_reject && dkim_failed) {
					policy::InboundVerdict v;
					v.client_ip = s.client_ip;
					v.helo = s.helo;
					v.mail_from = s.mail_from;
					v.spf = spf::ResultName(s.spf_eval.result);
					v.dkim = "fail";
					v.dmarc = dmarc::ResultName(dm.result);
					v.disposition = "reject";
					v.detail = "DKIM signature failed";
					policy::LogInbound(con, v);
					stream.WriteLine("550 5.7.20 DKIM signature verification failed");
					s.ResetTransaction();
					continue;
				}
				if (enforcement.dmarc_enforce && dm.ShouldReject()) {
					policy::InboundVerdict v;
					v.client_ip = s.client_ip;
					v.helo = s.helo;
					v.mail_from = s.mail_from;
					v.spf = spf::ResultName(s.spf_eval.result);
					v.dmarc = dmarc::ResultName(dm.result);
					v.disposition = "reject";
					v.detail = dm.info;
					policy::LogInbound(con, v);
					stream.WriteLine("550 5.7.1 " + from_domain +
					                 " publishes DMARC p=reject and this message is not aligned");
					s.ResetTransaction();
					continue;
				}

				trace = "Authentication-Results: " + AuthResults(authserv, s, dkim_results, dm) + "\r\n";
				if (s.spf_done) {
					trace += "Received-SPF: " +
					         spf::ReceivedSpf(s.spf_eval, s.client_ip, s.helo, s.mail_from, authserv) +
					         "\r\n";
				}
			}

			// --- trace header ----------------------------------------------
			std::string received = "Received: from " + (s.helo.empty() ? "unknown" : s.helo);
			if (!s.client_ip.empty()) {
				received += " ([" + s.client_ip + "])";
			}
			received += "\r\n\tby " + authserv + " (QuackCit) with " +
			            (lmtp ? "LMTP" : (s.tls_active ? "ESMTPS" : "ESMTP"));
			received += ";\r\n\t" + util::RfcDate() + "\r\n";

			bool quarantine = !lmtp && enforcement.dmarc_enforce && dm.ShouldQuarantine();
			std::string headers = received + trace;
			if (quarantine) {
				headers += "X-Quackmail-Quarantine: dmarc=fail policy=quarantine\r\n";
			}
			std::string stored = headers + body;

			// --- delivery ---------------------------------------------------
			deliver::Options opts;
			if (quarantine) {
				opts.folder_override = enforcement.quarantine_room;
			}
			opts.subaddress_sep = citadel::GetConfig(con, "qm_subaddress_sep", "+");
			opts.subaddress_create = citadel::GetConfig(con, "qm_subaddress_create", "0") == "1";

			// LMTP owes one reply per envelope recipient, so each is delivered
			// and reported separately. SMTP delivers them together.
			auto log_one = [&](const std::string &rcpt, const std::string &disposition,
			                   const std::string &detail) {
				policy::InboundVerdict v;
				v.client_ip = s.client_ip;
				v.helo = s.helo;
				v.mail_from = s.mail_from;
				v.rcpt = rcpt;
				v.spf = s.spf_done ? spf::ResultName(s.spf_eval.result) : "";
				v.dkim = dkim_results.empty() ? "none" : dkim::ResultName(dkim_results[0].result);
				v.dmarc = lmtp ? "" : dmarc::ResultName(dm.result);
				v.rbl = s.rbl_hit.listed ? s.rbl_hit.zone : "";
				v.disposition = disposition;
				v.detail = detail;
				policy::LogInbound(con, v);
			};

			// Alias forwards go onto the same outbound queue the submission
			// service uses, so retries and backoff live in one place.
			auto enqueue_forwards = [&](const Recipient &r) {
				for (const auto &fwd : r.forwards) {
					store::EnqueueOutbound(con, s.mail_from, fwd, stored);
				}
			};

			// One delivery for the whole transaction, in both modes: the store is
			// reference-counted, so a message to several local users is stored
			// once and pointed into each room. Delivering per recipient would
			// store a duplicate copy per recipient.
			// Mailing-list traffic is settled before ordinary delivery. A command
			// address is answered and never stored; a post the list will not take
			// from this sender is parked for an aide. Both leave the recipient
			// with nothing to deliver, so neither reaches LocalDeliver below.
			for (auto &r : s.rcpts) {
				if (!r.IsList()) {
					continue;
				}
				std::string list_err;
				if (r.list_cmd.kind == listserv::Command::Post) {
					if (r.list_hold) {
						listserv::Hold(con, r.list, s.mail_from, stored, list_err);
					}
					continue; // an accepted post is an ordinary room delivery
				}
				listserv::HandleCommand(con, r.list, r.list_cmd, s.mail_from, body, list_err);
			}

			std::vector<std::string> targets;
			for (const auto &r : s.rcpts) {
				enqueue_forwards(r);
				targets.insert(targets.end(), r.destinations.begin(), r.destinations.end());
				opts.extra_rooms.insert(opts.extra_rooms.end(), r.rooms.begin(), r.rooms.end());
				if (!r.detail.empty()) {
					for (const auto &dest : r.destinations) {
						opts.subaddress.push_back({dest, r.detail});
					}
				}
			}
			deliver::Outcome outcome;
			bool ok = (targets.empty() && opts.extra_rooms.empty())
			              ? true // everything was forwarded; nothing to store
			              : deliver::LocalDeliver(con, s.mail_from, targets, stored, opts, outcome);

			// The per-recipient Sieve verdicts come back keyed by destination,
			// so an envelope recipient is refused only if every destination it
			// expanded to was refused.
			auto reject_reason = [&](const Recipient &r) -> std::string {
				if (r.destinations.empty()) {
					return "";
				}
				std::string reason;
				for (const auto &dest : r.destinations) {
					bool found = false;
					for (const auto &rej : outcome.rejected) {
						if (rej.first == dest) {
							found = true;
							reason = rej.second;
							break;
						}
					}
					if (!found) {
						return ""; // at least one destination accepted it
					}
				}
				return reason;
			};

			// The same fold for a full mailbox. Separate from reject_reason
			// because the two earn different codes: a Sieve reject is the user's
			// own permanent decision (5.x.x), a full mailbox is a condition that
			// clears (4.x.x), and folding them would turn a deferral the sender
			// would have retried into a bounce they cannot.
			auto over_quota = [&](const Recipient &r) -> bool {
				if (r.destinations.empty()) {
					return false;
				}
				for (const auto &dest : r.destinations) {
					bool found = false;
					for (const auto &full : outcome.over_quota) {
						if (full == dest) {
							found = true;
							break;
						}
					}
					if (!found) {
						return false; // at least one destination had room
					}
				}
				return true;
			};

			// What happened to one envelope recipient, in words, for both the
			// audit log and the LMTP reply.
			auto list_detail = [&](const Recipient &r) -> std::string {
				if (!r.IsList()) {
					return "";
				}
				if (r.list_cmd.kind != listserv::Command::Post) {
					return "list command accepted";
				}
				return r.list_hold ? "held for moderation" : "posted to list " + r.list.address;
			};

			for (const auto &r : s.rcpts) {
				std::string refused = ok ? reject_reason(r) : "";
				bool full = ok && refused.empty() && over_quota(r);
				const char *disposition = (!ok || full) ? "defer"
				                                        : (!refused.empty() ? "reject"
				                                                            : (quarantine ? "quarantine" : "accept"));
				std::string list_note = list_detail(r);
				log_one(r.envelope, disposition,
				        !ok ? outcome.err
				            : (full ? std::string("mailbox full")
				                    : (!list_note.empty()
				                           ? list_note
				                           : (!r.destinations.empty()
				                                  ? refused
				                                  : (!r.rooms.empty() ? std::string("delivered to room")
				                                                      : "forwarded")))));

				// LMTP owes one reply per envelope recipient; SMTP sends a
				// single reply for the transaction, emitted after this loop.
				if (!lmtp) {
					continue;
				}
				if (!ok) {
					stream.WriteLine("451 4.3.0 <" + r.envelope + "> local storage error");
				} else if (full) {
					stream.WriteLine("452 4.2.2 <" + r.envelope + "> mailbox full");
				} else if (!refused.empty()) {
					stream.WriteLine("550 5.7.1 <" + r.envelope + "> " + refused);
				} else if (!list_note.empty()) {
					stream.WriteLine("250 2.0.0 <" + r.envelope + "> " + list_note);
				} else if (!r.rooms.empty() && r.destinations.empty()) {
					stream.WriteLine("250 2.0.0 <" + r.envelope + "> delivered to room");
				} else if (r.destinations.empty()) {
					stream.WriteLine("250 2.0.0 <" + r.envelope + "> forwarded");
				} else {
					stream.WriteLine("250 2.0.0 <" + r.envelope + "> delivered");
				}
			}

			if (!lmtp) {
				// SMTP has one reply for the whole transaction, so a full mailbox
				// defers it only when *every* envelope recipient was full — the
				// same rule RCPT applies, and for the same reason. A recipient
				// that resolved to a public room or an off-site forward is never
				// over quota, so those keep the message accepted.
				bool all_full = !s.rcpts.empty();
				for (const auto &r : s.rcpts) {
					if (!over_quota(r)) {
						all_full = false;
						break;
					}
				}
				if (!ok) {
					stream.WriteLine("451 4.3.0 Local storage error");
				} else if (all_full) {
					stream.WriteLine("452 4.2.2 Mailbox full");
				} else if (!outcome.rejected.empty()) {
					stream.WriteLine("550 5.7.1 " + outcome.rejected[0].second);
				} else {
					stream.WriteLine("250 2.0.0 OK: message accepted");
				}
			}
			s.ResetTransaction();
		} else if (verb == "RSET") {
			s.ResetTransaction();
			stream.WriteLine("250 2.0.0 OK");
		} else if (verb == "NOOP") {
			stream.WriteLine("250 2.0.0 OK");
		} else if (verb == "VRFY") {
			stream.WriteLine("252 2.5.2 Cannot VRFY user");
		} else if (verb == "QUIT") {
			stream.WriteLine("221 2.0.0 quackmail closing connection");
			return;
		} else {
			stream.WriteLine("500 5.5.2 Unknown command");
		}
	}
}

// Inbound MX: accept mail only for local recipients and deliver it into their
// Citadel Mail rooms. This is the public-facing MTA, so it offers no AUTH and
// never relays; authenticated submission for outbound mail lives in smtp_out.
void HandleSmtp(DatabaseInstance &db, net::ClientStream &stream) {
	HandleInbound(db, stream, g_smtp_in, Mode::Mx);
}

// LMTP (RFC 2033): the trusted local-injection path. It skips every spam check
// — SPF, DKIM, DMARC and the DNSBL — because the caller is assumed to be a
// local process that has already made its own decision. It still applies domain
// routing, alias expansion and the recipient's Sieve filter, which are
// addressing rather than filtering. Bind it to loopback.
void HandleLmtp(DatabaseInstance &db, net::ClientStream &stream) {
	HandleInbound(db, stream, g_lmtp, Mode::Lmtp);
}

void LoadInternal(ExtensionLoader &loader) {
	Connection con(loader.GetDatabaseInstance());
	store::EnsureSchema(con);
	RegisterServerControls(loader, "qm_smtp_in", 2525, g_smtp_in, HandleSmtp);
	// 24 is LMTP's assigned port; 2033 is the dev default here, matching the
	// pattern of every other listener in this repo.
	RegisterServerControls(loader, "qm_lmtp", 2033, g_lmtp, HandleLmtp);
}

} // namespace

void QuackmailSmtpInExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackmailSmtpInExtension::Name() {
	return "quackmail_smtp_in";
}
std::string QuackmailSmtpInExtension::Version() const {
#ifdef EXT_VERSION_QUACKMAIL_SMTP_IN
	return EXT_VERSION_QUACKMAIL_SMTP_IN;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(quackmail_smtp_in, loader) {
	duckdb::LoadInternal(loader);
}
}
