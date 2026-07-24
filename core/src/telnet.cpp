#include "quackmail/telnet.hpp"

namespace quackmail {
namespace telnet {
namespace {

// RFC 854 commands and the handful of options we care about.
constexpr unsigned char IAC = 255;
constexpr unsigned char DONT = 254;
constexpr unsigned char DO = 253;
constexpr unsigned char WONT = 252;
constexpr unsigned char WILL = 251;
constexpr unsigned char SB = 250;
constexpr unsigned char SE = 240;

constexpr unsigned char OPT_ECHO = 1;
constexpr unsigned char OPT_SGA = 3;   // suppress go-ahead
constexpr unsigned char OPT_NAWS = 31; // window size

} // namespace

void Session::Send3(unsigned char verb, unsigned char option) {
	char msg[3] = {(char)IAC, (char)verb, (char)option};
	stream_.Write(std::string(msg, 3));
}

void Session::Negotiate() {
	// Character-at-a-time with server-side echo, the classic BBS setup.
	Send3(WILL, OPT_ECHO);
	Send3(WILL, OPT_SGA);
	Send3(DO, OPT_SGA);
	Send3(DO, OPT_NAWS);
}

int Session::GetChar() {
	char c;
	while (stream_.ReadByte(c)) {
		unsigned char u = (unsigned char)c;

		if (u == IAC) {
			char v;
			if (!stream_.ReadByte(v)) {
				return -1;
			}
			unsigned char verb = (unsigned char)v;
			if (verb == IAC) {
				return IAC; // escaped 0xFF is literal data
			}
			if (verb == SB) {
				// Skip the subnegotiation payload up to IAC SE.
				char p, q;
				while (stream_.ReadByte(p)) {
					if ((unsigned char)p == IAC && stream_.ReadByte(q) &&
					    (unsigned char)q == SE) {
						break;
					}
				}
				continue;
			}
			if (verb == WILL || verb == WONT || verb == DO || verb == DONT) {
				char o;
				if (!stream_.ReadByte(o)) {
					return -1;
				}
				unsigned char opt = (unsigned char)o;
				if (verb == DO) {
					// The peer wants us to do something. We only ever offer
					// ECHO and SGA; refuse everything else.
					if (opt == OPT_ECHO) {
						echo_ = true; // it accepted our echo offer
					} else if (opt != OPT_SGA) {
						Send3(WONT, opt);
					}
				} else if (verb == DONT) {
					if (opt == OPT_ECHO) {
						echo_ = false;
					}
					Send3(WONT, opt);
				} else if (verb == WILL) {
					// Let the peer do SGA/NAWS; decline anything else.
					if (opt != OPT_SGA && opt != OPT_NAWS) {
						Send3(DONT, opt);
					}
				}
				continue;
			}
			continue; // other two-byte commands (NOP, AYT, ...) are ignored
		}

		// CR LF / CR NUL both mean "end of line": report the CR, swallow the
		// partner byte on the next call.
		if (pending_cr_) {
			pending_cr_ = false;
			if (u == '\n' || u == 0) {
				continue;
			}
		}
		if (u == '\r') {
			pending_cr_ = true;
			return '\r';
		}
		return u;
	}
	return -1;
}

bool Session::ReadLine(std::string &out, bool mask) {
	out.clear();
	while (true) {
		int ch = GetChar();
		if (ch < 0) {
			return false;
		}
		if (ch == '\r' || ch == '\n') {
			if (echo_) {
				stream_.Write("\r\n");
			}
			return true;
		}
		if (ch == 8 || ch == 127) { // backspace / DEL
			if (!out.empty()) {
				out.pop_back();
				if (echo_) {
					stream_.Write("\b \b");
				}
			}
			continue;
		}
		if (ch < 32) {
			continue; // ignore other control characters
		}
		out.push_back((char)ch);
		if (echo_) {
			stream_.Write(mask ? "*" : std::string(1, (char)ch));
		}
	}
}

void Session::Write(const std::string &text) {
	std::string out;
	out.reserve(text.size() + 8);
	for (char c : text) {
		if (c == '\n') {
			out += "\r\n";
		} else {
			out.push_back(c);
		}
	}
	stream_.Write(out);
}

} // namespace telnet
} // namespace quackmail
