#include "quackmail/dkim.hpp"

#include "quackmail/dns.hpp"
#include "quackmail/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <utility>

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

namespace quackmail {
namespace dkim {

namespace {

std::string Lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string Trim(const std::string &s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) {
		return "";
	}
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

bool IsWsp(char c) {
	return c == ' ' || c == '\t';
}

// ---------------------------------------------------------------------------
// Message splitting
// ---------------------------------------------------------------------------

// One header as it appeared on the wire: the name, the raw value (folded lines
// included, no trailing CRLF), in original order. DKIM signs headers bottom-up
// and needs the exact bytes, so this cannot reuse mime::Parse's unfolded view.
struct RawHeader {
	std::string name;
	std::string value;
};

// Split `raw` into its header list and body. Accepts CRLF and bare LF input and
// normalises to CRLF, since the canonicalizations are defined on CRLF text.
void SplitMessage(const std::string &raw, std::vector<RawHeader> &headers, std::string &body) {
	headers.clear();
	body.clear();

	// Normalise line endings first.
	std::string norm;
	norm.reserve(raw.size() + raw.size() / 32);
	for (size_t i = 0; i < raw.size(); i++) {
		if (raw[i] == '\r') {
			continue; // re-added below
		}
		if (raw[i] == '\n') {
			norm += "\r\n";
		} else {
			norm += raw[i];
		}
	}

	auto sep = norm.find("\r\n\r\n");
	std::string head;
	if (sep == std::string::npos) {
		head = norm;
	} else {
		head = norm.substr(0, sep + 2); // keep the final CRLF of the last header
		body = norm.substr(sep + 4);
	}

	// Walk the header block, joining continuation lines (leading WSP).
	size_t pos = 0;
	std::string cur_name, cur_value;
	bool have = false;
	auto flush = [&]() {
		if (have) {
			headers.push_back(RawHeader {cur_name, cur_value});
		}
		have = false;
		cur_name.clear();
		cur_value.clear();
	};
	while (pos < head.size()) {
		size_t eol = head.find("\r\n", pos);
		if (eol == std::string::npos) {
			eol = head.size();
		}
		std::string line = head.substr(pos, eol - pos);
		pos = eol + 2;
		if (line.empty()) {
			continue;
		}
		if (IsWsp(line[0])) {
			if (have) {
				cur_value += "\r\n";
				cur_value += line;
			}
			continue;
		}
		flush();
		auto colon = line.find(':');
		if (colon == std::string::npos) {
			continue; // not a header; ignore
		}
		cur_name = line.substr(0, colon);
		cur_value = line.substr(colon + 1);
		have = true;
	}
	flush();
}

// ---------------------------------------------------------------------------
// Hashing helpers
// ---------------------------------------------------------------------------

std::string Digest(const std::string &data, const EVP_MD *md) {
	unsigned char out[EVP_MAX_MD_SIZE];
	unsigned int len = 0;
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if (!ctx) {
		return "";
	}
	std::string result;
	if (EVP_DigestInit_ex(ctx, md, nullptr) == 1 &&
	    EVP_DigestUpdate(ctx, data.data(), data.size()) == 1 &&
	    EVP_DigestFinal_ex(ctx, out, &len) == 1) {
		result.assign(reinterpret_cast<char *>(out), len);
	}
	EVP_MD_CTX_free(ctx);
	return result;
}

// ---------------------------------------------------------------------------
// Tag lists ("v=1; a=rsa-sha256; d=example.com; ...")
// ---------------------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> ParseTagList(const std::string &in) {
	std::vector<std::pair<std::string, std::string>> tags;
	size_t pos = 0;
	while (pos < in.size()) {
		auto semi = in.find(';', pos);
		std::string chunk = in.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
		pos = semi == std::string::npos ? in.size() : semi + 1;
		auto eq = chunk.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		std::string name = Trim(chunk.substr(0, eq));
		std::string value = chunk.substr(eq + 1);
		// Values may be folded across lines and padded with WSP, which is not
		// part of the value (§3.2). Base64 values additionally ignore all WSP.
		std::string cleaned;
		for (char c : value) {
			if (c == '\r' || c == '\n') {
				continue;
			}
			cleaned += c;
		}
		tags.emplace_back(Lower(name), Trim(cleaned));
	}
	return tags;
}

std::string TagValue(const std::vector<std::pair<std::string, std::string>> &tags,
                     const std::string &name) {
	for (auto &t : tags) {
		if (t.first == name) {
			return t.second;
		}
	}
	return "";
}

// Strip all whitespace — base64 tag values (b=, bh=, p=) are folded freely.
std::string StripWsp(const std::string &in) {
	std::string out;
	for (char c : in) {
		if (!IsWsp(c) && c != '\r' && c != '\n') {
			out += c;
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// Key handling
// ---------------------------------------------------------------------------

// Build an EVP_PKEY from the base64 p= tag. RSA keys are SubjectPublicKeyInfo;
// Ed25519 keys (RFC 8463) are the raw 32-byte public key.
EVP_PKEY *PublicKeyFromTag(const std::string &p_b64, const std::string &key_type, std::string &err) {
	std::string der;
	if (!util::Base64Decode(StripWsp(p_b64), der) || der.empty()) {
		err = "key record p= is not valid base64";
		return nullptr;
	}
	if (key_type == "ed25519") {
		if (der.size() != 32) {
			err = "ed25519 public key must be 32 bytes";
			return nullptr;
		}
		EVP_PKEY *k = EVP_PKEY_new_raw_public_key(
		    EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(der.data()), der.size());
		if (!k) {
			err = "ed25519 public key rejected";
		}
		return k;
	}
	const unsigned char *p = reinterpret_cast<const unsigned char *>(der.data());
	EVP_PKEY *k = d2i_PUBKEY(nullptr, &p, (long)der.size());
	if (!k) {
		err = "public key is not a valid SubjectPublicKeyInfo";
	}
	return k;
}

// Fetch the key record for selector._domainkey.domain, preferring `lookup`.
bool FetchKeyRecord(const KeyLookup &lookup, const std::string &selector, const std::string &domain,
                    std::string &record, bool &temp_fail) {
	temp_fail = false;
	if (lookup && lookup(selector, domain, record) && !record.empty()) {
		return true;
	}
	std::vector<std::string> txts;
	auto st = dns::LookupTXT(selector + "._domainkey." + domain, txts);
	if (st == dns::Status::TempFail) {
		temp_fail = true;
		return false;
	}
	for (auto &t : txts) {
		// A key record is the one carrying a p= tag; other TXT at that name
		// (rare, but legal) is skipped.
		if (t.find("p=") != std::string::npos) {
			record = t;
			return true;
		}
	}
	return false;
}

const EVP_MD *HashForAlgorithm(const std::string &algo) {
	if (algo == "rsa-sha1") {
		return EVP_sha1();
	}
	// rsa-sha256 and ed25519-sha256 both hash with SHA-256.
	return EVP_sha256();
}

} // namespace

// ---------------------------------------------------------------------------
// Canonicalization (RFC 6376 §3.4)
// ---------------------------------------------------------------------------

std::string CanonicalizeBody(const std::string &body, bool relaxed) {
	// Split into CRLF-terminated lines.
	std::vector<std::string> lines;
	size_t pos = 0;
	while (pos < body.size()) {
		size_t eol = body.find("\r\n", pos);
		if (eol == std::string::npos) {
			lines.push_back(body.substr(pos));
			break;
		}
		lines.push_back(body.substr(pos, eol - pos));
		pos = eol + 2;
	}

	if (relaxed) {
		// §3.4.4: collapse WSP runs to one SP, drop trailing WSP on each line.
		for (auto &line : lines) {
			std::string out;
			bool in_wsp = false;
			for (char c : line) {
				if (IsWsp(c)) {
					in_wsp = true;
					continue;
				}
				if (in_wsp && !out.empty()) {
					out += ' ';
				}
				in_wsp = false;
				out += c;
			}
			line = out;
		}
	}

	// Both forms drop trailing empty lines.
	while (!lines.empty() && lines.back().empty()) {
		lines.pop_back();
	}

	if (lines.empty()) {
		// simple: an empty body canonicalizes to a single CRLF.
		// relaxed: an empty body canonicalizes to nothing at all.
		return relaxed ? "" : "\r\n";
	}

	std::string out;
	for (auto &line : lines) {
		out += line;
		out += "\r\n";
	}
	return out;
}

std::string CanonicalizeHeader(const std::string &name, const std::string &value, bool relaxed) {
	if (!relaxed) {
		// simple: the header exactly as received, plus its CRLF.
		return name + ":" + value + "\r\n";
	}
	// §3.4.2: lowercase the name, unfold, collapse WSP runs to one SP, strip
	// leading and trailing WSP from the value, and remove WSP around the colon.
	std::string unfolded;
	for (char c : value) {
		if (c == '\r' || c == '\n') {
			continue; // unfold: the CRLF of a continuation disappears
		}
		unfolded += c;
	}
	std::string collapsed;
	bool in_wsp = false;
	for (char c : unfolded) {
		if (IsWsp(c)) {
			in_wsp = true;
			continue;
		}
		if (in_wsp && !collapsed.empty()) {
			collapsed += ' ';
		}
		in_wsp = false;
		collapsed += c;
	}
	return Lower(Trim(name)) + ":" + collapsed + "\r\n";
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

std::vector<VerifyResult> Verify(const std::string &raw, const KeyLookup &lookup) {
	std::vector<VerifyResult> results;

	std::vector<RawHeader> headers;
	std::string body;
	SplitMessage(raw, headers, body);

	for (size_t sig_idx = 0; sig_idx < headers.size(); sig_idx++) {
		if (Lower(Trim(headers[sig_idx].name)) != "dkim-signature") {
			continue;
		}
		VerifyResult vr;
		auto tags = ParseTagList(headers[sig_idx].value);

		vr.domain = TagValue(tags, "d");
		vr.selector = TagValue(tags, "s");
		vr.algorithm = Lower(TagValue(tags, "a"));
		std::string b_b64 = StripWsp(TagValue(tags, "b"));
		std::string bh_b64 = StripWsp(TagValue(tags, "bh"));
		std::string h_list = TagValue(tags, "h");
		std::string canon = Lower(TagValue(tags, "c"));
		std::string l_tag = TagValue(tags, "l");
		std::string x_tag = TagValue(tags, "x");

		if (vr.domain.empty() || vr.selector.empty() || b_b64.empty() || bh_b64.empty() ||
		    h_list.empty()) {
			vr.result = Result::PermError;
			vr.info = "signature is missing a required tag";
			results.push_back(vr);
			continue;
		}
		if (vr.algorithm.empty()) {
			vr.algorithm = "rsa-sha256";
		}
		if (vr.algorithm != "rsa-sha256" && vr.algorithm != "rsa-sha1" &&
		    vr.algorithm != "ed25519-sha256") {
			vr.result = Result::Neutral;
			vr.info = "unsupported algorithm " + vr.algorithm;
			results.push_back(vr);
			continue;
		}
		if (!x_tag.empty()) {
			long long expires = std::atoll(x_tag.c_str());
			if (expires > 0 && (long long)std::time(nullptr) > expires) {
				vr.result = Result::Fail;
				vr.info = "signature expired";
				results.push_back(vr);
				continue;
			}
		}

		// c= is "header/body"; either half may be omitted and defaults to simple.
		bool relaxed_header = false, relaxed_body = false;
		if (!canon.empty()) {
			auto slash = canon.find('/');
			std::string hpart = slash == std::string::npos ? canon : canon.substr(0, slash);
			std::string bpart = slash == std::string::npos ? "simple" : canon.substr(slash + 1);
			relaxed_header = (Trim(hpart) == "relaxed");
			relaxed_body = (Trim(bpart) == "relaxed");
		}

		const EVP_MD *md = HashForAlgorithm(vr.algorithm);

		// --- body hash ---
		std::string canon_body = CanonicalizeBody(body, relaxed_body);
		if (!l_tag.empty()) {
			// l= limits the signature to a prefix of the body; anything appended
			// past that point is deliberately unsigned.
			size_t limit = (size_t)std::atoll(l_tag.c_str());
			if (limit < canon_body.size()) {
				canon_body.resize(limit);
			}
		}
		std::string want_bh;
		if (!util::Base64Decode(bh_b64, want_bh)) {
			vr.result = Result::PermError;
			vr.info = "bh= is not valid base64";
			results.push_back(vr);
			continue;
		}
		if (Digest(canon_body, md) != want_bh) {
			vr.result = Result::Fail;
			vr.info = "body hash mismatch";
			results.push_back(vr);
			continue;
		}

		// --- fetch the public key ---
		std::string key_record;
		bool temp_fail = false;
		if (!FetchKeyRecord(lookup, vr.selector, vr.domain, key_record, temp_fail)) {
			vr.result = temp_fail ? Result::TempError : Result::PermError;
			vr.info = temp_fail ? "key lookup failed temporarily" : "no key for " + vr.selector +
			                                                            "._domainkey." + vr.domain;
			results.push_back(vr);
			continue;
		}
		auto key_tags = ParseTagList(key_record);
		std::string p_tag = StripWsp(TagValue(key_tags, "p"));
		std::string key_type = Lower(TagValue(key_tags, "k"));
		if (key_type.empty()) {
			key_type = vr.algorithm == "ed25519-sha256" ? "ed25519" : "rsa";
		}
		if (p_tag.empty()) {
			vr.result = Result::PermError;
			vr.info = "key record has been revoked (empty p=)";
			results.push_back(vr);
			continue;
		}
		std::string key_err;
		EVP_PKEY *pkey = PublicKeyFromTag(p_tag, key_type, key_err);
		if (!pkey) {
			vr.result = Result::PermError;
			vr.info = key_err;
			results.push_back(vr);
			continue;
		}

		// --- rebuild the signed header block ---
		// §5.4.2: headers are taken bottom-up, each name consuming the next
		// unused occurrence from the end, so a prepended duplicate cannot be
		// substituted for the signed one.
		std::vector<bool> used(headers.size(), false);
		used[sig_idx] = true; // the signature header itself is appended last
		std::string signed_headers;
		{
			std::string list = h_list;
			size_t pos = 0;
			while (pos <= list.size()) {
				auto colon = list.find(':', pos);
				std::string want =
				    Lower(Trim(list.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos)));
				pos = colon == std::string::npos ? list.size() + 1 : colon + 1;
				if (want.empty()) {
					continue;
				}
				for (size_t i = headers.size(); i-- > 0;) {
					if (used[i] || Lower(Trim(headers[i].name)) != want) {
						continue;
					}
					used[i] = true;
					signed_headers += CanonicalizeHeader(headers[i].name, headers[i].value, relaxed_header);
					break;
				}
			}
		}

		// The DKIM-Signature header itself is included with b= emptied and with
		// no trailing CRLF.
		std::string sig_value = headers[sig_idx].value;
		{
			// Erase everything after "b=" up to the next ';'.
			std::string lowered = Lower(sig_value);
			size_t bpos = std::string::npos;
			for (size_t i = 0; i + 1 < lowered.size(); i++) {
				if (lowered[i] != 'b' || lowered[i + 1] != '=') {
					continue;
				}
				// Must be at the start of a tag: preceded only by WSP/';'/start.
				size_t j = i;
				bool at_tag_start = true;
				while (j-- > 0) {
					char c = lowered[j];
					if (IsWsp(c) || c == '\r' || c == '\n') {
						continue;
					}
					at_tag_start = (c == ';');
					break;
				}
				if (at_tag_start) {
					bpos = i;
					break;
				}
			}
			if (bpos != std::string::npos) {
				size_t end = sig_value.find(';', bpos);
				sig_value = sig_value.substr(0, bpos + 2) +
				            (end == std::string::npos ? "" : sig_value.substr(end));
			}
		}
		std::string canon_sig = CanonicalizeHeader(headers[sig_idx].name, sig_value, relaxed_header);
		if (canon_sig.size() >= 2) {
			canon_sig.resize(canon_sig.size() - 2); // drop the trailing CRLF
		}
		signed_headers += canon_sig;

		// --- verify ---
		std::string signature;
		if (!util::Base64Decode(b_b64, signature)) {
			EVP_PKEY_free(pkey);
			vr.result = Result::PermError;
			vr.info = "b= is not valid base64";
			results.push_back(vr);
			continue;
		}

		bool ok = false;
		EVP_MD_CTX *ctx = EVP_MD_CTX_new();
		if (ctx) {
			if (vr.algorithm == "ed25519-sha256") {
				// RFC 8463: Ed25519 signs the SHA-256 hash of the header block
				// with pure Ed25519 (no additional prehash inside the signature).
				std::string h = Digest(signed_headers, EVP_sha256());
				if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1) {
					ok = EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char *>(signature.data()),
					                      signature.size(),
					                      reinterpret_cast<const unsigned char *>(h.data()), h.size()) == 1;
				}
			} else {
				if (EVP_DigestVerifyInit(ctx, nullptr, md, nullptr, pkey) == 1 &&
				    EVP_DigestVerifyUpdate(ctx, signed_headers.data(), signed_headers.size()) == 1) {
					ok = EVP_DigestVerifyFinal(ctx,
					                           reinterpret_cast<const unsigned char *>(signature.data()),
					                           signature.size()) == 1;
				}
			}
			EVP_MD_CTX_free(ctx);
		}
		EVP_PKEY_free(pkey);

		vr.result = ok ? Result::Pass : Result::Fail;
		vr.info = ok ? "signature verified" : "signature does not verify";
		results.push_back(vr);
	}

	return results;
}

// ---------------------------------------------------------------------------
// Signing
// ---------------------------------------------------------------------------

bool Sign(const std::string &raw, const std::string &domain, const std::string &selector,
          const std::string &private_key_pem, const std::string &header_list, std::string &out,
          std::string &err) {
	if (domain.empty() || selector.empty() || private_key_pem.empty()) {
		err = "domain, selector and private key are all required";
		return false;
	}

	BIO *bio = BIO_new_mem_buf(private_key_pem.data(), (int)private_key_pem.size());
	if (!bio) {
		err = "out of memory";
		return false;
	}
	EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
	BIO_free(bio);
	if (!pkey) {
		err = "private key is not a readable PEM";
		return false;
	}

	bool ed25519 = EVP_PKEY_id(pkey) == EVP_PKEY_ED25519;
	std::string algo = ed25519 ? "ed25519-sha256" : "rsa-sha256";
	const EVP_MD *md = EVP_sha256();

	std::vector<RawHeader> headers;
	std::string body;
	SplitMessage(raw, headers, body);

	// We always sign relaxed/relaxed: it survives the whitespace fixups that
	// intermediate MTAs still perform, which simple/simple does not.
	std::string canon_body = CanonicalizeBody(body, true);
	std::string bh = util::Base64Encode(Digest(canon_body, md));

	// Keep only the h= names that are actually present, preserving the caller's
	// order — signing an absent header would make the signature unverifiable.
	std::vector<std::string> want;
	{
		std::string list = header_list.empty() ? "from:to:cc:subject:date:message-id:mime-version:"
		                                         "content-type:content-transfer-encoding"
		                                       : header_list;
		size_t pos = 0;
		while (pos <= list.size()) {
			auto colon = list.find(':', pos);
			std::string name =
			    Lower(Trim(list.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos)));
			pos = colon == std::string::npos ? list.size() + 1 : colon + 1;
			if (name.empty()) {
				continue;
			}
			for (auto &h : headers) {
				if (Lower(Trim(h.name)) == name) {
					want.push_back(name);
					break;
				}
			}
		}
	}
	if (want.empty()) {
		EVP_PKEY_free(pkey);
		err = "none of the headers to sign are present";
		return false;
	}

