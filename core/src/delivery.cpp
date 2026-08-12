#include "quackmail/delivery.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/itip.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mailpolicy.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/sieve.hpp"
#include "quackmail/submission.hpp"
#include "quackmail/util.hpp"

#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <ctime>

namespace quackmail {
namespace deliver {

using duckdb::Connection;
using duckdb::Value;

// Case-insensitive header lookup over a parsed message's header list.
static std::string HeaderValue(const mime::ParsedMessage &parsed, const std::string &name) {
	std::string want = util::Upper(name);
	for (auto &h : parsed.headers) {
		if (util::Upper(h.first) == want) {
			return h.second;
		}
	}
	return "";
}

namespace {

// ---- Sieve vacation, the half that needs a database ----------------------
//
// The evaluator has already decided that this *message* deserves an auto-reply:
// every RFC 5230 §4.5/§4.6 rule that reads the message alone is applied there,
// where a test can pin it without a mail server. What is left is the one
// question the message cannot answer — whether we already replied to this
// correspondent inside the window — and actually sending the thing.

// The key a window is kept under. RFC 5230 §4.3: a script that changes its
// message should start a fresh window, so the default handle is derived from
// the text; an explicit :handle overrides that, which is how a user rewords the
// message without replying to everybody a second time.
std::string VacationHandle(const sieve::Action &a) {
	if (!a.vacation.handle.empty()) {
		return a.vacation.handle;
	}
	// A cheap content hash. It only has to change when the text does.
	uint64_t h = 1469598103934665603ULL;
	for (char c : a.vacation.subject + "\n" + a.reason) {
		h = (h ^ (unsigned char)c) * 1099511628211ULL;
	}
	char buf[24];
	snprintf(buf, sizeof(buf), "auto-%016llx", (unsigned long long)h);
	return buf;
}

// True when a reply to `sender` under `handle` already went out inside the
// window, or when recording that we are about to send one fails.
bool VacationAlreadySent(Connection &con, const std::string &user, const std::string &handle,
                         const std::string &sender, int days) {
	auto stmt = con.Prepare("SELECT count(*) FROM quackmail_vacation_sent "
	                        "WHERE username = $1 AND handle = $2 AND lower(sender) = lower($3) "
	                        "AND sent_at > now() - ($4 * INTERVAL 1 DAY)");
	if (stmt->HasError()) {
		return true; // cannot tell: stay silent rather than risk a reply storm
	}
	duckdb::vector<Value> params = {Value(user), Value(handle), Value(sender),
	                                Value::BIGINT((int64_t)days)};
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		return true;
	}
	auto &mat = r->Cast<duckdb::MaterializedQueryResult>();
	return mat.RowCount() > 0 && mat.GetValue(0, 0).GetValue<int64_t>() > 0;
}

void RecordVacation(Connection &con, const std::string &user, const std::string &handle,
                    const std::string &sender) {
	auto stmt = con.Prepare("INSERT INTO quackmail_vacation_sent (username, handle, sender) "
	                        "VALUES ($1, $2, $3)");
	if (stmt->HasError()) {
		return;
	}
	duckdb::vector<Value> params = {Value(user), Value(handle), Value(sender)};
	stmt->Execute(params, false);
	// Keep the table bounded: the longest window we honour is a year.
	con.Query("DELETE FROM quackmail_vacation_sent WHERE sent_at < now() - INTERVAL 366 DAY");
}

// Send one auto-reply. Best effort by design — the message it is replying to has
// already been stored, and failing the delivery because a reply could not go out
// would lose mail to fix a courtesy.
void SendVacation(Connection &con, const std::string &user, const std::string &rcpt,
                  const std::string &sender, const mime::ParsedMessage &parsed,
                  const sieve::Action &a) {
	if (citadel::GetConfig(con, "qm_sieve_vacation", "1") == "0") {
		return;
	}
	std::string handle = VacationHandle(a);
	if (VacationAlreadySent(con, user, handle, sender, a.vacation.days)) {
		return;
	}

	// `:from` is a From-forgery primitive if it is taken on trust: any account
	// holder could auto-reply as anybody. It is honoured only when it is an
	// address this user actually owns, and otherwise ignored in favour of the
	// address the message was addressed to.
	std::string from = rcpt;
	if (!a.vacation.from.empty()) {
		std::string local = util::LocalPart(a.vacation.from);
		auto at = a.vacation.from.rfind('@');
		std::string domain = at == std::string::npos ? "" : a.vacation.from.substr(at + 1);
		if (util::Upper(local) == util::Upper(user) && policy::IsLocalDomain(con, domain)) {
			from = a.vacation.from;
		}
	}

	// The quota is charged here rather than left to submission::Send, which is
	// quota-agnostic by contract. An auto-replier with the limit switched off is
	// a backscatter cannon pointed at whoever forges a sender.
	auto quota = policy::CheckRate(con, user, 1);
	if (!quota.allowed) {
		return;
	}

	std::string fqdn = citadel::GetConfig(con, "c_fqdn", "localhost");
	std::string original = mime::DecodeEncodedWords(HeaderValue(parsed, "Subject"));
	std::string subject = a.vacation.subject.empty() ? "Auto: " + original : a.vacation.subject;

	mime::HeaderList headers;
	headers.push_back({"Message-ID", "<" + util::RandomHex(12) + "." +
	                                     std::to_string((int64_t)std::time(nullptr)) + "@" + fqdn + ">"});
	headers.push_back({"Date", util::RfcDate(0)});
	headers.push_back({"From", from});
	headers.push_back({"To", sender});
	headers.push_back({"Subject", subject});
	std::string msgid = HeaderValue(parsed, "Message-ID");
	if (!msgid.empty()) {
		headers.push_back({"In-Reply-To", msgid});
		headers.push_back({"References", msgid});
	}
	// §4.6 makes this a MUST, and it is what stops two servers both on holiday
	// from writing to each other until one of them runs out of disk: the other
	// side's own VacationRefusal reads it and says nothing back.
	headers.push_back({"Auto-Submitted", "auto-replied"});
	headers.push_back({"Precedence", "bulk"});

	std::vector<mime::BuildPart> parts;
	mime::BuildPart text;
	text.content_type = "text/plain";
	text.content = a.reason;
	parts.push_back(text);
	std::string body = mime::BuildMessage(headers, parts);

	// §4.5: the envelope sender is empty. A bounce of an auto-reply must not
	// come back to the person on holiday, and an empty return-path is the only
	// thing every MTA agrees means that.
	submission::Result res;
	std::string received = submission::ReceivedHeader(con, "sieve", user, false);
	if (submission::Send(con, "", {sender}, received, body, res)) {
		policy::RecordSend(con, user, sender, 1);
		RecordVacation(con, user, handle, sender);
	}
}

} // namespace

bool LocalDeliver(Connection &con, const std::string &mail_from, const std::vector<std::string> &rcpts,
                  const std::string &body, const Options &opts, Outcome &out) {
	out = Outcome();
	auto parsed = mime::Parse(body);

	// Recipients this message is actually being kept for, which is not the same
	// as `rcpts`: a Sieve reject or discard drops one. Only these get the
	// scheduling side effect below.
	std::vector<std::string> kept;

	// Both of these are settled per recipient inside the loop but can only be
	// acted on once the message has a number, so they wait until after the
	// insert. A flag names one user's view of one message; an auto-reply must
	// not go out for a message that then failed to store.
	struct PendingFlags {
		std::string user;
		std::vector<std::string> flags;
	};
	std::vector<PendingFlags> pending_flags;
	struct PendingVacation {
		std::string user;
		std::string rcpt;
		sieve::Action action;
	};
	std::vector<PendingVacation> pending_vacations;

	// Resolve the delivery rooms across all recipients (deduplicated: one stored
	// message is pointed into every destination room).
	std::vector<int64_t> rooms;
	auto add_room = [&](int64_t room) {
		if (room < 0) {
			return;
		}
		if (std::find(rooms.begin(), rooms.end(), room) == rooms.end()) {
			rooms.push_back(room);
		}
	};

	// Rooms named by the envelope itself (a room_<name>@ recipient) rather than
	// by any user's mailbox.
	for (int64_t room : opts.extra_rooms) {
		add_room(room);
	}

	for (const std::string &rcpt : rcpts) {
		std::string user = util::LocalPart(rcpt);

		// The subaddress detail, if this recipient was addressed as user+detail@.
		std::string detail;
		for (const auto &e : opts.subaddress) {
			if (e.first == rcpt) {
				detail = e.second;
			}
		}
		// Where the detail files to, when it names a folder that already exists.
		// An unknown folder falls back to the inbox unless the site opted in to
		// creating them — the sender chooses this name, so it is not trusted.
		auto detail_room = [&]() -> int64_t {
			if (detail.empty()) {
				return -1;
			}
			return opts.subaddress_create ? citadel::GetOrCreateUserRoom(con, user, detail)
			                              : citadel::FindUserRoom(con, user, detail);
		};

		// A site-level quarantine overrides the user's own filter: the point is
		// to keep suspect mail out of the inbox even if a rule would file it there.
		if (!opts.folder_override.empty()) {
			add_room(citadel::GetOrCreateUserRoom(con, user, opts.folder_override));
			kept.push_back(user);
			continue;
		}

		std::string script = sieve::LoadActiveScript(con, user);
		if (script.empty()) {
			int64_t sub = detail_room();
			add_room(sub >= 0 ? sub : citadel::GetOrCreateMailRoom(con, user));
			kept.push_back(user);
			continue;
		}

		sieve::Envelope env(mail_from, rcpt);
		env.separator = opts.subaddress_sep;
		auto result = sieve::Evaluate(script, parsed, body, env);

		std::string reject = result.RejectReason();
		if (!reject.empty()) {
			out.rejected.emplace_back(rcpt, reject);
			continue;
		}
		if (result.IsDiscard()) {
			out.discarded.push_back(rcpt);
			continue;
		}
		kept.push_back(user);

		// imap4flags: the union of what every delivering action asked for. The
		// flags belong to (message, user), and this message is one row however
		// many of the user's folders it lands in, so a per-folder distinction
		// could not be stored even if RFC 5232 wanted one.
		PendingFlags flags;
		flags.user = user;

		for (const auto &action : result.actions) {
			switch (action.type) {
			case sieve::Action::KEEP: {
				// An implicit or explicit keep honours the subaddress; an
				// explicit fileinto below outranks it, because a filter the user
				// wrote beats a folder the sender picked.
				int64_t sub = detail_room();
				add_room(sub >= 0 ? sub : citadel::GetOrCreateMailRoom(con, user));
				flags.flags.insert(flags.flags.end(), action.flags.begin(), action.flags.end());
				break;
			}
			case sieve::Action::FILEINTO:
				add_room(citadel::GetOrCreateUserRoom(
				    con, user, action.folder.empty() ? "Mail" : action.folder));
				flags.flags.insert(flags.flags.end(), action.flags.begin(), action.flags.end());
				break;
			case sieve::Action::REDIRECT:
				// Forwarding goes through the same queue the submission service
				// uses, so retries and backoff are handled in one place.
				if (!action.address.empty()) {
					store::EnqueueOutbound(con, mail_from, action.address, body);
				}
				break;
			case sieve::Action::VACATION: {
				PendingVacation pv;
				pv.user = user;
				pv.rcpt = rcpt;
				pv.action = action;
				pending_vacations.push_back(pv);
				break;
			}
			case sieve::Action::DISCARD:
			case sieve::Action::REJECT:
				break; // handled above
			}
		}
		if (!flags.flags.empty()) {
			pending_flags.push_back(flags);
		}
	}

	// A vacation reply is not a delivery, so it does not depend on the message
	// having been stored — `discard; vacation "away";` is a filter somebody
	// might reasonably write, and it means both halves.
	auto send_vacations = [&]() {
		for (const auto &pv : pending_vacations) {
			SendVacation(con, pv.user, pv.rcpt, mail_from, parsed, pv.action);
		}
	};

	if (rooms.empty()) {
		send_vacations();
		return true; // nothing to store (filtered out, redirected, or rejected)
	}

	citadel::Message msg;
	// Author: the From: display name/address, falling back to the envelope sender.
	if (!parsed.from.empty()) {
		auto addrs = mime::ParseAddressList(parsed.from);
		if (!addrs.empty()) {
			msg.author = !addrs[0].name.empty() ? addrs[0].name : addrs[0].addr;
		}
	}
	if (msg.author.empty()) {
		msg.author = mail_from;
	}
	std::string joined;
	for (size_t i = 0; i < rcpts.size(); i++) {
		joined += (i ? ", " : "") + rcpts[i];
	}
	msg.recipient = joined;
	msg.subject = mime::DecodeEncodedWords(parsed.subject);
	msg.euid = parsed.message_id;
	msg.references = HeaderValue(parsed, "References");
	msg.format_type = 4; // RFC822/MIME
	msg.raw = body;
	int64_t epoch = 0;
	msg.msgtime = mime::ParseDate(HeaderValue(parsed, "Date"), epoch) ? epoch : (int64_t)std::time(nullptr);

	out.msgnum = citadel::InsertMessage(con, msg, rooms, out.err);
	out.ok = out.msgnum >= 0;

	// imap4flags, now that the message has a number. These are the same rows
	// IMAP's STORE writes, so a rule that files something as already-read shows
	// up read in every client at once.
	if (out.ok) {
		for (const auto &pf : pending_flags) {
			for (const auto &flag : pf.flags) {
				citadel::AddMsgFlag(con, out.msgnum, pf.user, flag);
			}
		}
		send_vacations();
	}

	// The one place inbound mail is looked at by content type rather than by
	// address. It sits here, *after* the filters have had their say, on purpose:
	// a user who wrote `discard` for a sender meant it, and quietly acting on a
	// scheduling message they told us to throw away would make the rule a lie.
	// A message that was kept, though, has a calendar effect that should not
	// depend on which folder a rule filed it in.
	//
	// The guard is the cheap substring check inside CalendarPart: almost no mail
	// is a scheduling message, and building the MIME part tree for every inbound
	// message to discover that would be a tax on the whole delivery path.
	if (out.ok && !kept.empty()) {
		std::string method;
		std::string ics = itip::CalendarPart(body, method);
		if (!ics.empty() && !method.empty()) {
			for (const auto &user : kept) {
				std::string sched_err;
				itip::Receive(con, user, ics, method, sched_err);
			}
		}
	}
	return out.ok;
}

bool LocalDeliver(Connection &con, const std::string &mail_from, const std::vector<std::string> &rcpts,
                  const std::string &body, std::string &err) {
	Options opts;
	Outcome out;
	bool ok = LocalDeliver(con, mail_from, rcpts, body, opts, out);
	err = out.err;
	return ok;
}

} // namespace deliver
} // namespace quackmail
