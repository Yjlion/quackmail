#include "ssh_transport.hpp"

#include "quackmail/util.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace quackmail {
namespace sshd {

using ssh::Reader;
using ssh::Writer;

namespace {

// Larger than the 35000 bytes RFC 4253 §6.1 requires a server to accept, and
// small enough that a hostile length field cannot make us allocate much.
constexpr uint32_t kMaxPacket = 256 * 1024;

const char *kServerVersion = "SSH-2.0-QuackCit_1.0";

uint32_t Be32(const std::string &s, size_t at = 0) {
	return ((uint32_t)(uint8_t)s[at] << 24) | ((uint32_t)(uint8_t)s[at + 1] << 16) |
	       ((uint32_t)(uint8_t)s[at + 2] << 8) | (uint32_t)(uint8_t)s[at + 3];
}

std::string U32Bytes(uint32_t v) {
	return Writer().U32(v).Data();
}

std::string Random(size_t n) {
	std::string out(n, '\0');
	RAND_bytes((unsigned char *)&out[0], (int)n);
	return out;
}

struct CipherCtxFree {
	void operator()(EVP_CIPHER_CTX *c) const {
		EVP_CIPHER_CTX_free(c);
	}
};
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxFree>;

// ---- the ciphers ------------------------------------------------------------

class NoneCipher : public PacketCipher {
public:
	size_t FirstRead() const override {
		return 4;
	}
	bool Length(uint32_t, const std::string &first, uint32_t &len) override {
		len = Be32(first);
		return true;
	}
	size_t RestRead(uint32_t len) const override {
		return len;
	}
	bool Open(uint32_t, const std::string &, const std::string &rest, std::string &plain) override {
		plain = rest;
		return true;
	}
	std::string Seal(uint32_t, const std::string &plain) override {
		return U32Bytes((uint32_t)plain.size()) + plain;
	}
	size_t BlockSize() const override {
		return 8;
	}
	bool LengthIsAad() const override {
		return false;
	}
};

// aes128-gcm@openssh.com / aes256-gcm@openssh.com (RFC 5647 as OpenSSH uses
// it): the length is additional data, the 12-byte nonce is a 4-byte fixed
// field plus an 8-byte counter bumped after every packet.
class AesGcm : public PacketCipher {
public:
	AesGcm(const std::string &key, const std::string &iv, bool encrypt)
	    : key_(key), iv_(iv), encrypt_(encrypt),
	      cipher_(key.size() == 32 ? EVP_aes_256_gcm() : EVP_aes_128_gcm()) {
	}
	size_t FirstRead() const override {
		return 4;
	}
	bool Length(uint32_t, const std::string &first, uint32_t &len) override {
		len = Be32(first);
		return len % 16 == 0 && len >= 16;
	}
	size_t RestRead(uint32_t len) const override {
		return (size_t)len + 16;
	}
	bool Open(uint32_t, const std::string &first, const std::string &rest, std::string &plain) override {
		size_t n = rest.size() - 16;
		CipherCtx ctx(EVP_CIPHER_CTX_new());
		int outl = 0;
		plain.assign(n, '\0');
		bool ok = ctx && EVP_DecryptInit_ex(ctx.get(), cipher_, nullptr, nullptr, nullptr) == 1 &&
		          EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
		          EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, (const unsigned char *)key_.data(),
		                             (const unsigned char *)iv_.data()) == 1 &&
		          EVP_DecryptUpdate(ctx.get(), nullptr, &outl, (const unsigned char *)first.data(), 4) == 1 &&
		          EVP_DecryptUpdate(ctx.get(), (unsigned char *)&plain[0], &outl,
		                            (const unsigned char *)rest.data(), (int)n) == 1 &&
		          EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, (void *)(rest.data() + n)) == 1 &&
		          EVP_DecryptFinal_ex(ctx.get(), nullptr, &outl) == 1;
		Bump();
		return ok;
	}
	std::string Seal(uint32_t, const std::string &plain) override {
		std::string len = U32Bytes((uint32_t)plain.size());
		std::string ct(plain.size(), '\0');
		unsigned char tag[16];
		CipherCtx ctx(EVP_CIPHER_CTX_new());
		int outl = 0;
		EVP_EncryptInit_ex(ctx.get(), cipher_, nullptr, nullptr, nullptr);
		EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
		EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, (const unsigned char *)key_.data(),
		                   (const unsigned char *)iv_.data());
		EVP_EncryptUpdate(ctx.get(), nullptr, &outl, (const unsigned char *)len.data(), 4);
		EVP_EncryptUpdate(ctx.get(), (unsigned char *)&ct[0], &outl, (const unsigned char *)plain.data(),
		                  (int)plain.size());
		EVP_EncryptFinal_ex(ctx.get(), nullptr, &outl);
		EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, tag);
		Bump();
		return len + ct + std::string((const char *)tag, 16);
	}
	size_t BlockSize() const override {
		return 16;
	}
	bool LengthIsAad() const override {
		return true;
	}

