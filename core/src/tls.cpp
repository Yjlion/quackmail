#include "quackmail/tls.hpp"

#include <mutex>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace quackmail {
namespace tls {

void EnsureOpenSSLInit() {
	static std::once_flag once;
	std::call_once(once, []() { OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr); });
}

static std::string SSLError() {
	unsigned long e = ERR_get_error();
	if (e == 0) {
		return "unknown TLS error";
	}
	char buf[256];
	ERR_error_string_n(e, buf, sizeof(buf));
	return buf;
}

// Generate an in-memory self-signed certificate for CN=localhost and install it
// on the context. Used when no cert/key paths are supplied (dev/testing).
static bool InstallSelfSigned(SSL_CTX *ctx, std::string &err) {
	EVP_PKEY *pkey = EVP_RSA_gen(2048);
	if (!pkey) {
		err = "failed to generate RSA key: " + SSLError();
		return false;
	}
	X509 *x509 = X509_new();
	if (!x509) {
		EVP_PKEY_free(pkey);
		err = "failed to allocate X509";
		return false;
	}

	ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
	X509_gmtime_adj(X509_getm_notBefore(x509), 0);
	X509_gmtime_adj(X509_getm_notAfter(x509), 60L * 60L * 24L * 365L);
	X509_set_pubkey(x509, pkey);

	X509_NAME *name = X509_get_subject_name(x509);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
	                           reinterpret_cast<const unsigned char *>("localhost"), -1, -1, 0);
	X509_set_issuer_name(x509, name);

	bool ok = true;
	if (X509_sign(x509, pkey, EVP_sha256()) == 0) {
		err = "failed to self-sign cert: " + SSLError();
		ok = false;
	} else if (SSL_CTX_use_certificate(ctx, x509) != 1) {
		err = "failed to use generated cert: " + SSLError();
		ok = false;
	} else if (SSL_CTX_use_PrivateKey(ctx, pkey) != 1) {
		err = "failed to use generated key: " + SSLError();
		ok = false;
	}

	// The context takes its own references; release ours.
	X509_free(x509);
	EVP_PKEY_free(pkey);
	return ok;
}

TlsContext::~TlsContext() {
	SSL_CTX *live = ctx_.exchange(nullptr, std::memory_order_acq_rel);
	if (live) {
		SSL_CTX_free(live);
	}
	for (SSL_CTX *old : retired_) {
		SSL_CTX_free(old);
	}
	retired_.clear();
}

// Build a context from `config` without touching what is currently in service,
// so a reload that fails leaves the running listener exactly as it was.
static SSL_CTX *BuildServerCtx(const TlsConfig &config, std::string &err) {
	EnsureOpenSSLInit();
	SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx) {
		err = "SSL_CTX_new failed: " + SSLError();
		return nullptr;
	}
	SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

	if (!config.cert_path.empty() && !config.key_path.empty()) {
		if (SSL_CTX_use_certificate_chain_file(ctx, config.cert_path.c_str()) != 1) {
			err = "failed to load cert " + config.cert_path + ": " + SSLError();
			SSL_CTX_free(ctx);
			return nullptr;
		}
		if (SSL_CTX_use_PrivateKey_file(ctx, config.key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
			err = "failed to load key " + config.key_path + ": " + SSLError();
			SSL_CTX_free(ctx);
			return nullptr;
		}
		if (SSL_CTX_check_private_key(ctx) != 1) {
			err = "cert/key mismatch: " + SSLError();
			SSL_CTX_free(ctx);
			return nullptr;
		}
	} else {
		if (!InstallSelfSigned(ctx, err)) {
			SSL_CTX_free(ctx);
			return nullptr;
		}
	}
	return ctx;
}

bool TlsContext::Init(const TlsConfig &config, std::string &err) {
	SSL_CTX *ctx = BuildServerCtx(config, err);
	if (!ctx) {
		return false;
	}
	config_ = config;
	SSL_CTX *old = ctx_.exchange(ctx, std::memory_order_acq_rel);
	if (old) {
		retired_.push_back(old);
	}
	return true;
}

bool TlsContext::Reload(std::string &err) {
	if (config_.cert_path.empty() || config_.key_path.empty()) {
		// A generated self-signed certificate has no source to reload from, and
		// minting a new one would change a fingerprint somebody has already
		// chosen to trust.
		err = "no certificate file is configured for this listener";
		return false;
	}
	SSL_CTX *ctx = BuildServerCtx(config_, err);
	if (!ctx) {
		return false;
	}
	SSL_CTX *old = ctx_.exchange(ctx, std::memory_order_acq_rel);
	if (old) {
		// Kept rather than freed: a thread may be between Get() and SSL_new.
		retired_.push_back(old);
	}
	return true;
}

ClientContext::~ClientContext() {
	if (ctx_) {
		SSL_CTX_free(ctx_);
		ctx_ = nullptr;
	}
}

bool ClientContext::Init(std::string &err) {
	return Init(ClientTlsConfig(), err);
}

bool ClientContext::Init(const ClientTlsConfig &config, std::string &err) {
	EnsureOpenSSLInit();
	ctx_ = SSL_CTX_new(TLS_client_method());
	if (!ctx_) {
		err = "SSL_CTX_new(client) failed: " + SSLError();
		return false;
	}
	SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
	if (!config.verify_peer) {
		// Opportunistic transport encryption: do not verify the peer
		// certificate. See the note in tls.hpp — this is the MX-to-MX case.
		SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
		return true;
	}
	if (config.ca_bundle.empty()) {
		if (SSL_CTX_set_default_verify_paths(ctx_) != 1) {
			err = "no system CA store could be loaded: " + SSLError();
			return false;
		}
	} else if (SSL_CTX_load_verify_locations(ctx_, config.ca_bundle.c_str(), nullptr) != 1) {
		err = "failed to load CA bundle " + config.ca_bundle + ": " + SSLError();
		return false;
	}
	SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
	SSL_CTX_set_verify_depth(ctx_, 10);
	return true;
}

} // namespace tls
} // namespace quackmail
