#include "quackmail/sshkeys.hpp"

#include "quackmail/citadel_store.hpp"
#include "quackmail/util.hpp"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>

#include <ctime>
#include <memory>
#include <mutex>

namespace quackmail {
namespace ssh {

using duckdb::Connection;
using duckdb::MaterializedQueryResult;
using duckdb::Value;

PublicKey::PublicKey() {
}
HostKey::HostKey() {
}
StoredKey::StoredKey() {
}

// ---- wire encoding --------------------------------------------------------

bool Reader::Take(size_t n, std::string &out) {
	if (!ok_ || data_.size() - pos_ < n) {
		ok_ = false;
		return false;
	}
	out.assign(data_, pos_, n);
	pos_ += n;
	return true;
}

bool Reader::Byte(uint8_t &out) {
	std::string b;
	if (!Take(1, b)) {
		return false;
	}
	out = (uint8_t)b[0];
	return true;
}

bool Reader::Bool(bool &out) {
	uint8_t b = 0;
	if (!Byte(b)) {
		return false;
	}
	out = b != 0;
	return true;
}

bool Reader::U32(uint32_t &out) {
	std::string b;
	if (!Take(4, b)) {
		return false;
	}
	out = ((uint32_t)(uint8_t)b[0] << 24) | ((uint32_t)(uint8_t)b[1] << 16) | ((uint32_t)(uint8_t)b[2] << 8) |
	      (uint32_t)(uint8_t)b[3];
	return true;
}

bool Reader::U64(uint64_t &out) {
	uint32_t hi = 0, lo = 0;
	if (!U32(hi) || !U32(lo)) {
		return false;
	}
	out = ((uint64_t)hi << 32) | lo;
	return true;
}

bool Reader::String(std::string &out) {
	uint32_t n = 0;
	if (!U32(n)) {
		return false;
	}
	return Take(n, out);
}

bool Reader::Mpint(std::string &out) {
	if (!String(out)) {
		return false;
	}
	// A negative mpint is never a valid key or signature component.
	if (!out.empty() && ((uint8_t)out[0] & 0x80)) {
		ok_ = false;
		return false;
	}
	size_t i = 0;
	while (i < out.size() && out[i] == 0) {
		i++;
	}
	out.erase(0, i);
	return true;
}

bool Reader::NameList(std::vector<std::string> &out) {
	std::string s;
	if (!String(s)) {
		return false;
	}
	out.clear();
	if (s.empty()) {
		return true;
	}
	size_t start = 0;
	while (true) {
		size_t comma = s.find(',', start);
		out.push_back(s.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
		if (comma == std::string::npos) {
			break;
		}
		start = comma + 1;
	}
	return true;
}

bool Reader::Rest(std::string &out) {
	return Take(data_.size() - pos_, out);
}

Writer &Writer::Byte(uint8_t v) {
	out_.push_back((char)v);
	return *this;
}

Writer &Writer::Bool(bool v) {
	return Byte(v ? 1 : 0);
}

Writer &Writer::U32(uint32_t v) {
	out_.push_back((char)(v >> 24));
	out_.push_back((char)(v >> 16));
	out_.push_back((char)(v >> 8));
	out_.push_back((char)v);
	return *this;
}

Writer &Writer::U64(uint64_t v) {
	U32((uint32_t)(v >> 32));
	return U32((uint32_t)v);
}

Writer &Writer::String(const std::string &v) {
	U32((uint32_t)v.size());
	out_ += v;
	return *this;
}

Writer &Writer::Mpint(const std::string &v) {
	size_t i = 0;
	while (i < v.size() && v[i] == 0) {
		i++;
	}
	std::string mag = v.substr(i);
	if (!mag.empty() && ((uint8_t)mag[0] & 0x80)) {
		mag.insert(mag.begin(), '\0');
	}
	return String(mag);
}

Writer &Writer::NameList(const std::vector<std::string> &v) {
	return String(Join(v));
}

Writer &Writer::Raw(const std::string &v) {
	out_ += v;
	return *this;
}

std::string Join(const std::vector<std::string> &names) {
	std::string out;
	for (auto &n : names) {
		if (!out.empty()) {
			out += ",";
		}
		out += n;
	}
	return out;
}

// ---- OpenSSL plumbing -----------------------------------------------------

namespace {

struct PkeyFree {
	void operator()(EVP_PKEY *p) const {
		EVP_PKEY_free(p);
	}
};
using Pkey = std::unique_ptr<EVP_PKEY, PkeyFree>;

struct MdCtxFree {
	void operator()(EVP_MD_CTX *p) const {
		EVP_MD_CTX_free(p);
	}
};

struct BnFree {
	void operator()(BIGNUM *p) const {
		BN_free(p);
	}
};
using Bn = std::unique_ptr<BIGNUM, BnFree>;

Bn ToBn(const std::string &mag) {
	return Bn(BN_bin2bn((const unsigned char *)mag.data(), (int)mag.size(), nullptr));
}

// The OpenSSL group name and bit size for an ecdsa-sha2-* curve identifier.
bool CurveFor(const std::string &curve, const char *&group, int &bits, const EVP_MD *&md) {
	if (curve == "nistp256") {
		group = "P-256";
		bits = 256;
		md = EVP_sha256();
		return true;
	}
	if (curve == "nistp384") {
		group = "P-384";
		bits = 384;
		md = EVP_sha384();
		return true;
	}
	if (curve == "nistp521") {
		group = "P-521";
		bits = 521;
		md = EVP_sha512();
		return true;
	}
	return false;
}

Pkey FromParams(const char *alg, OSSL_PARAM_BLD *bld) {
	Pkey out;
	OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(bld);
	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(nullptr, alg, nullptr);
	EVP_PKEY *pkey = nullptr;
	if (params && ctx && EVP_PKEY_fromdata_init(ctx) == 1 &&
	    EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) == 1) {
		out.reset(pkey);
	}
	EVP_PKEY_CTX_free(ctx);
	OSSL_PARAM_free(params);
	return out;
}

// Parse a key blob into an OpenSSL key, filling in what PublicKey reports.
Pkey KeyFromBlob(const std::string &blob, PublicKey &info, std::string &err) {
	Reader r(blob);
	std::string type;
	if (!r.String(type)) {
		err = "truncated key";
		return nullptr;
	}
	info.type = type;
	info.blob = blob;
	if (type == "ssh-ed25519") {
		std::string pk;
		if (!r.String(pk) || !r.AtEnd() || pk.size() != 32) {
			err = "malformed ed25519 key";
			return nullptr;
		}
		info.bits = 256;
		Pkey k(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, (const unsigned char *)pk.data(), 32));
		if (!k) {
			err = "invalid ed25519 key";
		}
		return k;
	}
	if (type.rfind("ecdsa-sha2-", 0) == 0) {
		std::string curve, q;
		if (!r.String(curve) || !r.String(q) || !r.AtEnd() || type != "ecdsa-sha2-" + curve) {
			err = "malformed ECDSA key";
			return nullptr;
		}
		const char *group = nullptr;
		const EVP_MD *md = nullptr;
		if (!CurveFor(curve, group, info.bits, md)) {
			err = "unsupported ECDSA curve";
			return nullptr;
		}
		OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
		OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, group, 0);
		OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, q.data(), q.size());
		Pkey k = FromParams("EC", bld);
		OSSL_PARAM_BLD_free(bld);
		if (!k) {
			err = "invalid ECDSA key";
			return nullptr;
		}
		// fromdata does not insist the point is on the curve; this does.
		EVP_PKEY_CTX *chk = EVP_PKEY_CTX_new(k.get(), nullptr);
		bool ok = chk && EVP_PKEY_public_check(chk) == 1;
		EVP_PKEY_CTX_free(chk);
		if (!ok) {
			err = "invalid ECDSA key";
			return nullptr;
		}
		return k;
	}
	if (type == "ssh-rsa") {
		std::string e, n;
		if (!r.Mpint(e) || !r.Mpint(n) || !r.AtEnd() || e.empty() || n.empty()) {
			err = "malformed RSA key";
			return nullptr;
		}
		Bn bn_n = ToBn(n), bn_e = ToBn(e);
		info.bits = BN_num_bits(bn_n.get());
		if (info.bits < 2048) {
			err = "RSA keys must be at least 2048 bits";
			return nullptr;
		}
		if (info.bits > 16384) {
			err = "RSA key too large";
			return nullptr;
		}
		OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
		OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, bn_n.get());
		OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, bn_e.get());
		Pkey k = FromParams("RSA", bld);
		OSSL_PARAM_BLD_free(bld);
		if (!k) {
			err = "invalid RSA key";
		}
		return k;
	}
	err = "unsupported key type " + type;
	return nullptr;
}

