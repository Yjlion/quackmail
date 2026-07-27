#pragma once

#include "quackmail/tls.hpp"

#include <string>

#include <openssl/ssl.h>

namespace quackmail {
namespace net {

// A connected client socket, optionally wrapped in TLS. Provides buffered line
// reading (CRLF-terminated) suitable for text mail protocols, plus an in-band
// STARTTLS upgrade.
class ClientStream {
public:
	explicit ClientStream(int fd);
	~ClientStream();
	ClientStream(const ClientStream &) = delete;
	ClientStream &operator=(const ClientStream &) = delete;

	// Read a single CRLF/LF-terminated line (terminator stripped). Returns false
	// on EOF/error or if max_len is exceeded.
	bool ReadLine(std::string &line, size_t max_len = 4096);

	// Read a single byte. Returns false on EOF/error. Used by protocols that are
	// not line-oriented (telnet option negotiation, XML streams).
	bool ReadByte(char &c);

	// Read whatever bytes are already available (up to max_bytes), without
	// waiting for a line terminator. Returns false on EOF/error.
	bool ReadAvailable(std::string &out, size_t max_bytes = 8192);

	// Read exactly n bytes. Returns false (with out holding whatever arrived) on
	// EOF, error or timeout. This is what a Content-Length body needs: it cannot
	// be built out of ReadAvailable from outside, because that clears `out` on
	// every call and cannot see the internal buffer.
	bool ReadN(std::string &out, size_t n);

	// Wait until input is available or the timeout expires. Returns true when a
	// subsequent read will not block. Lets a protocol that must also push
	// unsolicited output (XMPP) wake up periodically.
	bool WaitReadable(int timeout_ms);

	// Read an SMTP DATA payload: bytes up to a line containing only ".".
	// Performs dot-unstuffing. Returns false on EOF/error or size overflow.
	bool ReadDotStuffed(std::string &out, size_t max_bytes);

	bool Write(const std::string &data);
	bool WriteLine(const std::string &line); // appends CRLF

	// Upgrade the plaintext connection to TLS (server side). Returns false + err.
	bool StartTls(SSL_CTX *ctx, std::string &err);
	// Perform the TLS handshake immediately (implicit-TLS listeners).
	bool AcceptTls(SSL_CTX *ctx, std::string &err);
	// Upgrade an outbound connection to TLS (client side, for the relay drainer).
	bool ConnectTls(SSL_CTX *ctx, std::string &err);

	// Apply SO_RCVTIMEO / SO_SNDTIMEO, so a peer that opens a socket and then
	// stops talking cannot hold a connection thread forever. Opt-in and off by
	// default: the mail and XMPP handlers deliberately sit idle for minutes, and
	// only HTTP (where every connection is one short request) wants this.
	// 0 disables the corresponding timeout. Returns false if setsockopt failed.
	bool SetTimeouts(int read_ms, int write_ms);

	bool IsTls() const {
		return ssl_ != nullptr;
	}
	int Fd() const {
		return fd_;
	}

	// The connected client's address in presentation form ("192.0.2.1",
	// "2001:db8::1"), or "" if it cannot be determined. ConnHandler only ever
	// receives the stream, so this is how SPF and DNSBL checks reach the peer
	// address. IPv4-mapped IPv6 addresses are unwrapped to plain IPv4.
	std::string PeerIp() const;

private:
	ssize_t RawRead(char *buf, size_t n);
	bool RawWrite(const char *buf, size_t n);
	bool FillBuffer();

	int fd_;
	SSL *ssl_ = nullptr;
	std::string rbuf_;
	size_t rpos_ = 0;
};

} // namespace net
} // namespace quackmail
