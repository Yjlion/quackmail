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

	// The same shell carried over SSH instead (telnet/src/ssh_*.cpp), in place
	// of Negotiate. Everything telnet negotiates arrives in the pty-req instead:
	// the terminal type, the window size, and the fact that the server echoes.
	// The SSH side still speaks telnet framing *into* this session — it doubles
	// a 0xFF from the client and reports window-change as an IAC SB NAWS — so
	// the input parser needs no second mode; only output differs, because there
	// is no telnet peer on the far side to undo an IAC escape.
	void BeginSsh(const std::string &term, int width, int height);
	bool IsSsh() const {
		return ssh_;
	}

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

	// ---- prompting ------------------------------------------------------
	// Every call site used to hand-roll these out of Write + ReadLine.

	// Show a prompt and read a reply. Returns false on EOF. An empty reply
	// yields `dflt`.
	bool Prompt(const std::string &question, std::string &out, const std::string &dflt = "");
	// A yes/no question; `dflt` is used for an empty reply.
	bool YesNo(const std::string &question, bool dflt, bool &out);
	bool PromptInt(const std::string &question, int64_t dflt, int64_t &out);

	// ---- paging ---------------------------------------------------------
	// A telnet BBS pauses at the end of each screen. Without this every listing
	// scrolls off the top of the window in one burst.

	// Rows per screen. 0 disables paging (the default, so a raw socket or a
	// test harness gets an uninterrupted stream).
	void SetPageSize(int rows);
	int PageSize() const {
		return page_rows_;
	}
	// Reset the line counter — call when a new screen of output starts.
	void ResetPager() {
		page_used_ = 0;
	}
	// Write text through the pager, pausing at each screenful. Returns false if
	// the reader asked to stop (S, Q or Ctrl-C) or the connection dropped.
	bool Page(const std::string &text);

	// ---- terminal geometry ----------------------------------------------
	// Filled in from the NAWS subnegotiation when the client sends one;
	// otherwise these keep whatever SetSize was last given.
	void SetSize(int width, int height);
	int Width() const {
		return width_;
	}
	int Height() const {
		return height_;
	}
	// True once the client has actually reported a window size.
	bool HaveNaws() const {
		return have_naws_;
	}

	bool Echoing() const {
		return echo_;
	}

	// ---- colour ---------------------------------------------------------
	// The BBS shell asks for colour when the user's US_COLOR bit is set, but a
	// terminal that cannot render it must never see an escape sequence — so a
	// client that identifies itself as "dumb" or "unknown" is refused even then.
	// A client that never answers TERMINAL-TYPE is assumed capable, the same
	// benefit of the doubt the NAWS fallback gives.
	void SetColor(bool on) {
		color_ = on;
	}
	bool ColorEnabled() const {
		return color_ && !dumb_terminal_;
	}
	const std::string &TermType() const {
		return term_type_;
	}
	// The escape for one palette entry, or "" when colour is off, so call sites
	// never have to branch. `Attr::Reset` returns to the terminal default.
	enum class Attr {
		Reset,
		Banner,   // room banners, section headings
		Prompt,   // the room prompt and menu keys
		Header,   // message From/Subject lines
		Body,     // message text
		Notice,   // instant messages, system notices
		Error,    // refusals and warnings
		Dim,      // secondary detail
	};
	std::string Colour(Attr attr) const;

	// Printable width, ignoring CSI escape sequences. The pager needs this or a
	// coloured line counts its escapes as visible columns and wraps early.
	static size_t VisibleWidth(const std::string &text);

	// ---- raw byte I/O ----------------------------------------------------
	// For a binary file transfer (Xmodem), which is bytes rather than text: no
	// CRLF translation on the way out, no line discipline on the way in.
	//
	// Note what WriteRaw still does: it doubles 0xFF. A telnet stream reserves
	// that byte as IAC, so a payload byte of 0xFF has to be escaped or the peer
	// reads it as the start of a command — which is exactly how a binary
	// transfer over telnet corrupts every file containing one.
	void WriteRaw(const std::string &bytes);

	// One byte of payload, IAC sequences consumed and acted on as usual. Returns
	// false on EOF or if `timeout_ms` passes with nothing arriving — a transfer
	// protocol has to be able to time out and retry rather than block forever.
	bool ReadRawByte(unsigned char &out, int timeout_ms);

	// Discard whatever has already arrived. Xmodem needs this between retries:
	// a NAK sent after a bad block must not be answered with the tail of the
	// block that was already in flight.
	void DrainInput(int settle_ms);

private:
	void Send3(unsigned char verb, unsigned char option);
	// Next raw byte, honouring the pushback slot. False on EOF.
	bool NextByte(unsigned char &u);
	// Next byte of payload: IAC negotiation consumed and answered, no line
	// discipline. -1 on EOF. GetChar is this plus CR/LF handling.
	int NextPayloadByte();
	// Consume a subnegotiation payload (IAC SB ... IAC SE), acting on NAWS.
	void ReadSubnegotiation();
	// The "<more>" prompt between screens. False if the reader asked to stop.
	bool MorePrompt();

	net::ClientStream &stream_;
	bool echo_ = false;
	bool pending_cr_ = false; // a CR was consumed; swallow a following LF/NUL
	int pushback_ = -1;       // one byte of lookahead (CR CR LF disambiguation)
	int width_ = 80;
	int height_ = 24;
	bool have_naws_ = false;
	int page_rows_ = 0; // 0 = no paging
	int page_used_ = 0;
	bool color_ = false;
	bool dumb_terminal_ = false;
	std::string term_type_;
	bool ssh_ = false;
};

} // namespace telnet
} // namespace quackmail
