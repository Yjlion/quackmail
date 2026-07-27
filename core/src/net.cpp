#include "quackmail/net.hpp"

#include <algorithm>
#include <cstring>
#include <poll.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

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

bool ClientStream::ReadByte(char &c) {
	while (rpos_ >= rbuf_.size()) {
		if (!FillBuffer()) {
			return false;
		}
	}
	c = rbuf_[rpos_++];
	return true;
}

bool ClientStream::ReadAvailable(std::string &out, size_t max_bytes) {
	out.clear();
	if (rpos_ >= rbuf_.size() && !FillBuffer()) {
		return false;
	}
	size_t n = std::min(max_bytes, rbuf_.size() - rpos_);
	out.assign(rbuf_, rpos_, n);
	rpos_ += n;
	return true;
}

bool ClientStream::ReadN(std::string &out, size_t n) {
	out.clear();
	out.reserve(n);
	while (out.size() < n) {
		if (rpos_ >= rbuf_.size() && !FillBuffer()) {
			return false;
		}
		size_t take = std::min(n - out.size(), rbuf_.size() - rpos_);
		out.append(rbuf_, rpos_, take);
		rpos_ += take;
	}
	return true;
}

bool ClientStream::SetTimeouts(int read_ms, int write_ms) {
	if (fd_ < 0) {
		return false;
	}
	auto apply = [this](int optname, int ms) {
		struct timeval tv {};
		tv.tv_sec = ms / 1000;
		tv.tv_usec = (ms % 1000) * 1000;
		return ::setsockopt(fd_, SOL_SOCKET, optname, &tv, sizeof(tv)) == 0;
	};
	bool ok = true;
	if (read_ms >= 0) {
		ok = apply(SO_RCVTIMEO, read_ms) && ok;
	}
	if (write_ms >= 0) {
		ok = apply(SO_SNDTIMEO, write_ms) && ok;
	}
	// A timeout makes recv() fail with EAGAIN, and SSL_read() return <= 0.
	// RawRead turns both into -1, which every caller already treats as EOF —
	// so a stalled peer simply loses its connection.
	return ok;
}

bool ClientStream::WaitReadable(int timeout_ms) {
	// Bytes already buffered here, or inside OpenSSL's record buffer, would not
	// show up in poll().
	if (rpos_ < rbuf_.size()) {
		return true;
	}
	if (ssl_ && SSL_pending(ssl_) > 0) {
		return true;
	}
	struct pollfd pfd {};
	pfd.fd = fd_;
	pfd.events = POLLIN;
	int r = ::poll(&pfd, 1, timeout_ms);
	return r > 0;
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

std::string ClientStream::PeerIp() const {
	if (fd_ < 0) {
		return "";
	}
	struct sockaddr_storage ss {};
	socklen_t slen = sizeof(ss);
	if (::getpeername(fd_, reinterpret_cast<struct sockaddr *>(&ss), &slen) != 0) {
		return "";
	}
	char buf[INET6_ADDRSTRLEN] = {0};
	if (ss.ss_family == AF_INET) {
		auto *a = reinterpret_cast<struct sockaddr_in *>(&ss);
		if (!inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf))) {
			return "";
		}
		return buf;
	}
	if (ss.ss_family == AF_INET6) {
		auto *a = reinterpret_cast<struct sockaddr_in6 *>(&ss);
		// A dual-stack listener reports IPv4 peers as ::ffff:1.2.3.4; SPF and
		// DNSBL both want the bare IPv4 form.
		if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) {
			struct in_addr v4 {};
			std::memcpy(&v4, a->sin6_addr.s6_addr + 12, sizeof(v4));
			if (!inet_ntop(AF_INET, &v4, buf, sizeof(buf))) {
				return "";
			}
			return buf;
		}
		if (!inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf))) {
			return "";
		}
		return buf;
	}
	return "";
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
