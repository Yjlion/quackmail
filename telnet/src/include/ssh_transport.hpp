#pragma once

// The SSH transport layer (RFC 4253): version exchange, binary packets, key
// exchange and the packet ciphers. Server side only.
//
// Written on OpenSSL 3 rather than on an SSH library: the only one on a typical
// build host is libssh2, which is client-only, and a server library would be a
// new runtime dependency on every deploy host. The algorithm set is small and
// modern on purpose, which is what keeps this a few hundred lines:
//
//   kex        curve25519-sha256 (and its @libssh.org name), with
//              kex-strict-s-v00@openssh.com — the Terrapin countermeasure
//   host key   ssh-ed25519
//   ciphers    chacha20-poly1305@openssh.com, aes256-gcm@openssh.com,
//              aes128-gcm@openssh.com, and aes256-ctr / aes128-ctr for clients
//              without an AEAD (paramiko, older JSch) — those with
//   MACs       hmac-sha2-256-etm@openssh.com, hmac-sha2-512-etm@openssh.com,
//              hmac-sha2-256, hmac-sha2-512
//   compression none
//
// No SHA-1 anywhere, no CBC, no diffie-hellman-group*: every client this has to
// serve (OpenSSH since 6.5, PuTTY since 0.68, WinSCP, FileZilla, paramiko)
// offers something from each list above.
//
// Everything runs on the connection's own thread: the server reads a packet,
// acts on it, and writes its replies, so a key re-exchange the client starts
// mid-session is handled in line rather than having to fence off a writer on
// another thread.

#include "quackmail/sshkeys.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace quackmail {
namespace sshd {

// SSH message numbers used here (RFC 4250 §4.1).
enum Msg : uint8_t {
	DISCONNECT = 1,
	IGNORE = 2,
	UNIMPLEMENTED = 3,
	DEBUG = 4,
	SERVICE_REQUEST = 5,
	SERVICE_ACCEPT = 6,
	EXT_INFO = 7,
	KEXINIT = 20,
	NEWKEYS = 21,
	KEX_ECDH_INIT = 30,
	KEX_ECDH_REPLY = 31,
	USERAUTH_REQUEST = 50,
	USERAUTH_FAILURE = 51,
	USERAUTH_SUCCESS = 52,
	USERAUTH_BANNER = 53,
	USERAUTH_PK_OK = 60,
	GLOBAL_REQUEST = 80,
	REQUEST_SUCCESS = 81,
	REQUEST_FAILURE = 82,
	CHANNEL_OPEN = 90,
	CHANNEL_OPEN_CONFIRMATION = 91,
	CHANNEL_OPEN_FAILURE = 92,
	CHANNEL_WINDOW_ADJUST = 93,
	CHANNEL_DATA = 94,
	CHANNEL_EXTENDED_DATA = 95,
	CHANNEL_EOF = 96,
	CHANNEL_CLOSE = 97,
	CHANNEL_REQUEST = 98,
	CHANNEL_SUCCESS = 99,
	CHANNEL_FAILURE = 100,
};

// Disconnect reason codes (RFC 4253 §11.1).
enum Reason : uint32_t {
	PROTOCOL_ERROR = 2,
	KEY_EXCHANGE_FAILED = 3,
	MAC_ERROR = 5,
	SERVICE_NOT_AVAILABLE = 7,
	BY_APPLICATION = 11,
	NO_MORE_AUTH_METHODS = 14,
};

// One direction's packet protection. Created by the key exchange; the initial
// "none" state is a cipher too, so reading and writing have one path.
class PacketCipher {
public:
	virtual ~PacketCipher() = default;
	// Bytes to read before the packet length is known.
	virtual size_t FirstRead() const = 0;
	// The packet length (excluding itself and the tag), from those bytes.
	virtual bool Length(uint32_t seq, const std::string &first, uint32_t &len) = 0;
	// Bytes still to read after `first` for a packet of `len`.
	virtual size_t RestRead(uint32_t len) const = 0;
	// Authenticate and decrypt: `plain` becomes padding_length || payload ||
	// padding.
	virtual bool Open(uint32_t seq, const std::string &first, const std::string &rest, std::string &plain) = 0;
	// Encrypt padding_length || payload || padding into wire bytes, length
	// field and tag included.
	virtual std::string Seal(uint32_t seq, const std::string &plain) = 0;
	virtual size_t BlockSize() const = 0;
	// Is the length field outside the padded region (AEAD, EtM)?
	virtual bool LengthIsAad() const = 0;
};

class Transport {
public:
	// `fd` is a connected socket this object reads and writes directly. The
	// host key signs the exchange hash.
	Transport(int fd, const ssh::HostKey &host_key);
	~Transport();

	// Version exchange and the first key exchange. False (with the reason
	// logged in `Error()`) if the client could not be brought up to NEWKEYS.
	bool Handshake();

	// The next packet payload that is not transport-layer housekeeping: IGNORE,
	// DEBUG and UNIMPLEMENTED are consumed, and a client-initiated re-exchange
	// is run in line. False on EOF, timeout (`timeout_ms` > 0), a protocol
	// error, or a DISCONNECT from the peer.
	bool Read(std::string &payload, int timeout_ms = -1);
	bool Write(const std::string &payload);
	// Send DISCONNECT and stop.
	void Disconnect(uint32_t reason, const std::string &text);

	// Is a packet (or part of one) waiting? For the channel loop's poll.
	int Fd() const {
		return fd_;
	}
	bool Buffered() const {
		return rpos_ < rbuf_.size();
	}

	const std::string &SessionId() const {
		return session_id_;
	}
	const std::string &ClientVersion() const {
		return client_version_;
	}
	// Did the client offer ext-info-c? The server then sends server-sig-algs.
	bool ClientWantsExtInfo() const {
		return client_ext_info_;
	}
	const std::string &Error() const {
		return error_;
	}

private:
	bool ReadExact(size_t n, std::string &out, int timeout_ms);
	bool WriteAll(const std::string &bytes);
	bool ReadPacket(std::string &payload, int timeout_ms);
	bool WritePacket(const std::string &payload);
	// Run one key exchange. `their_kexinit` is the client's KEXINIT payload
	// when the client started it (re-exchange), empty for the first one.
	bool KeyExchange(const std::string &their_kexinit);
	std::string BuildKexInit();
	bool Fail(const std::string &why);

	int fd_;
	ssh::HostKey host_key_;
	std::string rbuf_;
	size_t rpos_ = 0;
	std::string client_version_;
	std::string server_version_;
	std::string session_id_;
	std::unique_ptr<PacketCipher> in_;
	std::unique_ptr<PacketCipher> out_;
	uint32_t in_seq_ = 0;
	uint32_t out_seq_ = 0;
	bool strict_kex_ = false;
	bool first_kex_done_ = false;
	bool client_ext_info_ = false;
	bool dead_ = false;
	std::string error_;
};

} // namespace sshd
} // namespace quackmail