bool DigestVerify(EVP_PKEY *key, const EVP_MD *md, const std::string &sig, const std::string &data) {
	std::unique_ptr<EVP_MD_CTX, MdCtxFree> ctx(EVP_MD_CTX_new());
	if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, md, nullptr, key) != 1) {
		return false;
	}
	return EVP_DigestVerify(ctx.get(), (const unsigned char *)sig.data(), sig.size(),
	                        (const unsigned char *)data.data(), data.size()) == 1;
}

int64_t Now() {
	return (int64_t)std::time(nullptr);
}

std::string Trim(const std::string &s) {
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) {
		return std::string();
	}
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

} // namespace

bool ParseKeyBlob(const std::string &blob, PublicKey &out, std::string &err) {
	PublicKey info;
	Pkey k = KeyFromBlob(blob, info, err);
	if (!k) {
		return false;
	}
	out = info;
	return true;
}

bool ParseAuthorizedKey(const std::string &line_in, PublicKey &out, std::string &err) {
	std::string line = Trim(line_in);
	// A pasted key often arrives wrapped; base64 has no whitespace in it.
	size_t sp1 = line.find_first_of(" \t");
	if (sp1 == std::string::npos) {
		err = "expected \"type key [comment]\"";
		return false;
	}
	std::string type = line.substr(0, sp1);
	if (type.find('=') != std::string::npos || type.find(',') != std::string::npos ||
	    (type.rfind("ssh-", 0) != 0 && type.rfind("ecdsa-", 0) != 0)) {
		err = "authorized_keys options are not supported; paste the key alone";
		return false;
	}
	std::string rest = Trim(line.substr(sp1 + 1));
	size_t sp2 = rest.find_first_of(" \t");
	std::string b64 = rest.substr(0, sp2);
	std::string comment = sp2 == std::string::npos ? std::string() : Trim(rest.substr(sp2 + 1));
	std::string blob;
	if (!util::Base64Decode(b64, blob) || blob.empty()) {
		err = "the key is not valid base64";
		return false;
	}
	PublicKey info;
	if (!ParseKeyBlob(blob, info, err)) {
		return false;
	}
	if (info.type != type) {
		err = "the key type does not match its encoding";
		return false;
	}
	// Control characters in a comment would end up in a web page and a log.
	for (char &c : comment) {
		if ((unsigned char)c < 0x20 || c == 0x7f) {
			c = ' ';
		}
	}
	if (comment.size() > 200) {
		comment.resize(200);
	}
	info.comment = comment;
	out = info;
	return true;
}

