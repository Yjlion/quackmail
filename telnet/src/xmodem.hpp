#pragma once

// Xmodem-1K with CRC-16, both directions, over a telnet session.
//
// Why Xmodem and not Zmodem: Xmodem is a hundred and fifty lines and has no
// state beyond a block number, while Zmodem is a far larger protocol for the
// same result over a link that is already reliable — TCP has done the work
// Zmodem's windowing exists for. A BBS terminal that can do neither still has
// the plain listing and the base64 fallback, and anyone who wants a real file
// manager has WebDAV or FTP against the same rooms.
//
// The wire is a telnet stream, so every byte goes through Session::WriteRaw,
// which doubles 0xFF. Without that a payload byte of 0xFF reads at the far end
// as the start of a telnet command and the file arrives corrupted — the classic
// way binary transfer over telnet fails.

#include "quackmail/telnet.hpp"

#include <string>

namespace quackmail {
namespace xmodem {

// Send `data` to the peer. Returns false if the transfer was not completed —
// the peer never started it, cancelled, or stopped answering.
//
// The receiver drives: it sends 'C' to ask for CRC mode, and the transfer only
// begins once it does. `start_timeout_ms` bounds that wait, so a user who typed
// the command and then changed their mind is not held forever.
bool Send(telnet::Session &t, const std::string &data, int start_timeout_ms = 60000);

// Receive into `out`. Returns false on cancellation or timeout.
//
// Xmodem has no length field: the last block is padded, so `out` comes back
// rounded up to a block boundary. Trailing SUB (0x1A) padding is stripped,
// which is the convention every Xmodem implementation uses and which is also
// why Xmodem is a poor fit for a file that legitimately ends in 0x1A. Said here
// rather than discovered later.
bool Receive(telnet::Session &t, std::string &out, size_t max_bytes,
             int start_timeout_ms = 60000);

} // namespace xmodem
} // namespace quackmail