private:
	void Bump() {
		for (int i = 11; i >= 4; i--) {
			if (++iv_[(size_t)i] != 0) {
				break;
			}
		}
	}
	std::string key_;
	std::string iv_;
	bool encrypt_;
	const EVP_CIPHER *cipher_;
};

// chacha20-poly1305@openssh.com (OpenSSH PROTOCOL.chacha20poly1305). Two
// ChaCha20 keys: the second 32 bytes encrypt the length, the first 32 the
// payload, and the Poly1305 key is the first 32 bytes of the payload key's
// block 0. The nonce is the sequence number; OpenSSL's 16-byte ChaCha20 IV is
// laid out as the 64-bit block counter then that 64-bit nonce, which is the
// original (DJB) variant OpenSSH uses.
class ChaChaPoly : public PacketCipher {
public:
	explicit ChaChaPoly(const std::string &key) : main_(key.substr(0, 32)), header_(key.substr(32, 32)) {
	}
	size_t FirstRead() const override {
		return 4;
	}
	bool Length(uint32_t seq, const std::string &first, uint32_t &len) override {
		std::string plain = Stream(header_, seq, 0, first);
		len = Be32(plain);
		return len % 8 == 0 && len >= 8;
	}
	size_t RestRead(uint32_t len) const override {
		return (size_t)len + 16;
	}
	bool Open(uint32_t seq, const std::string &first, const std::string &rest, std::string &plain) override {
		size_t n = rest.size() - 16;
		std::string ct = rest.substr(0, n);
		std::string tag = Tag(seq, first + ct);
		if (tag.size() != 16 || CRYPTO_memcmp(tag.data(), rest.data() + n, 16) != 0) {
			return false;
		}
		plain = Stream(main_, seq, 1, ct);
		return true;
	}
	std::string Seal(uint32_t seq, const std::string &plain) override {
		std::string len = Stream(header_, seq, 0, U32Bytes((uint32_t)plain.size()));
		std::string ct = Stream(main_, seq, 1, plain);
		return len + ct + Tag(seq, len + ct);
	}
	size_t BlockSize() const override {
		return 8;
	}
	bool LengthIsAad() const override {
		return true;
	}

private:
	static std::string Stream(const std::string &key, uint32_t seq, uint32_t counter, const std::string &in) {
		unsigned char iv[16] = {0};
		iv[0] = (unsigned char)counter;
		iv[1] = (unsigned char)(counter >> 8);
		iv[2] = (unsigned char)(counter >> 16);
		iv[3] = (unsigned char)(counter >> 24);
		// iv[8..15] = the sequence number as a big-endian uint64.
		iv[12] = (unsigned char)(seq >> 24);
		iv[13] = (unsigned char)(seq >> 16);
		iv[14] = (unsigned char)(seq >> 8);
		iv[15] = (unsigned char)seq;
		std::string out(in.size(), '\0');
		CipherCtx ctx(EVP_CIPHER_CTX_new());
		int outl = 0;
		EVP_EncryptInit_ex(ctx.get(), EVP_chacha20(), nullptr, (const unsigned char *)key.data(), iv);
		if (!in.empty()) {
			EVP_EncryptUpdate(ctx.get(), (unsigned char *)&out[0], &outl, (const unsigned char *)in.data(),
			                  (int)in.size());
		}
		return out;
	}
	std::string Tag(uint32_t seq, const std::string &data) {
		std::string poly_key = Stream(main_, seq, 0, std::string(32, '\0'));
		EVP_MAC *mac = EVP_MAC_fetch(nullptr, "POLY1305", nullptr);
		EVP_MAC_CTX *ctx = mac ? EVP_MAC_CTX_new(mac) : nullptr;
		unsigned char out[16];
		size_t outl = 0;
		bool ok = ctx && EVP_MAC_init(ctx, (const unsigned char *)poly_key.data(), 32, nullptr) == 1 &&
		          EVP_MAC_update(ctx, (const unsigned char *)data.data(), data.size()) == 1 &&
		          EVP_MAC_final(ctx, out, &outl, sizeof(out)) == 1;
		EVP_MAC_CTX_free(ctx);
		EVP_MAC_free(mac);
		OPENSSL_cleanse(&poly_key[0], poly_key.size());
		return ok ? std::string((const char *)out, outl) : std::string();
	}
	std::string main_;
	std::string header_;
};

