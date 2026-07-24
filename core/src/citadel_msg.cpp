#include "quackmail/citadel_msg.hpp"

#include "quackmail/mime.hpp"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace quackmail {
namespace citadel {
namespace {

// Citadel derives the address local-part from the author's display name by
// lowercasing it and replacing every non-alphanumeric character with '_'
// ("Citadel Sysop" -> "citadel_sysop"), then appends the node name.
std::string AddressLocalPart(const std::string &author) {
	std::string out;
	out.reserve(author.size());
	for (unsigned char c : author) {
		out += std::isalnum(c) ? (char)std::tolower(c) : '_';
	}
	return out;
}

// RFC822 date, e.g. "Thu, 23 Jul 2026 18:27:25 -0400" (local time, like Citadel).
std::string Rfc822Date(int64_t epoch) {
	time_t t = (time_t)epoch;
	struct tm tm {};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	char buf[64];
	if (std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S %z", &tm) == 0) {
		return "";
	}
	return buf;
}

} // namespace

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

std::string MessageId(const Message &msg, const std::string &node) {
	if (msg.euid.find('@') != std::string::npos) {
		return "<" + msg.euid + ">";
	}
	char stamp[32];
	std::snprintf(stamp, sizeof stamp, "%08lX", (unsigned long)msg.msgtime);
	return "<" + std::string(stamp) + "-" + std::to_string(msg.msgnum) + "@" + node + ">";
}

std::string RenderRfc822(const Message &msg, const std::string &node) {
	// Format 4 messages are stored as the RFC822 bytes we received; serve them
	// unchanged so signatures/MIME structure survive round-tripping.
	if (msg.format_type == 4) {
		return msg.raw;
	}

	std::string out;
	auto hdr = [&out](const std::string &name, const std::string &value) {
		out += name + ": " + value + "\r\n";
	};

	if (!msg.node.empty()) {
		hdr("Return-Path", msg.node);
	}
	std::string date = Rfc822Date(msg.msgtime);
	if (!date.empty()) {
		hdr("Date", date);
	}
	hdr("Subject", msg.subject.empty() ? "(no subject)" : msg.subject);
	if (!msg.recipient.empty()) {
		hdr("To", msg.recipient.find('@') == std::string::npos ? msg.recipient + "@" + node
		                                                      : msg.recipient);
	}
	if (!msg.references.empty()) {
		// Citadel stores references pipe-delimited; RFC822 wants angle-bracketed.
		std::string refs;
		size_t start = 0;
		while (start <= msg.references.size()) {
			size_t bar = msg.references.find('|', start);
			std::string one = msg.references.substr(
			    start, bar == std::string::npos ? std::string::npos : bar - start);
			if (!one.empty()) {
				refs += (refs.empty() ? "" : " ");
				refs += one.front() == '<' ? one : "<" + one + ">";
			}
			if (bar == std::string::npos) {
				break;
			}
			start = bar + 1;
		}
		if (!refs.empty()) {
			hdr("References", refs);
		}
	}
	hdr("Message-ID", MessageId(msg, node));
	hdr("From", "\"" + msg.author + "\" <" + AddressLocalPart(msg.author) + "@" + node + ">");
	out += "\r\n";

	// Body, normalized to CRLF line endings.
	std::string body = BodyText(msg);
	std::istringstream in(body);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		out += line + "\r\n";
	}
	return out;
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