	std::string h_tag;
	for (size_t i = 0; i < want.size(); i++) {
		h_tag += (i ? ":" : "") + want[i];
	}

	// Build the signature header with an empty b=, sign, then fill b= in.
	std::string sig_value = " v=1; a=" + algo + "; c=relaxed/relaxed;\r\n" + "\td=" + domain +
	                        "; s=" + selector + ";\r\n" + "\tt=" + std::to_string((long long)std::time(nullptr)) +
	                        ";\r\n" + "\th=" + h_tag + ";\r\n" + "\tbh=" + bh + ";\r\n" + "\tb=";

	std::string signed_headers;
	{
		std::vector<bool> used(headers.size(), false);
		for (auto &name : want) {
			for (size_t i = headers.size(); i-- > 0;) {
				if (used[i] || Lower(Trim(headers[i].name)) != name) {
					continue;
				}
				used[i] = true;
				signed_headers += CanonicalizeHeader(headers[i].name, headers[i].value, true);
				break;
			}
		}
	}
	std::string canon_sig = CanonicalizeHeader("DKIM-Signature", sig_value, true);
	if (canon_sig.size() >= 2) {
		canon_sig.resize(canon_sig.size() - 2);
	}
	signed_headers += canon_sig;

	std::string signature;
	{
		EVP_MD_CTX *ctx = EVP_MD_CTX_new();
		if (!ctx) {
			EVP_PKEY_free(pkey);
			err = "out of memory";
			return false;
		}
		bool ok = false;
		size_t siglen = 0;
		if (ed25519) {
			std::string h = Digest(signed_headers, EVP_sha256());
			if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) == 1 &&
			    EVP_DigestSign(ctx, nullptr, &siglen, reinterpret_cast<const unsigned char *>(h.data()),
			                   h.size()) == 1) {
				signature.resize(siglen);
				ok = EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(&signature[0]), &siglen,
				                    reinterpret_cast<const unsigned char *>(h.data()), h.size()) == 1;
			}
		} else {
			if (EVP_DigestSignInit(ctx, nullptr, md, nullptr, pkey) == 1 &&
			    EVP_DigestSignUpdate(ctx, signed_headers.data(), signed_headers.size()) == 1 &&
			    EVP_DigestSignFinal(ctx, nullptr, &siglen) == 1) {
				signature.resize(siglen);
				ok = EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char *>(&signature[0]), &siglen) == 1;
			}
		}
		EVP_MD_CTX_free(ctx);
		EVP_PKEY_free(pkey);
		if (!ok) {
			char buf[256];
			ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
			err = std::string("signing failed: ") + buf;
			return false;
		}
		signature.resize(siglen);
	}

	// Fold the base64 signature so the header stays under the line limit.
	std::string b64 = util::Base64Encode(signature);
	std::string folded;
	for (size_t i = 0; i < b64.size(); i += 64) {
		if (i) {
			folded += "\r\n\t";
		}
		folded += b64.substr(i, 64);
	}

	out = "DKIM-Signature:" + sig_value + folded + "\r\n" + raw;
	return true;
}