// aes128-ctr / aes256-ctr with an HMAC-SHA2 MAC, encrypt-and-MAC (RFC 4253
// §6.4) or encrypt-then-MAC (the -etm@openssh.com names). Only for clients that
// have no AEAD; the counter runs on across packets, so one context lives for the
// whole key epoch.
class AesCtrHmac : public PacketCipher {
public:
	AesCtrHmac(const std::string &key, const std::string &iv, const std::string &mac_key, const EVP_MD *md,
	           bool etm, bool encrypt)
	    : mac_key_(mac_key), md_(md), mac_len_((size_t)EVP_MD_get_size(md)), etm_(etm), ctx_(EVP_CIPHER_CTX_new()) {
		const EVP_CIPHER *c = key.size() == 32 ? EVP_aes_256_ctr() : EVP_aes_128_ctr();
		if (encrypt) {
			EVP_EncryptInit_ex(ctx_.get(), c, nullptr, (const unsigned char *)key.data(),
			                   (const unsigned char *)iv.data());
		} else {
			EVP_DecryptInit_ex(ctx_.get(), c, nullptr, (const unsigned char *)key.data(),
			                   (const unsigned char *)iv.data());
		}
	}
	size_t FirstRead() const override {
		return etm_ ? 4 : 16;
	}
	bool Length(uint32_t, const std::string &first, uint32_t &len) override {
		if (etm_) {
			len = Be32(first);
			return len % 16 == 0 && len >= 16;
		}
		first_plain_ = Crypt(first);
		len = Be32(first_plain_);
		return (len + 4) % 16 == 0 && len + 4 >= 16;
	}
	size_t RestRead(uint32_t len) const override {
		return (etm_ ? (size_t)len : (size_t)len + 4 - 16) + mac_len_;
	}
	bool Open(uint32_t seq, const std::string &first, const std::string &rest, std::string &plain) override {
		size_t n = rest.size() - mac_len_;
		std::string body = rest.substr(0, n);
		if (etm_) {
			std::string mac = Mac(seq, first + body);
			if (CRYPTO_memcmp(mac.data(), rest.data() + n, mac_len_) != 0) {
				return false;
			}
			plain = Crypt(body);
			return true;
		}
		std::string full = first_plain_ + Crypt(body);
		std::string mac = Mac(seq, full);
		if (CRYPTO_memcmp(mac.data(), rest.data() + n, mac_len_) != 0) {
			return false;
		}
		plain = full.substr(4);
		return true;
	}
	std::string Seal(uint32_t seq, const std::string &plain) override {
		std::string len = U32Bytes((uint32_t)plain.size());
		if (etm_) {
			std::string ct = Crypt(plain);
			return len + ct + Mac(seq, len + ct);
		}
		std::string full = len + plain;
		return Crypt(full) + Mac(seq, full);
	}
	size_t BlockSize() const override {
		return 16;
	}
	bool LengthIsAad() const override {
		return etm_;
	}

private:
	std::string Crypt(const std::string &in) {
		std::string out(in.size(), '\0');
		int outl = 0;
		if (!in.empty()) {
			EVP_CipherUpdate(ctx_.get(), (unsigned char *)&out[0], &outl, (const unsigned char *)in.data(),
			                 (int)in.size());
		}
		return out;
	}
	std::string Mac(uint32_t seq, const std::string &data) {
		std::string msg = U32Bytes(seq) + data;
		unsigned char out[EVP_MAX_MD_SIZE];
		unsigned int outl = 0;
		HMAC(md_, mac_key_.data(), (int)mac_key_.size(), (const unsigned char *)msg.data(), msg.size(), out,
		     &outl);
		return std::string((const char *)out, outl);
	}
	std::string mac_key_;
	const EVP_MD *md_;
	size_t mac_len_;
	bool etm_;
	CipherCtx ctx_;
	std::string first_plain_;
};

