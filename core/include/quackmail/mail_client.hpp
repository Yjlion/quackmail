#pragma once

#include <functional>
#include <string>
#include <vector>

namespace quackmail {
namespace mailclient {

// POP3 and IMAP *clients*, for pulling mail off somebody else's server.
//
// The tree already speaks both protocols as a server; this is the other side of
// the same wire, and the only thing in QuackCit that reaches out to a mailbox
// rather than serving one. Both are deliberately minimal: fetch what is new,
// hand each message to a callback, optionally delete it upstream. No flags, no
// folders beyond the one named, no partial fetches — anything more belongs in
// the IMAP server, which already has it.

enum class TlsMode {
	None,     // plaintext
	StartTls, // upgrade in band (POP3 STLS / IMAP STARTTLS)
	Implicit, // TLS from the first byte (995 / 993)
};

struct Account {
	std::string host;
	int port = 0;
	TlsMode tls = TlsMode::StartTls;
	std::string username;
	std::string password;
	std::string mailbox = "INBOX"; // IMAP only
	int timeout_ms = 30000;
};

// One message pulled off the far end.
struct Fetched {
	std::string uid; // POP3 UIDL, or "<uidvalidity>.<uid>" for IMAP
	std::string raw; // the RFC822 bytes
};

// Decides whether a message is worth downloading, by uid. Returning false skips
// it — which for POP3 is the difference between one RETR and none, and is how
// the same mailbox can be polled every minute without re-downloading it all.
using WantFn = std::function<bool(const std::string &uid)>;

struct Result {
	bool ok = false;
	std::string error;
	int64_t seen = 0;      // messages the server offered
	int64_t fetched = 0;   // messages actually downloaded
	int64_t deleted = 0;   // messages removed upstream
	// IMAP: what the mailbox reported, so the caller can store a resume point
	// and notice a UIDVALIDITY change (which invalidates every stored uid).
	int64_t uidvalidity = 0;
	int64_t highest_uid = 0;
	std::string info; // a human-readable note, for the feed's status column
};

// Pull from a POP3 server. `want` filters by UIDL; `on_message` receives each
// downloaded message. With `del`, a fetched message is DELEted before QUIT —
// which is what commits the deletion, so an error anywhere before that leaves
// the mailbox untouched.
Result FetchPop3(const Account &acct, int max_messages, size_t max_bytes, bool del, const WantFn &want,
                 const std::function<bool(const Fetched &)> &on_message);

// Pull from an IMAP server. `since_uid` resumes: only UIDs above it are
// considered, which is what keeps a large mailbox cheap to poll. Pass 0 (and a
// mismatched `uidvalidity`) to start over.
Result FetchImap(const Account &acct, int64_t uidvalidity, int64_t since_uid, int max_messages,
                 size_t max_bytes, bool del, const WantFn &want,
                 const std::function<bool(const Fetched &)> &on_message);

// Connect and authenticate only, then disconnect. What the admin console's
// "test" button calls: it answers "are these credentials right" without
// touching the mailbox.
Result TestPop3(const Account &acct);
Result TestImap(const Account &acct);

} // namespace mailclient
} // namespace quackmail
