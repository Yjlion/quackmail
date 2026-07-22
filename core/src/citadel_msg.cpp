#include "quackmail/citadel_msg.hpp"

#include "quackmail/mime.hpp"

#include <sstream>

namespace quackmail {
namespace citadel {

std::string BodyText(const Message &msg) {
	if (msg.format_type != 4) {
		return msg.raw; // native format: raw already holds the body text
	}
	// RFC822/MIME: return the first text/* leaf part's decoded content.
	auto root = mime::ParseEntity(msg.raw);
	if (!root.IsMultipart() && root.content_type.type == "text") {
		return root.body_decoded;
	}
	std::string fallback;
	for (auto &part : mime::FlattenParts(root)) {
		if (part.content_type.rfind("text/plain", 0) == 0) {
			return part.content;
		}
		if (fallback.empty() && part.content_type.rfind("text/", 0) == 0) {
			fallback = part.content;
		}
	}
	return fallback;
}

std::vector<std::string> FormatMsg0(const Message &msg, int mode) {
	std::vector<std::string> lines;
	bool want_headers = (mode != 2);
	bool want_body = (mode != 1);

	if (want_headers) {
		lines.push_back("type=" + std::to_string(msg.format_type));
		lines.push_back("msgn=" + (msg.euid.empty() ? std::to_string(msg.msgnum) : msg.euid));
		lines.push_back("time=" + std::to_string(msg.msgtime));
		lines.push_back("from=" + msg.author);
		if (!msg.recipient.empty()) {
			lines.push_back("rcpt=" + msg.recipient);
		}
		if (!msg.origin_room.empty()) {
			lines.push_back("room=" + msg.origin_room);
		}
		if (!msg.node.empty()) {
			lines.push_back("node=" + msg.node);
		}
		if (!msg.subject.empty()) {
			lines.push_back("subj=" + msg.subject);
		}
	}

	if (want_body) {
		lines.push_back("text");
		std::string body = BodyText(msg);
		std::istringstream in(body);
		std::string line;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			lines.push_back(line);
		}
	}
	return lines;
}

} // namespace citadel
} // namespace quackmail
