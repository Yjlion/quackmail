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

} // namespace citadel
} // namespace quackmail
