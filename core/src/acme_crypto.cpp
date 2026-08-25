// The pure half of the ACME client: JOSE, JWK, CSR, certificate inspection.
//
// Split from the state machine so it can be tested without a server, a network
// or a database — RFC 7638 §3.1 and RFC 8555 §8.1 both come with known-answer
// vectors, and they are the only way to be sure a thumbprint is right before a
// CA tells you it is not.
#include "quackmail/acme.hpp"

#include "quackmail/json.hpp"
#include "quackmail/util.hpp"

#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <ctime>
#include <memory>

namespace quackmail {
namespace acme {

namespace {

std::string SslError() {
	char buf[256];
	ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
	return buf;
}

struct PkeyDeleter {
	void operator()(EVP_PKEY *p) const {
		EVP_PKEY_free(p);
	}
};
using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;

Pkey ReadPrivateKey(const std::string &pem, std::string &err) {
	BIO *bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
	if (!bio) {
		err = "out of memory";
		return Pkey();
	}
	EVP_PKEY *raw = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
	BIO_free(bio);
	if (!raw) {
		err = "the account key is not a readable PEM private key";
		return Pkey();
	}
	if (EVP_PKEY_base_id(raw) != EVP_PKEY_RSA) {
		EVP_PKEY_free(raw);
		err = "the account key is not RSA (this client signs RS256 only)";
		return Pkey();
	}
	return Pkey(raw);
}

// A BIGNUM as base64url of its big-endian bytes, with no leading zero byte —
// which is what a JWK member is (RFC 7518 §6.3.1).
std::string BnToB64Url(const BIGNUM *bn) {
	if (!bn) {
		return std::string();
	}
	int len = BN_num_bytes(bn);
	if (len <= 0) {
		return std::string();
	}
	std::string raw((size_t)len, '\0');
	BN_bn2bin(bn, reinterpret_cast<unsigned char *>(&raw[0]));
	return util::Base64UrlEncode(raw);
}

bool RsaParts(EVP_PKEY *pkey, std::string &n_b64, std::string &e_b64, std::string &err) {
	BIGNUM *n = nullptr;
	BIGNUM *e = nullptr;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	if (EVP_PKEY_get_bn_param(pkey, "n", &n) != 1 || EVP_PKEY_get_bn_param(pkey, "e", &e) != 1) {
		BN_free(n);
		BN_free(e);
		err = "could not read the RSA public parameters: " + SslError();
		return false;
	}
	n_b64 = BnToB64Url(n);
	e_b64 = BnToB64Url(e);
	BN_free(n);
	BN_free(e);
#else
	const RSA *rsa = EVP_PKEY_get0_RSA(pkey);
	if (!rsa) {
		err = "could not read the RSA public parameters";
		return false;
	}
	const BIGNUM *cn = nullptr;
	const BIGNUM *ce = nullptr;
	RSA_get0_key(rsa, &cn, &ce, nullptr);
	n_b64 = BnToB64Url(cn);
	e_b64 = BnToB64Url(ce);
	(void)n;
	(void)e;
#endif
	if (n_b64.empty() || e_b64.empty()) {
		err = "the RSA public parameters are empty";
		return false;
	}
	return true;
}

} // namespace

bool GenerateAccountKey(int bits, std::string &pem, std::string &err) {
	if (bits < 2048) {
		bits = 2048;
	}
	EVP_PKEY *pkey = nullptr;
	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
	if (!ctx) {
		err = "cannot create a key context";
		return false;
	}
	bool ok = EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) == 1 &&
	          EVP_PKEY_keygen(ctx, &pkey) == 1;
	EVP_PKEY_CTX_free(ctx);
	if (!ok || !pkey) {
		if (pkey) {
			EVP_PKEY_free(pkey);
		}
		err = "RSA key generation failed: " + SslError();
		return false;
	}
	BIO *bio = BIO_new(BIO_s_mem());
	if (!bio || PEM_write_bio_PKCS8PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
		if (bio) {
			BIO_free(bio);
		}
		EVP_PKEY_free(pkey);
		err = "could not serialize the key: " + SslError();
		return false;
	}
	char *data = nullptr;
	long len = BIO_get_mem_data(bio, &data);
	pem.assign(data, (size_t)len);
	BIO_free(bio);
	EVP_PKEY_free(pkey);
	return true;
}

bool JwkPublic(const std::string &key_pem, std::string &jwk_json, std::string &err) {
	Pkey pkey = ReadPrivateKey(key_pem, err);
	if (!pkey) {
		return false;
	}
	std::string n;
	std::string e;
	if (!RsaParts(pkey.get(), n, e, err)) {
		return false;
	}
	// Exactly this spelling: required members only, lexicographic order, no
	// whitespace. RFC 7638 §3.3 takes the thumbprint over these bytes, so this
	// is the wire format and not a style choice.
	jwk_json = "{\"e\":\"" + e + "\",\"kty\":\"RSA\",\"n\":\"" + n + "\"}";
	return true;
}

bool JwkThumbprint(const std::string &key_pem, std::string &thumbprint, std::string &err) {
	std::string jwk;
	if (!JwkPublic(key_pem, jwk, err)) {
		return false;
	}
	thumbprint = util::Base64UrlEncode(util::Sha256Raw(jwk));
	return true;
}

std::string KeyAuthorization(const std::string &token, const std::string &thumbprint) {
	return token + "." + thumbprint;
}

bool JwsSign(const std::string &key_pem, const std::string &protected_json,
             const std::string &payload, std::string &flattened, std::string &err) {
	Pkey pkey = ReadPrivateKey(key_pem, err);
	if (!pkey) {
		return false;
	}
	const std::string p64 = util::Base64UrlEncode(protected_json);
	// POST-as-GET is an *empty string* payload, not an empty object: RFC 8555
	// §6.3 distinguishes the two, and sending "{}" turns a read into a write.
	const std::string y64 = payload.empty() ? std::string() : util::Base64UrlEncode(payload);
	const std::string signing_input = p64 + "." + y64;

	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if (!ctx) {
		err = "out of memory";
		return false;
	}
	std::string signature;
	size_t siglen = 0;
	bool ok = EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey.get()) == 1 &&
	          EVP_DigestSignUpdate(ctx, signing_input.data(), signing_input.size()) == 1 &&
	          EVP_DigestSignFinal(ctx, nullptr, &siglen) == 1;
	if (ok) {
		signature.resize(siglen);
		ok = EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char *>(&signature[0]), &siglen) == 1;
		signature.resize(siglen);
	}
	EVP_MD_CTX_free(ctx);
	if (!ok) {
		err = "JWS signing failed: " + SslError();
		return false;
	}

	flattened = "{\"protected\":\"" + p64 + "\",\"payload\":\"" + y64 + "\",\"signature\":\"" +
	            util::Base64UrlEncode(signature) + "\"}";
	return true;
}