// ---- algorithm tables -------------------------------------------------------

const std::vector<std::string> kKex = {"curve25519-sha256", "curve25519-sha256@libssh.org"};
const std::vector<std::string> kHostKey = {"ssh-ed25519"};
const std::vector<std::string> kCiphers = {"chacha20-poly1305@openssh.com", "aes256-gcm@openssh.com",
                                           "aes128-gcm@openssh.com", "aes256-ctr", "aes128-ctr"};
const std::vector<std::string> kMacs = {"hmac-sha2-256-etm@openssh.com", "hmac-sha2-512-etm@openssh.com",
                                        "hmac-sha2-256", "hmac-sha2-512"};
const std::vector<std::string> kComp = {"none"};

bool IsAead(const std::string &cipher) {
	return cipher == "chacha20-poly1305@openssh.com" || cipher.find("-gcm@") != std::string::npos;
}

// RFC 4253 §7.1: the first algorithm on the *client's* list that the server
// also supports.
std::string Choose(const std::vector<std::string> &client, const std::vector<std::string> &server) {
	for (auto &c : client) {
		for (auto &s : server) {
			if (c == s) {
				return c;
			}
		}
	}
	return std::string();
}

size_t KeyLen(const std::string &cipher) {
	if (cipher == "chacha20-poly1305@openssh.com") {
		return 64;
	}
	return cipher.find("256") != std::string::npos ? 32 : 16;
}

size_t IvLen(const std::string &cipher) {
	if (cipher == "chacha20-poly1305@openssh.com") {
		return 0;
	}
	return IsAead(cipher) ? 12 : 16;
}

const EVP_MD *MacMd(const std::string &mac) {
	return mac.find("512") != std::string::npos ? EVP_sha512() : EVP_sha256();
}

std::unique_ptr<PacketCipher> MakeCipher(const std::string &cipher, const std::string &mac, const std::string &key,
                                         const std::string &iv, const std::string &mac_key, bool encrypt) {
	if (cipher == "chacha20-poly1305@openssh.com") {
		return std::unique_ptr<PacketCipher>(new ChaChaPoly(key));
	}
	if (IsAead(cipher)) {
		return std::unique_ptr<PacketCipher>(new AesGcm(key, iv, encrypt));
	}
	bool etm = mac.find("-etm@") != std::string::npos;
	return std::unique_ptr<PacketCipher>(new AesCtrHmac(key, iv, mac_key, MacMd(mac), etm, encrypt));
}

std::string Sha256(const std::string &data) {
	unsigned char out[32];
	EVP_Digest(data.data(), data.size(), out, nullptr, EVP_sha256(), nullptr);
	return std::string((const char *)out, 32);
}

