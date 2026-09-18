#pragma once

// SSH public keys: the wire encoding (RFC 4251 §5), the key formats a user can
// register (RFC 4253 §6.6, RFC 5656, RFC 8709, RFC 8332), signature
// verification over them, and the per-user key store the SSH listener checks
// `publickey` authentication against.
//
// This is the half of SSH that more than one module needs: the telnet
// extension's SSH listener verifies signatures with it, the umbrella's
// `qm_sshkey_*` functions and the web preferences page manage the store, and
// the server's own host key (always ssh-ed25519) is signed with it. The
// transport — key exchange, packet cipher, channels — lives with the listener
// in telnet/src/, because nothing else speaks it.
//
// Everything cryptographic is OpenSSL 3 EVP. There is no SSH library in the
// tree: libssh2 is client-only, and a server library would be a new runtime
// dependency on every deploy host for what is, on the key side, a few hundred
// lines of parsing.

#include "duckdb.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace quackmail {
namespace ssh {

// ---- wire encoding --------------------------------------------------------

// Reads the RFC 4251 data types out of a byte string. Every read checks bounds
// and fails sticky: after one short read every later one fails too, so a parser
// can read a whole structure and test Ok() once at the end.
class Reader {
public:
	explicit Reader(const std::string &data) : data_(data) {
	}
	bool Byte(uint8_t &out);
	bool Bool(bool &out);
	bool U32(uint32_t &out);
	bool U64(uint64_t &out);
	bool String(std::string &out); // uint32 length + bytes
	bool Mpint(std::string &out);  // two's-complement big-endian, leading zero stripped
	bool NameList(std::vector<std::string> &out);
	bool Rest(std::string &out); // everything left
	bool Ok() const {
		return ok_;
	}
	bool AtEnd() const {
		return ok_ && pos_ == data_.size();
	}
	size_t Remaining() const {
		return ok_ ? data_.size() - pos_ : 0;
	}

private:
	bool Take(size_t n, std::string &out);
	const std::string &data_;
	size_t pos_ = 0;
	bool ok_ = true;
};

class Writer {
public:
	Writer &Byte(uint8_t v);
	Writer &Bool(bool v);
	Writer &U32(uint32_t v);
	Writer &U64(uint64_t v);
	Writer &String(const std::string &v);
	// `v` is an unsigned big-endian magnitude; the leading zero a set top bit
	// needs is added here.
	Writer &Mpint(const std::string &v);
	Writer &NameList(const std::vector<std::string> &v);
	Writer &Raw(const std::string &v);
	const std::string &Data() const {
		return out_;
	}

private:
	std::string out_;
};

std::string Join(const std::vector<std::string> &names); // "a,b,c"

// ---- public keys ----------------------------------------------------------

struct PublicKey {
	std::string type;    // "ssh-ed25519", "ecdsa-sha2-nistp256", "ssh-rsa", ...
	std::string blob;    // the wire encoding, which is also what is stored
	std::string comment; // from an authorized_keys line; not part of the key
	int bits = 0;        // RSA modulus size / curve size, for display

	PublicKey();
};

// One authorized_keys line: "type base64 [comment]". Options before the type
// (`from=`, `command=`, ...) are refused rather than silently dropped — a user
// pasting a restricted key would otherwise get an unrestricted one.
bool ParseAuthorizedKey(const std::string &line, PublicKey &out, std::string &err);

// A key blob off the wire (the `publickey` auth request). Same checks.
bool ParseKeyBlob(const std::string &blob, PublicKey &out, std::string &err);

// "SHA256:<unpadded base64>", what `ssh-keygen -l` prints.
std::string Fingerprint(const std::string &blob);

// The authorized_keys form of a key: "type base64 comment".
std::string AuthorizedKeyLine(const PublicKey &key);

// Verify an SSH signature blob (string alg, string sig) over `data` with the
// key in `key_blob`. The signature algorithm must be one this key type may use:
// ssh-rsa keys sign with rsa-sha2-256 or rsa-sha2-512 only — SHA-1 `ssh-rsa`
// signatures are refused, as OpenSSH itself has done since 8.8.
bool VerifySignature(const std::string &key_blob, const std::string &sig_blob, const std::string &data);

// The signature algorithms VerifySignature accepts, for server-sig-algs.
std::vector<std::string> SignatureAlgorithms();

// ---- the server's host key -----------------------------------------------

// An ed25519 private key, PKCS#8 PEM. Generated when the listener first needs
// one and kept in citadel_config, so the fingerprint a user has accepted
// survives a restart.
struct HostKey {
	std::string pem;
	std::string blob; // public key, wire encoding

	HostKey();
};

bool GenerateHostKey(HostKey &out, std::string &err);
bool LoadHostKey(const std::string &pem, HostKey &out, std::string &err);
// An ssh-ed25519 signature blob over `data`.
bool SignWithHostKey(const HostKey &key, const std::string &data, std::string &sig_blob);

// The site's host key: citadel_config `qm_ssh_host_key`, generated and stored on
// first call. Callers serialize among themselves; the listener calls this once
// per connection.
bool SiteHostKey(duckdb::Connection &con, HostKey &out, std::string &err);
// Replace it with an operator-supplied PEM (an ed25519 key only).
bool ImportHostKey(duckdb::Connection &con, const std::string &pem, std::string &err);

// ---- the per-user key store ---------------------------------------------

struct StoredKey {
	std::string username;
	std::string type;
	std::string fingerprint;
	std::string comment;
	int bits = 0;
	int64_t added_at = 0;
	int64_t last_used = 0; // 0 = never

	StoredKey();
};

void EnsureSchema(duckdb::Connection &con);

// Add a key from an authorized_keys line. The same key on a second user is
// refused: a key is one person's credential, and sharing one would make the
// login ambiguous.
bool AddKey(duckdb::Connection &con, const std::string &username, const std::string &line, StoredKey &out,
            std::string &err);
std::vector<StoredKey> ListKeys(duckdb::Connection &con, const std::string &username);
bool RemoveKey(duckdb::Connection &con, const std::string &username, const std::string &fingerprint);
void RemoveAllKeys(duckdb::Connection &con, const std::string &username);
// Is this key registered to this user? Also stamps last_used when it is.
bool UserHasKey(duckdb::Connection &con, const std::string &username, const std::string &blob,
                bool touch = false);

} // namespace ssh
} // namespace quackmail
