#pragma once

#include <string>

namespace quackmail {
namespace html {

// Two sanitizers, for two genuinely different jobs.
//
// **SanitizeForDisplay** cleans a `text/html` part that arrived in someone
// else's mail, on its way to being rendered. It is a *deny-list*, and that is
// defensible only because it is not the actual boundary: the part is served into
// a sandboxed frame under its own `default-src 'none'` policy, and this is
// defence in depth behind that.
//
// **SanitizeForCompose** cleans HTML a local user typed, on its way into a
// message that will be *stored* and then *re-served from our origin* to other
// people — the recipient, everyone in a public room, every subscriber to a
// mailing list. So it is a true *allow-list*: anything not named is dropped.
//
// The asymmetry is the point. "It is the user's own input" stops being true the
// moment paste is involved: markup copied out of a hostile page is a stored-XSS
// vector aimed at third parties, and a deny-list is the wrong shape of defence
// for something we are about to keep and redistribute.

// Deny-list clean for rendering a received part. Preserves as much of the
// sender's formatting as it can.
std::string SanitizeForDisplay(const std::string &in);

// Allow-list clean for HTML a user composed. Applied *before* the message is
// built, so what is stored is already safe rather than relying on every future
// reader to clean it again.
//
// Elements kept: p br b i em strong u s a ul ol li blockquote pre code span div
// table thead tbody tr td th img h1 h2 h3 h4 hr.
// Attributes kept: href (http/https/mailto only), src (cid: and data:image/*
// only), alt, title, numeric width/height, and a `style` restricted to colour,
// font, text-align, and margin/padding.
// The `<html>`, `<head>` and `<body>` wrappers are unwrapped rather than kept.
std::string SanitizeForCompose(const std::string &in);

// Rewrite `src="cid:X"` to `prefix + <percent-encoded X>`, over already
// sanitized markup. A targeted attribute rewrite rather than a DOM round-trip:
// re-serializing would risk reintroducing something the sanitizer removed.
std::string RewriteCidUrls(const std::string &in, const std::string &prefix);

// Every `cid:` value the markup refers to, so a composer knows which uploaded
// parts are actually inline and which are attachments.
std::string FirstCidReference(const std::string &in);

// A plain-text rendering of HTML, for the text/plain half of an alternative.
// Block elements become line breaks, `<li>` becomes "- ", entities are resolved,
// everything else is dropped. Crude on purpose: the point is that a recipient
// with no HTML gets something readable, not that it round-trips.
std::string ToPlainText(const std::string &in);

} // namespace html
} // namespace quackmail
