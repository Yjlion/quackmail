#pragma once

#include "quackmail/net.hpp"

#include <string>

namespace quackmail {
namespace telnet {

// A telnet-aware view over a ClientStream: filters IAC option negotiation out of
// the input, answers it politely, and provides the character- and line-oriented
// input a BBS shell needs.
//
// Two kinds of client have to work:
//   * a real telnet client, which negotiates and then sends one keystroke at a
//     time — the server echoes (we offer WILL ECHO);
//   * a raw socket (nc, expect harnesses, our tests), which never negotiates and
//     sends whole lines — the far end echoes locally, so we must not.
// We therefore echo only after the peer has agreed to our ECHO offer.
class Session {
public:
	explicit Session(net::ClientStream &stream) : stream_(stream) {
	}

	// Offer WILL ECHO + WILL SUPPRESS-GO-AHEAD (character-at-a-time mode).
	void Negotiate();

	// Next input character with IAC sequences removed. Returns -1 on EOF.
	int GetChar();

	// Read one line (CR LF, CR NUL, LF, or bare CR all terminate). Handles
	// backspace/DEL. Returns false on EOF. Characters are echoed when the peer
	// asked us to echo; `mask` echoes '*' instead (password entry).
	bool ReadLine(std::string &out, bool mask = false);

	// Write text, translating bare '\n' into CRLF.
	void Write(const std::string &text);
	void WriteLine(const std::string &text) {
		Write(text + "\n");
	}

	bool Echoing() const {
		return echo_;
	}

private:
	void Send3(unsigned char verb, unsigned char option);
	// Next raw byte, honouring the pushback slot. False on EOF.
	bool NextByte(unsigned char &u);

	net::ClientStream &stream_;
	bool echo_ = false;
	bool pending_cr_ = false; // a CR was consumed; swallow a following LF/NUL
	int pushback_ = -1;       // one byte of lookahead (CR CR LF disambiguation)
};

} // namespace telnet
} // namespace quackmail
