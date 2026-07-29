#include "quackmail/smtp_client.hpp"

#include "quackmail/net.hpp"
#include "quackmail/tls.hpp"
#include "quackmail/util.hpp"

#include <cstdlib>
#include <cstring>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace quackmail {
namespace smtp {

using net::ClientStream;

namespace {

// A peer that will not complete a TCP handshake should cost the relay drainer
// half a minute, not the kernel's full SYN retry schedule.
constexpr int kConnectTimeoutMs = 30000;

// Read one SMTP reply, following multi-line continuations ("250-line" ... "250 line").
// Returns the 3-digit code (or -1 on I/O error); appends the text to `text`.
int ReadReply(ClientStream &s, std::string &text) {
	std::string line;
	int code = -1;
	while (s.ReadLine(line, 8192)) {
		if (!text.empty()) {
			text += "\n";
		}
		text += line;
		if (line.size() < 3) {
			return -1;
		}
		code = std::atoi(line.substr(0, 3).c_str());
		if (line.size() >= 4 && line[3] == '-') {
			continue; // continuation line
		}
		return code;
	}
	return -1;
}

SendStatus Classify(int code) {
	if (code >= 200 && code < 400) {
		return SendStatus::Sent;
	}
	if (code >= 400 && code < 500) {
		return SendStatus::Transient;
	}
	return SendStatus::Permanent;
}

// CRLF-normalize and dot-stuff a message body for the DATA phase.
std::string DotStuff(const std::string &raw) {
	std::string out;
	out.reserve(raw.size() + 16);
	size_t i = 0;
	while (i < raw.size()) {
		size_t nl = raw.find('\n', i);
		std::string ln = (nl == std::string::npos) ? raw.substr(i) : raw.substr(i, nl - i);
		if (!ln.empty() && ln.back() == '\r') {
			ln.pop_back();
		}
		if (!ln.empty() && ln[0] == '.') {
			out += '.'; // dot-stuffing
		}
		out += ln;
		out += "\r\n";
		if (nl == std::string::npos) {
			break;
		}
		i = nl + 1;
	}
	return out;
}

} // namespace

SendResult Deliver(const std::string &host, int port, const std::string &mail_from, const std::string &rcpt,
                   const std::string &raw, const ClientOpts &opts) {
	SendResult result;
	std::string err;
	int fd = net::Connect(host, port, kConnectTimeoutMs, err);
	if (fd < 0) {
		result.status = SendStatus::Transient;
		result.info = err;
		return result;
	}
	ClientStream s(fd);

	std::string reply;
	int code = ReadReply(s, reply); // greeting
	if (code < 0 || Classify(code) != SendStatus::Sent) {
		result.status = code < 0 ? SendStatus::Transient : Classify(code);
		result.info = "greeting: " + reply;
		return result;
	}

	auto cmd = [&](const std::string &c, int &rc) -> bool {
		if (!s.WriteLine(c)) {
			rc = -1;
			return false;
		}
		reply.clear();
		rc = ReadReply(s, reply);
		return rc >= 0;
	};

	if (!cmd("EHLO " + opts.helo_name, code) || Classify(code) != SendStatus::Sent) {
		result.status = code < 0 ? SendStatus::Transient : Classify(code);
		result.info = "EHLO: " + reply;
		return result;
	}
	bool offers_starttls = reply.find("STARTTLS") != std::string::npos;

	if (opts.use_starttls && offers_starttls && !s.IsTls()) {
		if (cmd("STARTTLS", code) && Classify(code) == SendStatus::Sent) {
			tls::ClientContext cctx;
			std::string terr;
			if (cctx.Init(terr) && s.ConnectTls(cctx.Get(), terr)) {
				// Re-EHLO over the encrypted channel.
				if (!cmd("EHLO " + opts.helo_name, code) || Classify(code) != SendStatus::Sent) {
					result.status = SendStatus::Transient;
					result.info = "EHLO(TLS): " + reply;
					return result;
				}
			}
		}
	}

	if (!opts.auth_user.empty()) {
		std::string plain;
		plain.push_back('\0'); // authzid empty
		plain += opts.auth_user;
		plain.push_back('\0');
		plain += opts.auth_pass;
		if (!cmd("AUTH PLAIN " + util::Base64Encode(plain), code) || Classify(code) != SendStatus::Sent) {
			result.status = code < 0 ? SendStatus::Transient : Classify(code);
			result.info = "AUTH: " + reply;
			return result;
		}
	}

	if (!cmd("MAIL FROM:<" + mail_from + ">", code) || Classify(code) != SendStatus::Sent) {
		result.status = code < 0 ? SendStatus::Transient : Classify(code);
		result.info = "MAIL: " + reply;
		return result;
	}
	if (!cmd("RCPT TO:<" + rcpt + ">", code) || Classify(code) != SendStatus::Sent) {
		result.status = code < 0 ? SendStatus::Transient : Classify(code);
		result.info = "RCPT: " + reply;
		return result;
	}
	if (!cmd("DATA", code) || code != 354) {
		result.status = code < 0 ? SendStatus::Transient : Classify(code);
		result.info = "DATA: " + reply;
		return result;
	}

	if (!s.Write(DotStuff(raw)) || !s.Write(".\r\n")) {
		result.status = SendStatus::Transient;
		result.info = "write body failed";
		return result;
	}
	reply.clear();
	code = ReadReply(s, reply);
	if (code < 0) {
		result.status = SendStatus::Transient;
		result.info = "no reply after data";
		return result;
	}
	result.status = Classify(code);
	result.info = reply;
	s.WriteLine("QUIT");
	return result;
}

} // namespace smtp
} // namespace quackmail
