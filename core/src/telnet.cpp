#include "quackmail/telnet.hpp"

#include <cctype>
#include <cstdlib>
#include <string>

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
constexpr unsigned char OPT_SGA = 3;    // suppress go-ahead
constexpr unsigned char OPT_TTYPE = 24; // terminal type (RFC 1091)
constexpr unsigned char OPT_NAWS = 31;  // window size

constexpr unsigned char TTYPE_IS = 0;
constexpr unsigned char TTYPE_SEND = 1;

// The palette, in the spirit of the official text client's commands.c: bright
// cyan for room banners, bright white for prompts, yellow for headers, bright
// red for errors. Values are SGR sequences, emitted only when colour is on.
constexpr const char *kReset = "\033[0m";
constexpr const char *kBanner = "\033[1;36m";
constexpr const char *kPrompt = "\033[1;37m";
constexpr const char *kHeader = "\033[1;33m";
constexpr const char *kBody = "\033[0;37m";
constexpr const char *kNotice = "\033[1;35m";
constexpr const char *kError = "\033[1;31m";
constexpr const char *kDim = "\033[0;36m";

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
	// Ask what kind of terminal this is, so colour is never sent to one that
	// cannot render it. A client that ignores this is treated as capable.
	Send3(DO, OPT_TTYPE);
}

std::string Session::Colour(Attr attr) const {
	if (!ColorEnabled()) {
		return "";
	}
	switch (attr) {
	case Attr::Banner:
		return kBanner;
	case Attr::Prompt:
		return kPrompt;
	case Attr::Header:
		return kHeader;
	case Attr::Body:
		return kBody;
	case Attr::Notice:
		return kNotice;
	case Attr::Error:
		return kError;
	case Attr::Dim:
		return kDim;
	case Attr::Reset:
		break;
	}
	return kReset;
}

size_t Session::VisibleWidth(const std::string &text) {
	size_t width = 0;
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] != '\033') {
			width++;
			continue;
		}
		// Skip a CSI sequence: ESC [ ... <final byte 0x40-0x7E>.
		if (i + 1 < text.size() && text[i + 1] == '[') {
			i += 2;
			while (i < text.size() && (text[i] < '@' || text[i] > '~')) {
				i++;
			}
		} else {
			i++; // a two-character escape
		}
	}
	return width;
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

// IAC SB <option> <payload...> IAC SE. Only NAWS carries anything we want:
// two 16-bit big-endian values, width then height. Everything else is drained
// and discarded.
void Session::ReadSubnegotiation() {
	std::string payload;
	unsigned char p, q;
	bool first = true;
	unsigned char option = 0;
	while (NextByte(p)) {
		if (p == IAC) {
			if (!NextByte(q)) {
				return;
			}
			if (q == SE) {
				break;
			}
			if (q == IAC) {
				payload.push_back((char)IAC); // escaped 0xFF inside the payload
				continue;
			}
			continue; // an unexpected command inside SB; ignore it
		}
		if (first) {
			option = p;
			first = false;
			continue;
		}
		payload.push_back((char)p);
		if (payload.size() > 64) {
			payload.resize(64); // nothing we parse is longer than this
		}
	}
	if (option == OPT_TTYPE && !payload.empty() && (unsigned char)payload[0] == TTYPE_IS) {
		term_type_.assign(payload, 1, std::string::npos);
		std::string lower;
		for (char c : term_type_) {
			lower += (char)std::tolower((unsigned char)c);
		}
		dumb_terminal_ = lower.empty() || lower == "dumb" || lower == "unknown" || lower == "net";
	}
	if (option == OPT_NAWS && payload.size() >= 4) {
		int w = ((unsigned char)payload[0] << 8) | (unsigned char)payload[1];
		int h = ((unsigned char)payload[2] << 8) | (unsigned char)payload[3];
		if (w > 0 && h > 0) {
			SetSize(w, h);
			have_naws_ = true;
		}
	}
}

void Session::SetSize(int width, int height) {
	// Clamp to something a terminal could plausibly be, so a bogus or hostile
	// NAWS cannot make the pager divide by a silly number.
	width_ = width < 20 ? 20 : (width > 1000 ? 1000 : width);
	height_ = height < 5 ? 5 : (height > 500 ? 500 : height);
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
				ReadSubnegotiation();
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
					// Let the peer do SGA/NAWS/TTYPE; decline anything else.
					if (opt == OPT_TTYPE) {
						// It agreed to describe itself: now ask it to.
						// IAC SB TERMINAL-TYPE SEND IAC SE.
						char req[6] = {(char)IAC,       (char)SB, (char)OPT_TTYPE,
						               (char)TTYPE_SEND, (char)IAC, (char)SE};
						stream_.Write(std::string(req, 6));
					} else if (opt != OPT_SGA && opt != OPT_NAWS) {
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

// ---- prompting -------------------------------------------------------------

bool Session::Prompt(const std::string &question, std::string &out, const std::string &dflt) {
	Write(question);
	if (!ReadLine(out)) {
		return false;
	}
	if (out.empty()) {
		out = dflt;
	}
	return true;
}

bool Session::YesNo(const std::string &question, bool dflt, bool &out) {
	std::string reply;
	if (!Prompt(question + (dflt ? " [Yes]: " : " [No]: "), reply)) {
		return false;
	}
	if (reply.empty()) {
		out = dflt;
	} else {
		out = reply[0] == 'y' || reply[0] == 'Y';
	}
	return true;
}

bool Session::PromptInt(const std::string &question, int64_t dflt, int64_t &out) {
	std::string reply;
	if (!Prompt(question + " [" + std::to_string(dflt) + "]: ", reply)) {
		return false;
	}
	if (reply.empty()) {
		out = dflt;
		return true;
	}
	char *end = nullptr;
	long long v = std::strtoll(reply.c_str(), &end, 10);
	out = (end && *end == '\0') ? (int64_t)v : dflt;
	return true;
}

// ---- paging ----------------------------------------------------------------

void Session::SetPageSize(int rows) {
	page_rows_ = rows > 2 ? rows : 0;
	page_used_ = 0;
}

bool Session::MorePrompt() {
	Write("<more> ");
	int ch = GetChar();
	// Erase the prompt so the paused screen does not keep it.
	Write("\r          \r");
	if (ch < 0) {
		return false;
	}
	// Citadel's convention: S(top) or Q(uit) abandons the listing, anything
	// else continues.
	return !(ch == 's' || ch == 'S' || ch == 'q' || ch == 'Q' || ch == 3);
}

bool Session::Page(const std::string &text) {
	if (page_rows_ <= 0) {
		Write(text);
		return true;
	}
	// Count the wrapped height of each line, not just the newlines, or a long
	// paragraph scrolls straight past the pause.
	size_t pos = 0;
	while (pos <= text.size()) {
		size_t nl = text.find('\n', pos);
		std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
		Write(line);
		if (nl == std::string::npos) {
			return true; // no trailing newline: leave the cursor where it is
		}
		Write("\n");
		int rows = (int)(VisibleWidth(line) / (size_t)width_) + 1;
		page_used_ += rows;
		if (page_used_ >= page_rows_ - 1) {
			page_used_ = 0;
			if (!MorePrompt()) {
				return false;
			}
		}
		pos = nl + 1;
	}
	return true;
}

} // namespace telnet
} // namespace quackmail