bool MakeCsr(const std::vector<std::string> &dns_names, std::string &key_pem,
             std::string &csr_b64url, std::string &err) {
	if (dns_names.empty()) {
		err = "a certificate needs at least one name";
		return false;
	}
	for (const std::string &n : dns_names) {
		// The names come from configuration, never from a request, but a name
		// with a comma or a control character in it would produce a CSR that
		// says something other than what the operator wrote.
		if (n.empty() || n.size() > 253 ||
		    n.find_first_of(" ,\r\n\t\"\\") != std::string::npos) {
			err = "'" + n + "' is not a usable DNS name";
			return false;
		}
	}
	if (key_pem.empty() && !GenerateAccountKey(2048, key_pem, err)) {
		return false;
	}
	Pkey pkey = ReadPrivateKey(key_pem, err);
	if (!pkey) {
		return false;
	}

	X509_REQ *req = X509_REQ_new();
	if (!req) {
		err = "out of memory";
		return false;
	}
	bool ok = X509_REQ_set_version(req, 0) == 1 && X509_REQ_set_pubkey(req, pkey.get()) == 1;

	if (ok) {
		X509_NAME *name = X509_REQ_get_subject_name(req);
		ok = X509_NAME_add_entry_by_txt(
		         name, "CN", MBSTRING_ASC,
		         reinterpret_cast<const unsigned char *>(dns_names[0].c_str()), -1, -1, 0) == 1;
	}

	// Every name goes in the SAN, including the first: a CN-only certificate has
	// not been accepted by a browser for years.
	if (ok) {
		std::string san;
		for (const std::string &n : dns_names) {
			if (!san.empty()) {
				san += ",";
			}
			san += "DNS:" + n;
		}
		STACK_OF(X509_EXTENSION) *exts = sk_X509_EXTENSION_new_null();
		X509_EXTENSION *ext =
		    X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, san.c_str());
		if (!exts || !ext) {
			ok = false;
			err = "could not build the subjectAltName extension: " + SslError();
		} else {
			sk_X509_EXTENSION_push(exts, ext);
			ok = X509_REQ_add_extensions(req, exts) == 1;
		}
		if (exts) {
			sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
		}
	}

	if (ok && X509_REQ_sign(req, pkey.get(), EVP_sha256()) <= 0) {
		ok = false;
		err = "could not sign the CSR: " + SslError();
	}
	if (!ok) {
		X509_REQ_free(req);
		if (err.empty()) {
			err = "could not build the CSR: " + SslError();
		}
		return false;
	}

	unsigned char *der = nullptr;
	int der_len = i2d_X509_REQ(req, &der);
	X509_REQ_free(req);
	if (der_len <= 0 || !der) {
		err = "could not encode the CSR";
		return false;
	}
	csr_b64url = util::Base64UrlEncode(std::string(reinterpret_cast<char *>(der), (size_t)der_len));
	OPENSSL_free(der);
	return true;
}

bool CertNotAfter(const std::string &cert_pem, int64_t &not_after, std::string &err) {
	BIO *bio = BIO_new_mem_buf(cert_pem.data(), (int)cert_pem.size());
	if (!bio) {
		err = "out of memory";
		return false;
	}
	X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
	BIO_free(bio);
	if (!cert) {
		err = "not a readable PEM certificate";
		return false;
	}
	const ASN1_TIME *t = X509_get0_notAfter(cert);
	struct tm tm {};
	int ok = ASN1_TIME_to_tm(t, &tm);
	X509_free(cert);
	if (ok != 1) {
		err = "the certificate has no readable expiry";
		return false;
	}
	not_after = (int64_t)timegm(&tm);
	return true;
}

} // namespace acme
} // namespace quackmail