bool GenerateKey(int bits, std::string &priv_pem, std::string &pub_b64, std::string &err) {
	if (bits < 1024) {
		bits = 2048;
	}
	EVP_PKEY *pkey = nullptr;
	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
	if (!ctx) {
		err = "cannot create key context";
		return false;
	}
	bool ok = EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) == 1 &&
	          EVP_PKEY_keygen(ctx, &pkey) == 1;
	EVP_PKEY_CTX_free(ctx);
	if (!ok || !pkey) {
		if (pkey) {
			EVP_PKEY_free(pkey);
		}
		err = "RSA key generation failed";
		return false;
	}

	// Private half as PKCS#8 PEM.
	BIO *bio = BIO_new(BIO_s_mem());
	if (!bio || PEM_write_bio_PKCS8PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
		if (bio) {
			BIO_free(bio);
		}
		EVP_PKEY_free(pkey);
		err = "cannot serialise the private key";
		return false;
	}
	{
		char *data = nullptr;
		long len = BIO_get_mem_data(bio, &data);
		priv_pem.assign(data, (size_t)len);
	}
	BIO_free(bio);

	// Public half as base64 SubjectPublicKeyInfo — the DNS p= tag.
	unsigned char *der = nullptr;
	int der_len = i2d_PUBKEY(pkey, &der);
	EVP_PKEY_free(pkey);
	if (der_len <= 0 || !der) {
		err = "cannot serialise the public key";
		return false;
	}
	pub_b64 = util::Base64Encode(std::string(reinterpret_cast<char *>(der), (size_t)der_len));
	OPENSSL_free(der);
	return true;
}

std::string DnsRecord(const std::string &pub_b64) {
	return "v=DKIM1; k=rsa; p=" + pub_b64;
}

std::string ResultName(Result r) {
	switch (r) {
	case Result::None:
		return "none";
	case Result::Pass:
		return "pass";
	case Result::Fail:
		return "fail";
	case Result::Neutral:
		return "neutral";
	case Result::TempError:
		return "temperror";
	case Result::PermError:
		return "permerror";
	}
	return "none";
}

} // namespace dkim
} // namespace quackmail