// RFC 4253 §7.2: HASH(K || H || X || session_id), extended by HASH(K || H ||
// everything so far) until long enough.
std::string DeriveKey(const std::string &k_mpint, const std::string &h, char letter, const std::string &sid,
                      size_t need) {
	std::string out = Sha256(k_mpint + h + std::string(1, letter) + sid);
	while (out.size() < need) {
		out += Sha256(k_mpint + h + out);
	}
	out.resize(need);
	return out;
}

struct PkeyFree {
	void operator()(EVP_PKEY *p) const {
		EVP_PKEY_free(p);
	}
};

} // namespace

// ---- Transport --------------------------------------------------------------

Transport::Transport(int fd, const ssh::HostKey &host_key)
    : fd_(fd), host_key_(host_key), server_version_(kServerVersion), in_(new NoneCipher()),
      out_(new NoneCipher()) {
}

Transport::~Transport() {
}

bool Transport::Fail(const std::string &why) {
	if (error_.empty()) {
		error_ = why;
	}
	dead_ = true;
	return false;
}

bool Transport::ReadExact(size_t n, std::string &out, int timeout_ms) {
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
	while (rbuf_.size() - rpos_ < n) {
		if (rpos_ > 0 && rpos_ == rbuf_.size()) {
			rbuf_.clear();
			rpos_ = 0;
		}
		int wait = -1;
		if (timeout_ms >= 0) {
			auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
			                                                                  std::chrono::steady_clock::now())
			                .count();
			if (left <= 0) {
				return Fail("timeout");
			}
			wait = (int)left;
		}
		struct pollfd pfd {};
		pfd.fd = fd_;
		pfd.events = POLLIN;
		int pr = ::poll(&pfd, 1, wait);
		if (pr < 0 && errno == EINTR) {
			continue;
		}
		if (pr <= 0) {
			return Fail(pr == 0 ? "timeout" : "poll failed");
		}
		char buf[16384];
		ssize_t got = ::recv(fd_, buf, sizeof(buf), 0);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			return Fail("connection closed");
		}
		rbuf_.append(buf, (size_t)got);
	}
	out.assign(rbuf_, rpos_, n);
	rpos_ += n;
	if (rpos_ == rbuf_.size()) {
		rbuf_.clear();
		rpos_ = 0;
	}
	return true;
}

bool Transport::WriteAll(const std::string &bytes) {
	size_t off = 0;
	while (off < bytes.size()) {
		ssize_t n = ::send(fd_, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return Fail("write failed");
		}
		off += (size_t)n;
	}
	return true;
}

bool Transport::ReadPacket(std::string &payload, int timeout_ms) {
	if (dead_) {
		return false;
	}
	std::string first, rest, plain;
	uint32_t len = 0;
	if (!ReadExact(in_->FirstRead(), first, timeout_ms)) {
		return false;
	}
	if (!in_->Length(in_seq_, first, len) || len > kMaxPacket || len < 5) {
		return Fail("bad packet length");
	}
	// Once the length is known the rest is already on its way; a stall here is
	// a dead peer, not an idle one.
	if (!ReadExact(in_->RestRead(len), rest, 60000)) {
		return false;
	}
	if (!in_->Open(in_seq_, first, rest, plain)) {
		Disconnect(MAC_ERROR, "corrupted packet");
		return Fail("packet authentication failed");
	}
	in_seq_++;
	if (plain.empty()) {
		return Fail("empty packet");
	}
	uint8_t padlen = (uint8_t)plain[0];
	if (padlen < 4 || (size_t)padlen + 1 >= plain.size()) {
		return Fail("bad padding");
	}
	payload = plain.substr(1, plain.size() - 1 - padlen);
	if (payload.empty()) {
		return Fail("empty payload");
	}
	return true;
}

