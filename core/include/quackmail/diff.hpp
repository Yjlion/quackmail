#pragma once

#include <string>

namespace quackmail {
namespace diff {

// Line-oriented unified diff, and a patcher for the same format.
//
// This exists because Citadel stores wiki page history as a chain of *reverse*
// unified diffs inside a MIME message (see wiki.hpp). Citadel produces them
// with the libxdiff it vendors; we produce and consume the same format so a
// history written here is readable by WebCit and vice versa. Vendoring libxdiff
// for two functions is not worth a new dependency in a tree that has none.
//
// Both directions are **bounded by construction**. A wiki page is text a user
// chose, the history is text a user wrote, and both are re-read on every page
// view, so neither function may have a pathological input:
//
//   * `max_lines` caps how much text is examined at all.
//   * `max_edits` caps the edit distance the search will explore.
//
// Past either bound Unified emits a single hunk that deletes everything and
// inserts everything. That is still a *correct* patch — it degrades, it never
// diverges — and it is what a minimal-diff algorithm produces for unrelated
// text anyway.

// A unified diff turning `from` into `to`, with `ctxlen` lines of context.
// Citadel uses 3, so that is the default.
//
// The output carries hunk headers only ("@@ -1,3 +1,4 @@"), with no "---"/"+++"
// file header, which is what libxdiff's emitter produces and what its patcher
// expects. Returns "" when the two are identical: an empty diff means "no
// change", and Citadel rejects a wiki edit that produces one.
std::string Unified(const std::string &from, const std::string &to, int ctxlen = 3,
                    size_t max_lines = 20000, size_t max_edits = 2000);

// Apply a unified diff. Returns false with `err` set when a hunk does not
// apply — the context did not match, or the header is malformed. Hunks are
// applied in order and must be non-overlapping and ascending, which is what
// every emitter produces.
bool Apply(const std::string &src, const std::string &patch, std::string &out, std::string &err);

} // namespace diff
} // namespace quackmail
