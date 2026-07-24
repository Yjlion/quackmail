#pragma once

#include <functional>
#include <string>
#include <vector>

namespace quackmail {
namespace dkim {

// RFC 6376 §3.9 / RFC 8601 verification outcomes.
enum class Result {
	None,      // the message carries no DKIM-Signature at all
	Pass,      // signature verified against the published key
	Fail,      // signature present but does not verify (body or header altered)
	Neutral,   // signature present but unusable (unknown algorithm, bad syntax)
	TempError, // key lookup failed transiently — retry, do not judge
	PermError, // key revoked or malformed
};

struct VerifyResult {
	Result result = Result::None;
	std::string domain;   // the d= tag: the signing identity
	std::string selector; // the s= tag
	std::string algorithm;
	std::string info; // human-readable detail for Authentication-Results
};

// Look up a DKIM key record ("v=DKIM1; k=rsa; p=...") for selector._domainkey
// .domain. Returning false means "no usable key" and the caller falls back to
// DNS; this indirection is what lets the verifier consult a locally stored key
// (quackmail_dkim_keys) and so run with no network at all.
using KeyLookup = std::function<bool(const std::string &selector, const std::string &domain,
                                     std::string &txt_record)>;

// Verify every DKIM-Signature header on `raw`, in the order they appear. An
// empty result vector means the message was unsigned. A message is generally
// considered DKIM-authenticated if *any* entry is Pass.
std::vector<VerifyResult> Verify(const std::string &raw, const KeyLookup &lookup = KeyLookup());

// Sign `raw` and return it in `out` with a DKIM-Signature header prepended.
// `header_list` is the colon-separated h= list; headers absent from the message
// are skipped. `private_key_pem` is an unencrypted PEM private key (RSA or
// Ed25519 — the algorithm is taken from the key type). Returns false + err.
bool Sign(const std::string &raw, const std::string &domain, const std::string &selector,
          const std::string &private_key_pem, const std::string &header_list, std::string &out,
          std::string &err);

// Generate an RSA key pair for signing. `priv_pem` is the PKCS#8 PEM to store;
// `pub_b64` is the base64 SubjectPublicKeyInfo that goes in the DNS record's
// p= tag. 2048 bits is the practical floor for deliverability.
bool GenerateKey(int bits, std::string &priv_pem, std::string &pub_b64, std::string &err);

// The full DNS TXT record body for a generated key, ready to publish at
// <selector>._domainkey.<domain>.
std::string DnsRecord(const std::string &pub_b64);

// The RFC 8601 result keyword ("pass", "fail", "none", ...).
std::string ResultName(Result r);

// ---------------------------------------------------------------------------
// Canonicalization (RFC 6376 §3.4), exposed because signing and verifying both
// need it and it is the part most worth testing directly.
// ---------------------------------------------------------------------------

// `relaxed` selects relaxed canonicalization; otherwise simple.
std::string CanonicalizeBody(const std::string &body, bool relaxed);
std::string CanonicalizeHeader(const std::string &name, const std::string &value, bool relaxed);

} // namespace dkim
} // namespace quackmail