std::string Fingerprint(const std::string &blob) {
	std::string b64 = util::Base64Encode(util::Sha256Raw(blob));
	while (!b64.empty() && b64.back() == '=') {
		b64.pop_back();
	}
	return "SHA256:" + b64;
}

std::string AuthorizedKeyLine(const PublicKey &key) {
	std::string out = key.type + " " + util::Base64Encode(key.blob);
	if (!key.comment.empty()) {
		out += " " + key.comment;
	}
	return out;
}

std::vector<std::string> SignatureAlgorithms() {
	return {"ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521",
	        "rsa-sha2-512", "rsa-sha2-256"};
}

bool VerifySignature(const std::string &key_blob, const std::string &sig_blob, const std::string &data) {
	PublicKey info;
	std::string err;
	Pkey key = KeyFromBlob(key_blob, info, err);
	if (!key) {
		return false;
	}
	Reader r(sig_blob);
	std::string alg, sig;
	if (!r.String(alg) || !r.String(sig) || !r.AtEnd()) {
		return false;
	}
	if (info.type == "ssh-ed25519") {
		return alg == "ssh-ed25519" && sig.size() == 64 && DigestVerify(key.get(), nullptr, sig, data);
	}
	if (info.type == "ssh-rsa") {
		const EVP_MD *md = alg == "rsa-sha2-256" ? EVP_sha256() : alg == "rsa-sha2-512" ? EVP_sha512() : nullptr;
		// RFC 8332 §3: the signature is exactly the modulus length.
		if (!md || (int)sig.size() != (info.bits + 7) / 8) {
			return false;
		}
		return DigestVerify(key.get(), md, sig, data);
	}
	if (info.type.rfind("ecdsa-sha2-", 0) == 0) {
		if (alg != info.type) {
			return false;
		}
		const char *group = nullptr;
		const EVP_MD *md = nullptr;
		int bits = 0;
		if (!CurveFor(info.type.substr(11), group, bits, md)) {
			return false;
		}
		// RFC 5656 §3.1.2: the signature is (mpint r, mpint s); OpenSSL wants DER.
		Reader rs(sig);
		std::string rr, ss;
		if (!rs.Mpint(rr) || !rs.Mpint(ss) || !rs.AtEnd()) {
			return false;
		}
		ECDSA_SIG *esig = ECDSA_SIG_new();
		BIGNUM *br = BN_bin2bn((const unsigned char *)rr.data(), (int)rr.size(), nullptr);
		BIGNUM *bs = BN_bin2bn((const unsigned char *)ss.data(), (int)ss.size(), nullptr);
		if (!esig || !br || !bs || ECDSA_SIG_set0(esig, br, bs) != 1) {
			BN_free(br);
			BN_free(bs);
			ECDSA_SIG_free(esig);
			return false;
		}
		unsigned char *der = nullptr;
		int len = i2d_ECDSA_SIG(esig, &der);
		ECDSA_SIG_free(esig);
		if (len <= 0) {
			return false;
		}
		std::string der_sig((const char *)der, (size_t)len);
		OPENSSL_free(der);
		return DigestVerify(key.get(), md, der_sig, data);
	}
	return false;
}