bool Transport::WritePacket(const std::string &payload) {
	if (dead_) {
		return false;
	}
	size_t bs = out_->BlockSize();
	size_t aad = out_->LengthIsAad() ? 4 : 0;
	size_t base = 4 + 1 + payload.size();
	size_t padlen = bs - ((base - aad) % bs);
	if (padlen < 4) {
		padlen += bs;
	}
	std::string plain;
	plain.reserve(1 + payload.size() + padlen);
	plain.push_back((char)padlen);
	plain += payload;
	plain += Random(padlen);
	std::string wire = out_->Seal(out_seq_, plain);
	out_seq_++;
	return WriteAll(wire);
}

std::string Transport::BuildKexInit() {
	std::vector<std::string> kex = kKex;
	if (!first_kex_done_) {
		// Only in the first KEXINIT: the marker says "this server does strict
		// kex", and a re-exchange must not advertise it again.
		kex.push_back("kex-strict-s-v00@openssh.com");
	}
	Writer w;
	w.Byte(KEXINIT).Raw(Random(16));
	w.NameList(kex).NameList(kHostKey);
	w.NameList(kCiphers).NameList(kCiphers);
	w.NameList(kMacs).NameList(kMacs);
	w.NameList(kComp).NameList(kComp);
	w.NameList({}).NameList({});
	w.Bool(false).U32(0);
	return w.Data();
}

