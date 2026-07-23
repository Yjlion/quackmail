#pragma once

#include <memory>
#include <string>

#include <openssl/ssl.h>

namespace quackmail {
namespace tls {

struct TlsConfig {
	bool implicit = false;  // whole connection is TLS from the first byte (e.g. SMTPS/IMAPS)
	bool starttls = false;  // advertise + honour in-band STARTTLS upgrade
	std::string cert_path;  // PEM cert; if empty a self-signed cert is generated
	std::string key_path;   // PEM private key
};

inline bool Enabled(const TlsConfig &c) {
	return c.implicit || c.starttls;
}

// A server-side SSL_CTX. Loads cert/key from disk, or generates an in-memory
// self-signed RSA cert when no paths are given (so dev TLS works out of the box).
class TlsContext {
public:
	TlsContext() = default;
	~TlsContext();
	TlsContext(const TlsContext &) = delete;
	TlsContext &operator=(const TlsContext &) = delete;

	bool Init(const TlsConfig &config, std::string &err);
	SSL_CTX *Get() const {
		return ctx_;
	}

private:
	SSL_CTX *ctx_ = nullptr;
};

// A client-side SSL_CTX for outbound STARTTLS / implicit TLS (the relay drainer).
// Opportunistic: the peer certificate is not verified, since mail relays commonly
// present self-signed or hostname-mismatched certs and encryption-without-
// authentication is the norm for MX-to-MX transport.
class ClientContext {
public:
	ClientContext() = default;
	~ClientContext();
	ClientContext(const ClientContext &) = delete;
	ClientContext &operator=(const ClientContext &) = delete;

	bool Init(std::string &err);
	SSL_CTX *Get() const {
		return ctx_;
	}

private:
	SSL_CTX *ctx_ = nullptr;
};

// One-time process-wide OpenSSL initialization (thread-safe, idempotent).
void EnsureOpenSSLInit();

} // namespace tls
} // namespace quackmail
