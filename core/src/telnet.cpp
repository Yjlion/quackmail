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

bool Session::NextByte(unsigned char &u) {
	if (pushback_ >= 0) {
		u = (unsigned char)pushback_;
		pushback_ = -1;
		return true;
	}
	char c;
	if (!stream_.ReadByte(c)) {
		return false;
	}
	u = (unsigned char)c;
	return true;
}

int Session::GetChar() {
	unsigned char u;
	while (NextByte(u)) {

		if (u == IAC) {
			unsigned char verb;
			if (!NextByte(verb)) {
				return -1;
			}
			if (verb == IAC) {
				return IAC; // escaped 0xFF is literal data
			}
			if (verb == SB) {
				// Skip the subnegotiation payload up to IAC SE.
				unsigned char p, q;
				while (NextByte(p)) {
					if (p == IAC && NextByte(q) && q == SE) {
						break;
					}
				}
				continue;
			}
			if (verb == WILL || verb == WONT || verb == DO || verb == DONT) {
				unsigned char opt;
				if (!NextByte(opt)) {
					return -1;
				}
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

		// CR LF and CR NUL both mean "end of line": report the CR and swallow the
		// partner byte here. Piping a CRLF file into a telnet client puts CR CR LF
		// on the wire, which must still count as one line ending — but a genuine
		// blank line (Enter pressed twice) must not be swallowed, so peek one byte
		// to tell the two apart.
		if (pending_cr_) {
			pending_cr_ = false;
			if (u == '\n' || u == 0) {
				continue;
			}
			if (u == '\r') {
				unsigned char next;
				if (!NextByte(next)) {
					return -1;
				}
				if (next == '\n' || next == 0) {
					continue; // CR CR LF -> a single terminator
				}
				pushback_ = next;
				pending_cr_ = true;
				return '\r'; // a real second line ending
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