bool Transport::KeyExchange(const std::string &their_kexinit_in) {
	std::string mine = BuildKexInit();
	if (!WritePacket(mine)) {
		return false;
	}
	std::string theirs = their_kexinit_in;
	while (theirs.empty()) {
		std::string p;
		if (!ReadPacket(p, 60000)) {
			return false;
		}
		if ((uint8_t)p[0] == KEXINIT) {
			theirs = p;
		} else if (!first_kex_done_ || strict_kex_) {
			// Strict kex (and simple sense, on the first exchange): nothing but
			// the exchange itself may arrive before NEWKEYS. This is precisely
			// the injected IGNORE the Terrapin attack depends on.
			Disconnect(PROTOCOL_ERROR, "unexpected message during key exchange");
			return Fail("unexpected message during key exchange");
		}
	}

	Reader r(theirs);
	uint8_t msg = 0;
	std::string cookie;
	std::vector<std::string> c_kex, c_host, c_enc_cs, c_enc_sc, c_mac_cs, c_mac_sc, c_comp_cs, c_comp_sc, c_lang1,
	    c_lang2;
	bool follows = false;
	uint32_t reserved = 0;
	r.Byte(msg);
	std::string cookie_raw;
	for (int i = 0; i < 16; i++) {
		uint8_t b;
		r.Byte(b);
	}
	r.NameList(c_kex);
	r.NameList(c_host);
	r.NameList(c_enc_cs);
	r.NameList(c_enc_sc);
	r.NameList(c_mac_cs);
	r.NameList(c_mac_sc);
	r.NameList(c_comp_cs);
	r.NameList(c_comp_sc);
	r.NameList(c_lang1);
	r.NameList(c_lang2);
	r.Bool(follows);
	r.U32(reserved);
	if (!r.Ok()) {
		return Fail("malformed KEXINIT");
	}
	if (!first_kex_done_) {
		for (auto &k : c_kex) {
			if (k == "kex-strict-c-v00@openssh.com") {
				strict_kex_ = true;
			}
			if (k == "ext-info-c") {
				client_ext_info_ = true;
			}
		}
		if (strict_kex_ && in_seq_ != 1) {
			// The client's KEXINIT must have been the very first packet.
			Disconnect(PROTOCOL_ERROR, "strict kex: KEXINIT was not the first packet");
			return Fail("strict kex violation");
		}
	}

	std::string kex = Choose(c_kex, kKex);
	std::string host = Choose(c_host, kHostKey);
	std::string enc_cs = Choose(c_enc_cs, kCiphers);
	std::string enc_sc = Choose(c_enc_sc, kCiphers);
	std::string mac_cs = IsAead(enc_cs) ? std::string() : Choose(c_mac_cs, kMacs);
	std::string mac_sc = IsAead(enc_sc) ? std::string() : Choose(c_mac_sc, kMacs);
	std::string comp_cs = Choose(c_comp_cs, kComp);
	std::string comp_sc = Choose(c_comp_sc, kComp);
	if (kex.empty() || host.empty() || enc_cs.empty() || enc_sc.empty() || comp_cs.empty() || comp_sc.empty() ||
	    (!IsAead(enc_cs) && mac_cs.empty()) || (!IsAead(enc_sc) && mac_sc.empty())) {
		Disconnect(KEY_EXCHANGE_FAILED, "no matching algorithm");
		return Fail("no common algorithm with the client");
	}
	// A client that guessed its first kex packet and guessed wrong has sent a
	// packet we must throw away (RFC 4253 §7).
	bool discard_guess = follows && (c_kex.empty() || c_kex[0] != kex || c_host.empty() || c_host[0] != host);

	std::string init;
	while (true) {
		if (!ReadPacket(init, 60000)) {
			return false;
		}
		if (discard_guess) {
			discard_guess = false;
			continue;
		}
		break;
	}
	Reader ir(init);
	std::string q_c;
	ir.Byte(msg);
	ir.String(q_c);
	if (!ir.Ok() || msg != KEX_ECDH_INIT || q_c.size() != 32) {
		Disconnect(KEY_EXCHANGE_FAILED, "expected KEX_ECDH_INIT");
		return Fail("bad KEX_ECDH_INIT");
	}

	// X25519: our ephemeral key, the client's public value, the shared secret.
	std::unique_ptr<EVP_PKEY, PkeyFree> eph(EVP_PKEY_Q_keygen(nullptr, nullptr, "X25519"));
	std::unique_ptr<EVP_PKEY, PkeyFree> peer(
	    EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, (const unsigned char *)q_c.data(), 32));
	if (!eph || !peer) {
		return Fail("X25519 setup failed");
	}
	unsigned char q_s_raw[32];
	size_t q_s_len = 32;
	EVP_PKEY_get_raw_public_key(eph.get(), q_s_raw, &q_s_len);
	std::string q_s((const char *)q_s_raw, 32);
	std::string shared(32, '\0');
	size_t shared_len = 32;
	EVP_PKEY_CTX *dctx = EVP_PKEY_CTX_new(eph.get(), nullptr);
	bool derived = dctx && EVP_PKEY_derive_init(dctx) == 1 && EVP_PKEY_derive_set_peer(dctx, peer.get()) == 1 &&
	               EVP_PKEY_derive(dctx, (unsigned char *)&shared[0], &shared_len) == 1;
	EVP_PKEY_CTX_free(dctx);
	if (!derived || shared == std::string(32, '\0')) {
		Disconnect(KEY_EXCHANGE_FAILED, "key agreement failed");
		return Fail("X25519 derive failed");
	}
	// RFC 8731 §3.1: the secret is interpreted as a big-endian integer and sent
	// as an mpint.
	std::string k_mpint = Writer().Mpint(shared).Data();
	OPENSSL_cleanse(&shared[0], shared.size());

	std::string h = Sha256(Writer()
	                           .String(client_version_)
	                           .String(server_version_)
	                           .String(theirs)
	                           .String(mine)
	                           .String(host_key_.blob)
	                           .String(q_c)
	                           .String(q_s)
	                           .Raw(k_mpint)
	                           .Data());
	if (session_id_.empty()) {
		session_id_ = h;
	}
	std::string sig;
	if (!ssh::SignWithHostKey(host_key_, h, sig)) {
		return Fail("could not sign with the host key");
	}
	if (!WritePacket(Writer().Byte(KEX_ECDH_REPLY).String(host_key_.blob).String(q_s).String(sig).Data())) {
		return false;
	}

	// Keys for both directions, then NEWKEYS each way. Ours switches as soon as
	// our NEWKEYS is out; theirs when theirs arrives.
	auto key = [&](char letter, size_t n) { return n == 0 ? std::string() : DeriveKey(k_mpint, h, letter, session_id_, n); };
	std::string iv_cs = key('A', IvLen(enc_cs)), iv_sc = key('B', IvLen(enc_sc));
	std::string ek_cs = key('C', KeyLen(enc_cs)), ek_sc = key('D', KeyLen(enc_sc));
	std::string mk_cs = mac_cs.empty() ? std::string() : key('E', (size_t)EVP_MD_get_size(MacMd(mac_cs)));
	std::string mk_sc = mac_sc.empty() ? std::string() : key('F', (size_t)EVP_MD_get_size(MacMd(mac_sc)));

	if (!WritePacket(std::string(1, (char)NEWKEYS))) {
		return false;
	}
	out_ = MakeCipher(enc_sc, mac_sc, ek_sc, iv_sc, mk_sc, true);
	if (strict_kex_) {
		out_seq_ = 0;
	}

	std::string nk;
	if (!ReadPacket(nk, 60000)) {
		return false;
	}
	if ((uint8_t)nk[0] != NEWKEYS) {
		Disconnect(PROTOCOL_ERROR, "expected NEWKEYS");
		return Fail("expected NEWKEYS");
	}
	in_ = MakeCipher(enc_cs, mac_cs, ek_cs, iv_cs, mk_cs, false);
	if (strict_kex_) {
		in_seq_ = 0;
	}
	OPENSSL_cleanse(&k_mpint[0], k_mpint.size());
	first_kex_done_ = true;
	return true;
}

