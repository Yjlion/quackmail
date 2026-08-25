#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

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
//
// The context can be **replaced while listeners are running**, which is what
// makes an automatically renewed certificate take effect without dropping
// connections: ServerController::AcceptLoop re-reads Get() on every accept
// rather than caching it, so a swapped pointer is picked up by the next
// connection.
//
// A retired SSL_CTX is *kept*, not freed, until this object is destroyed. A
// thread can be between Get() and SSL_new when the swap happens, and freeing
// underneath it is a crash; a certificate is renewed a handful of times a year,
// so the alternative costs a few kilobytes over a process lifetime and buys a
// whole class of race away.
class TlsContext {
public:
	TlsContext() = default;
	~TlsContext();
	TlsContext(const TlsContext &) = delete;
	TlsContext &operator=(const TlsContext &) = delete;

	bool Init(const TlsConfig &config, std::string &err);

	// Rebuild from the configuration Init was given and swap the result in.
	// Returns false with `err` set when there is nothing to reload (no cert
	// paths were configured — regenerating the self-signed one would change a
	// fingerprint clients have already accepted) or when the new material does
	// not load, in which case the old context stays in service.
	bool Reload(std::string &err);

	SSL_CTX *Get() const {
		return ctx_.load(std::memory_order_acquire);
	}

private:
	std::atomic<SSL_CTX *> ctx_ {nullptr};
	TlsConfig config_;
	std::vector<SSL_CTX *> retired_;
};

struct ClientTlsConfig {
	// Verify the peer certificate and its name. Off by default, because the
	// original caller is MX-to-MX transport where it must stay off.
	bool verify_peer = false;
	// A PEM bundle to trust instead of the system store. Only meaningful with
	// verify_peer, and the reason a private ACME server (step-ca, a lab Boulder)
	// works at all.
	std::string ca_bundle;
};

// A client-side SSL_CTX for outbound TLS.
//
// The no-argument Init is **opportunistic and verifies nothing**, and that is
// deliberate: mail relays commonly present self-signed or hostname-mismatched
// certificates, and encryption-without-authentication is the norm for MX-to-MX
// transport. Weakening that is not on the table and strengthening it is a
// separate decision, so the verifying mode is a second entry point rather than a
// change to this one.
//
// Anything talking to a service whose identity actually matters — an ACME
// directory above all — must use the ClientTlsConfig overload with
// verify_peer set, and must pass the host name to ClientStream::ConnectTls so
// SNI is sent and the name is checked.
class ClientContext {
public:
	ClientContext() = default;
	~ClientContext();
	ClientContext(const ClientContext &) = delete;
	ClientContext &operator=(const ClientContext &) = delete;

	bool Init(std::string &err);
	bool Init(const ClientTlsConfig &config, std::string &err);
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