// ---- host key ---------------------------------------------------------------

namespace {

bool PublicBlobOf(EVP_PKEY *pkey, std::string &blob) {
	unsigned char pub[32];
	size_t len = sizeof(pub);
	if (EVP_PKEY_get_raw_public_key(pkey, pub, &len) != 1 || len != 32) {
		return false;
	}
	blob = Writer().String("ssh-ed25519").String(std::string((const char *)pub, len)).Data();
	return true;
}

Pkey ReadPem(const std::string &pem) {
	BIO *bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
	if (!bio) {
		return nullptr;
	}
	Pkey k(PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr));
	BIO_free(bio);
	return k;
}

} // namespace

bool GenerateHostKey(HostKey &out, std::string &err) {
	Pkey k(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"));
	if (!k) {
		err = "could not generate an ed25519 key";
		return false;
	}
	BIO *bio = BIO_new(BIO_s_mem());
	if (!bio || PEM_write_bio_PrivateKey(bio, k.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
		BIO_free(bio);
		err = "could not encode the host key";
		return false;
	}
	char *data = nullptr;
	long len = BIO_get_mem_data(bio, &data);
	out.pem.assign(data, (size_t)len);
	BIO_free(bio);
	if (!PublicBlobOf(k.get(), out.blob)) {
		err = "could not read the host key back";
		return false;
	}
	return true;
}

bool LoadHostKey(const std::string &pem, HostKey &out, std::string &err) {
	Pkey k = ReadPem(pem);
	if (!k || EVP_PKEY_get_base_id(k.get()) != EVP_PKEY_ED25519) {
		err = "the host key must be an ed25519 private key in PEM (PKCS#8) form";
		return false;
	}
	out.pem = pem;
	if (!PublicBlobOf(k.get(), out.blob)) {
		err = "could not read the host key's public half";
		return false;
	}
	return true;
}

bool SignWithHostKey(const HostKey &key, const std::string &data, std::string &sig_blob) {
	Pkey k = ReadPem(key.pem);
	if (!k) {
		return false;
	}
	std::unique_ptr<EVP_MD_CTX, MdCtxFree> ctx(EVP_MD_CTX_new());
	unsigned char sig[64];
	size_t len = sizeof(sig);
	if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, k.get()) != 1 ||
	    EVP_DigestSign(ctx.get(), sig, &len, (const unsigned char *)data.data(), data.size()) != 1) {
		return false;
	}
	sig_blob = Writer().String("ssh-ed25519").String(std::string((const char *)sig, len)).Data();
	return true;
}

bool SiteHostKey(Connection &con, HostKey &out, std::string &err) {
	// Two first connections racing would each generate a key and one would
	// lose, leaving its client with a fingerprint that is gone on reconnect.
	static std::mutex mu;
	std::lock_guard<std::mutex> lock(mu);
	std::string pem = citadel::GetConfig(con, "qm_ssh_host_key", "");
	if (!pem.empty()) {
		return LoadHostKey(pem, out, err);
	}
	if (!GenerateHostKey(out, err)) {
		return false;
	}
	citadel::SetConfig(con, "qm_ssh_host_key", out.pem);
	return true;
}

bool ImportHostKey(Connection &con, const std::string &pem, std::string &err) {
	HostKey k;
	if (!LoadHostKey(pem, k, err)) {
		return false;
	}
	citadel::SetConfig(con, "qm_ssh_host_key", pem);
	return true;
}

// ---- the per-user key store ---------------------------------------------

namespace {

duckdb::unique_ptr<duckdb::QueryResult> Exec(Connection &con, const std::string &sql,
                                             duckdb::vector<Value> params) {
	auto stmt = con.Prepare(sql);
	if (stmt->HasError()) {
		return nullptr;
	}
	auto r = stmt->Execute(params, false);
	if (r->HasError()) {
		return nullptr;
	}
	return r;
}

int64_t Int(const Value &v) {
	return v.IsNull() ? 0 : v.GetValue<int64_t>();
}

} // namespace

