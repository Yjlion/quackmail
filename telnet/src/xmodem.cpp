#include "xmodem.hpp"

namespace quackmail {
namespace xmodem {

namespace {

const unsigned char SOH = 0x01; // 128-byte block follows
const unsigned char STX = 0x02; // 1024-byte block follows
const unsigned char EOT = 0x04; // end of transmission
const unsigned char ACK = 0x06;
const unsigned char NAK = 0x15;
const unsigned char CAN = 0x18;
const unsigned char SUB = 0x1A; // the pad byte

const int kByteTimeoutMs = 10000;
const int kMaxRetries = 10;

uint16_t Crc16(const char *data, size_t len) {
	// CRC-16/XMODEM: polynomial 0x1021, seed 0, no reflection. Not the same as
	// the CCITT variant with a 0xFFFF seed, and a receiver will reject every
	// block if the two are confused.
	uint16_t crc = 0;
	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)((unsigned char)data[i]) << 8;
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

void SendByte(telnet::Session &t, unsigned char b) {
	t.WriteRaw(std::string(1, (char)b));
}

void Cancel(telnet::Session &t) {
	// Two CANs is what every implementation looks for; one can be a stray byte.
	t.WriteRaw(std::string(2, (char)CAN));
}

} // namespace

bool Send(telnet::Session &t, const std::string &data, int start_timeout_ms) {
	// The receiver opens the conversation. 'C' asks for CRC blocks; a plain NAK
	// asks for the old checksum form, which this does not implement — CRC has
	// been universal for thirty years and supporting both doubles the protocol
	// for no one.
	unsigned char c = 0;
	bool started = false;
	for (int i = 0; i < 6 && !started; i++) {
		if (!t.ReadRawByte(c, start_timeout_ms / 6 + 1)) {
			continue;
		}
		if (c == 'C') {
			started = true;
		} else if (c == CAN) {
			return false;
		}
	}
	if (!started) {
		return false;
	}

	size_t at = 0;
	unsigned char block = 1;
	while (at < data.size()) {
		// 1K blocks while a kilobyte remains, 128 for the tail. Sending a 1K
		// block for the last forty bytes would pad nine hundred bytes of SUB
		// into the file for nothing.
		size_t want = (data.size() - at >= 1024) ? 1024 : 128;
		std::string payload = data.substr(at, want);
		payload.resize(want, (char)SUB);

		std::string frame;
		frame.push_back((char)(want == 1024 ? STX : SOH));
		frame.push_back((char)block);
		frame.push_back((char)(unsigned char)(255 - block));
		frame += payload;
		uint16_t crc = Crc16(payload.data(), payload.size());
		frame.push_back((char)(crc >> 8));
		frame.push_back((char)(crc & 0xFF));

		bool acked = false;
		for (int retry = 0; retry < kMaxRetries && !acked; retry++) {
			t.WriteRaw(frame);
			unsigned char reply = 0;
			if (!t.ReadRawByte(reply, kByteTimeoutMs)) {
				continue; // the peer went quiet; resend
			}
			if (reply == ACK) {
				acked = true;
			} else if (reply == CAN) {
				return false;
			}
			// NAK, or anything else: send it again.
		}
		if (!acked) {
			Cancel(t);
			return false;
		}
		at += want;
		block++;
	}

	// EOT until acknowledged. A receiver is allowed to NAK the first one.
	for (int retry = 0; retry < kMaxRetries; retry++) {
		SendByte(t, EOT);
		unsigned char reply = 0;
		if (t.ReadRawByte(reply, kByteTimeoutMs) && reply == ACK) {
			return true;
		}
	}
	return false;
}

bool Receive(telnet::Session &t, std::string &out, size_t max_bytes, int start_timeout_ms) {
	out.clear();
	unsigned char expect = 1;

	// Ask for CRC mode until the sender starts. The 'C' is repeated because a
	// sender that was not ready yet simply missed the first one.
	int waited = 0;
	unsigned char b = 0;
	bool got_header = false;
	while (waited < start_timeout_ms && !got_header) {
		SendByte(t, 'C');
		if (t.ReadRawByte(b, 3000)) {
			if (b == SOH || b == STX || b == EOT || b == CAN) {
				got_header = true;
				break;
			}
		}
		waited += 3000;
	}
	if (!got_header) {
		return false;
	}

	for (;;) {
		if (b == EOT) {
			SendByte(t, ACK);
			// Strip the pad. Xmodem has no length field, so this is the only way
			// back to the original size — and it is why a file whose real last
			// byte is 0x1A cannot survive an Xmodem round trip.
			size_t end = out.size();
			while (end > 0 && (unsigned char)out[end - 1] == SUB) {
				end--;
			}
			out.resize(end);
			return true;
		}
		if (b == CAN) {
			return false;
		}
		if (b != SOH && b != STX) {
			// Noise between blocks. Ask again rather than trying to resync.
			t.DrainInput(200);
			SendByte(t, NAK);
			if (!t.ReadRawByte(b, kByteTimeoutMs)) {
				return false;
			}
			continue;
		}

		const size_t want = (b == STX) ? 1024 : 128;
		unsigned char num = 0, inv = 0;
		if (!t.ReadRawByte(num, kByteTimeoutMs) || !t.ReadRawByte(inv, kByteTimeoutMs)) {
			return false;
		}
		std::string payload;
		payload.reserve(want);
		bool short_read = false;
		for (size_t i = 0; i < want; i++) {
			unsigned char d = 0;
			if (!t.ReadRawByte(d, kByteTimeoutMs)) {
				short_read = true;
				break;
			}
			payload.push_back((char)d);
		}
		unsigned char hi = 0, lo = 0;
		if (short_read || !t.ReadRawByte(hi, kByteTimeoutMs) || !t.ReadRawByte(lo, kByteTimeoutMs)) {
			return false;
		}

		const bool num_ok = (unsigned char)(255 - num) == inv;
		const bool crc_ok = Crc16(payload.data(), payload.size()) == (uint16_t)((hi << 8) | lo);

		if (num_ok && num == (unsigned char)(expect - 1)) {
			// The sender never saw our ACK and sent the previous block again.
			// Acknowledge it and move on rather than storing it twice.
			SendByte(t, ACK);
		} else if (num_ok && crc_ok && num == expect) {
			if (out.size() + payload.size() > max_bytes) {
				Cancel(t);
				return false;
			}
			out += payload;
			expect++;
			SendByte(t, ACK);
		} else {
			t.DrainInput(200);
			SendByte(t, NAK);
		}

		if (!t.ReadRawByte(b, kByteTimeoutMs)) {
			return false;
		}
	}
}

} // namespace xmodem
} // namespace quackmail