bool Transport::Handshake() {
	if (!WriteAll(server_version_ + "\r\n")) {
		return false;
	}
	// RFC 4253 §4.2: the server may see other lines before the identification
	// string; only the one starting "SSH-" counts. Bounded, so a peer that
	// talks forever without identifying itself is let go.
	for (int lines = 0; lines < 50; lines++) {
		std::string line;
		while (true) {
			std::string c;
			if (!ReadExact(1, c, 30000)) {
				return false;
			}
			if (c[0] == '\n') {
				break;
			}
			line += c;
			if (line.size() > 255) {
				return Fail("identification string too long");
			}
		}
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.rfind("SSH-", 0) == 0) {
			if (line.rfind("SSH-2.0-", 0) != 0 && line.rfind("SSH-1.99-", 0) != 0) {
				WriteAll("Protocol mismatch.\r\n");
				return Fail("not SSH-2.0");
			}
			client_version_ = line;
			break;
		}
	}
	if (client_version_.empty()) {
		return Fail("no identification string");
	}
	if (!KeyExchange(std::string())) {
		return false;
	}
	if (client_ext_info_) {
		// RFC 8308: tell the client which signature algorithms publickey auth
		// may use, or an RSA key is offered as SHA-1 `ssh-rsa` and refused.
		Write(Writer()
		          .Byte(EXT_INFO)
		          .U32(1)
		          .String("server-sig-algs")
		          .String(ssh::Join(ssh::SignatureAlgorithms()))
		          .Data());
	}
	return !dead_;
}

bool Transport::Read(std::string &payload, int timeout_ms) {
	while (true) {
		if (!ReadPacket(payload, timeout_ms)) {
			return false;
		}
		uint8_t msg = (uint8_t)payload[0];
		switch (msg) {
		case IGNORE:
		case DEBUG:
		case UNIMPLEMENTED:
			continue;
		case DISCONNECT:
			return Fail("client disconnected");
		case KEXINIT:
			if (!KeyExchange(payload)) {
				return false;
			}
			continue;
		default:
			return true;
		}
	}
}

bool Transport::Write(const std::string &payload) {
	return WritePacket(payload);
}

void Transport::Disconnect(uint32_t reason, const std::string &text) {
	if (dead_) {
		return;
	}
	WritePacket(Writer().Byte(DISCONNECT).U32(reason).String(text).String("").Data());
	dead_ = true;
}

} // namespace sshd
} // namespace quackmail
