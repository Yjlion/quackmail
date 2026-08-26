#pragma once

#include "web.hpp"

#include <string>

namespace duckdb {
namespace qmweb {

// The visitor's locale: their `web_locale` preference, else the site's
// `qm_default_locale`, else "en". Never empty. Anonymous pages (no user to
// look a preference up for, e.g. /login itself) fall straight through to the
// site default, the same way EffectiveTz does for the timezone.
std::string EffectiveLocale(const Ctx &ctx);

// Locales this build actually has translations for, as (code, label) pairs
// for a <select> — "" first, meaning "follow the site default". Adding a
// language is: give kMessages a new column, add one row here, and Tr() below
// starts serving it — no call site changes.
std::vector<std::pair<std::string, std::string>> LocaleOptions();

// Look up `key` in the message catalog and return the string for the
// caller's locale. Only English exists today, so this always returns the
// English string — but it already resolves EffectiveLocale(ctx), which is
// what makes adding a second language later a data change, not a call-site
// change. A key with no catalog entry returns the key itself: visible and
// obviously wrong in review, never empty and never a crash.
std::string Tr(const Ctx &ctx, const std::string &key);

} // namespace qmweb
} // namespace duckdb
