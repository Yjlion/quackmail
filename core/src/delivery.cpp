#include "quackmail/delivery.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/itip.hpp"
#include "quackmail/mail_store.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/sieve.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <ctime>

namespace quackmail {
namespace deliver {

using duckdb::Connection;

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

bool LocalDeliver(Connection &con, const std::string &mail_from, const std::vector<std::string> &rcpts,
                  const std::string &body, const Options &opts, Outcome &out) {
	out = Outcome();
	auto parsed = mime::Parse(body);

	// Recipients this message is actually being kept for, which is not the same
	// as `rcpts`: a Sieve reject or discard drops one. Only these get the
	// scheduling side effect below.
	std::vector<std::string> kept;

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

		for (const auto &action : result.actions) {
			switch (action.type) {
			case sieve::Action::KEEP: {
				// An implicit or explicit keep honours the subaddress; an
				// explicit fileinto below outranks it, because a filter the user
				// wrote beats a folder the sender picked.
				int64_t sub = detail_room();
				add_room(sub >= 0 ? sub : citadel::GetOrCreateMailRoom(con, user));
				break;
			}
			case sieve::Action::FILEINTO:
				add_room(citadel::GetOrCreateUserRoom(
				    con, user, action.folder.empty() ? "Mail" : action.folder));
				break;
			case sieve::Action::REDIRECT:
				// Forwarding goes through the same queue the submission service
				// uses, so retries and backoff are handled in one place.
				if (!action.address.empty()) {
					store::EnqueueOutbound(con, mail_from, action.address, body);
				}
				break;
			case sieve::Action::DISCARD:
			case sieve::Action::REJECT:
				break; // handled above
			}
		}
	}

	if (rooms.empty()) {
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
