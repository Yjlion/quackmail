#pragma once

#include "quackmail/citadel_store.hpp"

#include <string>
#include <vector>

namespace quackmail {
namespace citadel {

// Render a stored message as the field-per-line listing returned by the Citadel
// MSG0 command (WITHOUT the surrounding "100" result line or "000" terminator —
// the protocol handler adds those). Emits property lines like:
//   type=0
//   msgn=42
//   time=1700000000
//   from=Alice
//   subj=Hello
//   text
//   <body line 1>
//   <body line 2>
// mode: 0/3 = headers + body, 1 = headers only, 2 = body only.
std::vector<std::string> FormatMsg0(const Message &msg, int mode);

// Extract a plain-text body from a stored message: the raw bytes for native
// (format 0/1) messages, or the first text/* MIME part for RFC822 (format 4).
std::string BodyText(const Message &msg);

// The RFC2822 Message-ID a Citadel server reports for a stored message:
// "<XXXXXXXX-<msgnum>@<node>>", where XXXXXXXX is the message time in uppercase
// hex. Messages that already carry an EUID containing '@' keep it verbatim.
std::string MessageId(const Message &msg, const std::string &node);

// Render a stored message as RFC822 (the "MT_RFC822" view a real Citadel server
// serves over POP3/IMAP/NNTP). Format 4 messages are already RFC822 and are
// returned as-is; native messages get the header block Citadel synthesizes:
//   Return-Path / Date / Subject / To / References / Message-ID / From
// followed by a blank line and the body. Lines are CRLF-terminated.
std::string RenderRfc822(const Message &msg, const std::string &node);

// ---- groupware objects ---------------------------------------------------
//
// A calendar entry, contact or note is stored as an ordinary format-4 message
// with one part, keyed by the object's own UID. These two are the wrapper and
// its inverse, and they live here rather than in a front-end because the web
// UI, CalDAV/CardDAV and inbound iTIP all write into the same rooms — a second
// spelling of the wrapper would make one of them unable to read what another
// wrote.

std::string WrapObject(const std::string &content_type, const std::string &body,
                       const std::string &subject, const std::string &uid,
                       const std::string &author, const std::string &node);

// The stored object's own bytes: the first part whose type is `want_type`, or
// "" when this message is not one of ours. `text/x-vcard` is accepted for
// `text/vcard`, because that is what older Citadel writes.
std::string ObjectBody(const Message &msg, const std::string &want_type);

} // namespace citadel
} // namespace quackmail