void EnsureSchema(Connection &con) {
	// Keyed by fingerprint, which is a hash of the blob, so the same key cannot
	// be registered twice — to one user or to two.
	con.Query(R"(
		CREATE TABLE IF NOT EXISTS citadel_user_sshkeys (
			fingerprint VARCHAR PRIMARY KEY,
			username    VARCHAR NOT NULL,
			keytype     VARCHAR,
			key_blob    BLOB,
			bits        INTEGER,
			comment     VARCHAR,
			added_at    BIGINT,
			last_used   BIGINT
		)
	)");
}

bool AddKey(Connection &con, const std::string &username, const std::string &line, StoredKey &out,
            std::string &err) {
	PublicKey key;
	if (!ParseAuthorizedKey(line, key, err)) {
		return false;
	}
	std::string fp = Fingerprint(key.blob);
	auto existing = Exec(con, "SELECT username FROM citadel_user_sshkeys WHERE fingerprint = $1", {Value(fp)});
	if (existing) {
		auto &mat = existing->Cast<MaterializedQueryResult>();
		if (mat.RowCount() > 0) {
			err = util::Lower(mat.GetValue(0, 0).ToString()) == util::Lower(username)
			          ? "that key is already registered"
			          : "that key is registered to another user";
			return false;
		}
	}
	int64_t now = Now();
	if (!Exec(con,
	          "INSERT INTO citadel_user_sshkeys (fingerprint, username, keytype, key_blob, bits, comment, "
	          "added_at, last_used) VALUES ($1, $2, $3, $4, $5, $6, $7, 0)",
	          {Value(fp), Value(username), Value(key.type), Value::BLOB((duckdb::const_data_ptr_t)key.blob.data(),
	                                                                     key.blob.size()),
	           Value::INTEGER(key.bits), Value(key.comment), Value::BIGINT(now)})) {
		err = "could not store the key";
		return false;
	}
	out.username = username;
	out.type = key.type;
	out.fingerprint = fp;
	out.comment = key.comment;
	out.bits = key.bits;
	out.added_at = now;
	out.last_used = 0;
	return true;
}

std::vector<StoredKey> ListKeys(Connection &con, const std::string &username) {
	std::vector<StoredKey> out;
	auto r = Exec(con,
	              "SELECT username, keytype, fingerprint, comment, bits, added_at, last_used "
	              "FROM citadel_user_sshkeys WHERE lower(username) = lower($1) ORDER BY added_at, fingerprint",
	              {Value(username)});
	if (!r) {
		return out;
	}
	auto &mat = r->Cast<MaterializedQueryResult>();
	for (duckdb::idx_t i = 0; i < mat.RowCount(); i++) {
		StoredKey k;
		k.username = mat.GetValue(0, i).ToString();
		k.type = mat.GetValue(1, i).IsNull() ? "" : mat.GetValue(1, i).ToString();
		k.fingerprint = mat.GetValue(2, i).ToString();
		k.comment = mat.GetValue(3, i).IsNull() ? "" : mat.GetValue(3, i).ToString();
		k.bits = (int)Int(mat.GetValue(4, i));
		k.added_at = Int(mat.GetValue(5, i));
		k.last_used = Int(mat.GetValue(6, i));
		out.push_back(k);
	}
	return out;
}

bool RemoveKey(Connection &con, const std::string &username, const std::string &fingerprint) {
	auto r = Exec(con,
	              "DELETE FROM citadel_user_sshkeys WHERE lower(username) = lower($1) AND fingerprint = $2 "
	              "RETURNING fingerprint",
	              {Value(username), Value(fingerprint)});
	return r && r->Cast<MaterializedQueryResult>().RowCount() > 0;
}

void RemoveAllKeys(Connection &con, const std::string &username) {
	Exec(con, "DELETE FROM citadel_user_sshkeys WHERE lower(username) = lower($1)", {Value(username)});
}

bool UserHasKey(Connection &con, const std::string &username, const std::string &blob, bool touch) {
	std::string fp = Fingerprint(blob);
	auto r = Exec(con,
	              "SELECT count(*) FROM citadel_user_sshkeys WHERE fingerprint = $1 AND "
	              "lower(username) = lower($2)",
	              {Value(fp), Value(username)});
	if (!r || Int(r->Cast<MaterializedQueryResult>().GetValue(0, 0)) == 0) {
		return false;
	}
	if (touch) {
		Exec(con, "UPDATE citadel_user_sshkeys SET last_used = $1 WHERE fingerprint = $2",
		     {Value::BIGINT(Now()), Value(fp)});
	}
	return true;
}

} // namespace ssh
} // namespace quackmail
