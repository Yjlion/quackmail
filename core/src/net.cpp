#include "quackmail/net.hpp"

#include <cstring>
#include <unistd.h>

#include <sys/socket.h>

#include <openssl/err.h>

namespace quackmail {
namespace net {

static constexpr size_t kChunk = 8192;

ClientStream::ClientStream(int fd) : fd_(fd) {
}

ClientStream::~ClientStream() {
	if (ssl_) {
		SSL_shutdown(ssl_);
		SSL_free(ssl_);
		ssl_ = nullptr;
	}
	if (fd_ >= 0) {
		::close(fd_);
		fd_ = -1;
	}
}

ssize_t ClientStream::RawRead(char *buf, size_t n) {
	if (ssl_) {
		int r = SSL_read(ssl_, buf, (int)n);
		return r > 0 ? r : (r == 0 ? 0 : -1);
	}
	return ::recv(fd_, buf, n, 0);
}

bool ClientStream::RawWrite(const char *buf, size_t n) {
	size_t off = 0;
	while (off < n) {
		ssize_t w;
		if (ssl_) {
			w = SSL_write(ssl_, buf + off, (int)(n - off));
		} else {
			w = ::send(fd_, buf + off, n - off, 0);
		}
		if (w <= 0) {
			return false;
		}
		off += (size_t)w;
	}
	return true;
}

bool ClientStream::FillBuffer() {
	// Drop already-consumed bytes to keep the buffer bounded.
	if (rpos_ > 0) {
		rbuf_.erase(0, rpos_);
		rpos_ = 0;
	}
	char tmp[kChunk];
	ssize_t r = RawRead(tmp, sizeof(tmp));
	if (r <= 0) {
		return false;
	}
	rbuf_.append(tmp, (size_t)r);
	return true;
}

bool ClientStream::ReadLine(std::string &line, size_t max_len) {
	line.clear();
	while (true) {
		size_t nl = rbuf_.find('\n', rpos_);
		if (nl != std::string::npos) {
			size_t len = nl - rpos_;
			line = rbuf_.substr(rpos_, len);
			rpos_ = nl + 1;
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			return true;
		}
		if (rbuf_.size() - rpos_ > max_len) {
			return false; // line too long
		}
		if (!FillBuffer()) {
			return false; // EOF/error before newline
		}
	}
}

bool ClientStream::ReadDotStuffed(std::string &out, size_t max_bytes) {
	out.clear();
	std::string line;
	while (ReadLine(line, max_bytes)) {
		if (line == ".") {
			return true;
		}
		if (!line.empty() && line[0] == '.') {
			line.erase(0, 1); // undo dot-stuffing
		}
		if (out.size() + line.size() + 2 > max_bytes) {
			return false; // message too large
		}
		out += line;
		out += "\r\n";
	}
	return false; // EOF before terminating "."
}

bool ClientStream::Write(const std::string &data) {
	return RawWrite(data.data(), data.size());
}

bool ClientStream::WriteLine(const std::string &line) {
	std::string out = line;
	out += "\r\n";
	return RawWrite(out.data(), out.size());
}

bool ClientStream::AcceptTls(SSL_CTX *ctx, std::string &err) {
	if (!ctx) {
		err = "no TLS context";
		return false;
	}
	ssl_ = SSL_new(ctx);
	if (!ssl_) {
		err = "SSL_new failed";
		return false;
	}
	SSL_set_fd(ssl_, fd_);
	// Any bytes buffered before the handshake would break TLS; the callers only
	// upgrade at protocol points where the buffer is empty.
	int r = SSL_accept(ssl_);
	if (r != 1) {
		char buf[256];
		ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
		err = std::string("TLS handshake failed: ") + buf;
		SSL_free(ssl_);
		ssl_ = nullptr;
		return false;
	}
	return true;
}

bool ClientStream::StartTls(SSL_CTX *ctx, std::string &err) {
	return AcceptTls(ctx, err);
}

bool ClientStream::ConnectTls(SSL_CTX *ctx, std::string &err) {
	if (!ctx) {
		err = "no TLS context";
		return false;
	}
	ssl_ = SSL_new(ctx);
	if (!ssl_) {
		err = "SSL_new failed";
		return false;
	}
	SSL_set_fd(ssl_, fd_);
	int r = SSL_connect(ssl_);
	if (r != 1) {
		char buf[256];
		ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
		err = std::string("TLS connect failed: ") + buf;
		SSL_free(ssl_);
		ssl_ = nullptr;
		return false;
	}
	return true;
}

} // namespace net
} // namespace quackmail
