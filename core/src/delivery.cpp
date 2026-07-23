#include "quackmail/delivery.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/mime.hpp"
#include "quackmail/sieve.hpp"
#include "quackmail/util.hpp"

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
                  const std::string &body, std::string &err) {
	auto parsed = mime::Parse(body);

	// Resolve the delivery room for each local recipient (deduplicated).
	std::vector<int64_t> rooms;
	auto add_room = [&](int64_t room) {
		if (room < 0) {
			return;
		}
		for (int64_t r : rooms) {
			if (r == room) {
				return;
			}
		}
		rooms.push_back(room);
	};

	for (const std::string &rcpt : rcpts) {
		std::string user = util::LocalPart(rcpt);
		std::string folder = "Mail";
		std::string script = sieve::LoadActiveScript(con, user);
		if (!script.empty()) {
			auto action = sieve::Evaluate(script, parsed);
			if (action.type == sieve::Action::DISCARD) {
				continue; // silently dropped by the user's filter
			}
			if (action.type == sieve::Action::FILEINTO && !action.folder.empty()) {
				folder = action.folder;
			}
		}
		add_room(citadel::GetOrCreateUserRoom(con, user, folder));
	}

	if (rooms.empty()) {
		return true; // nothing to deliver (all recipients filtered out)
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

	return citadel::InsertMessage(con, msg, rooms, err) >= 0;
}

} // namespace deliver
} // namespace quackmail
