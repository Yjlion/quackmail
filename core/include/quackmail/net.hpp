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

	bool IsTls() const {
		return ssl_ != nullptr;
	}
	int Fd() const {
		return fd_;
	}

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
