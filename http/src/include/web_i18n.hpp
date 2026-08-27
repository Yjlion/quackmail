#pragma once

#include "web.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace qmweb {

// The visitor's locale, in order: their `web_locale` preference, the site's
// `qm_default_locale`, the request's Accept-Language, else "en". Never empty,
// and never a locale this build has no catalog for.
//
// Accept-Language sits below the two stored settings on purpose — a preference
// somebody set is a decision, and a header is a guess — but above the fallback,
// which is what lets an anonymous page (the login screen, the public mailing
// list pages) arrive in the reader's own language.
std::string EffectiveLocale(const Ctx &ctx);

// Locales this build has translations for, as (code, label) pairs for a
// <select> — "" first, meaning "follow the site default". Labels are endonyms
// ("Deutsch", not "German"): the one person guaranteed not to read the current
// interface language is the one looking for their own in this list.
std::vector<std::pair<std::string, std::string>> LocaleOptions();

// True if `code` is one this build can actually serve. The preference form
// validates against this rather than storing whatever was posted.
bool KnownLocale(const std::string &code);

// Look up `key` in the message catalog for the caller's locale.
//
// A key with no entry at all returns the key itself: visible and obviously
// wrong in review or in a screenshot, never empty and never a crash. A key that
// exists but is untranslated in this locale returns the **English** string,
// because a half-translated page should read as mixed rather than as debris.
std::string Tr(const Ctx &ctx, const std::string &key);

// The plural form for `n`. Two keys, because that is what the call site can
// name; which one is used is the locale's business — French counts 0 as
// singular, English and German do not.
std::string TrN(const Ctx &ctx, const std::string &key_one, const std::string &key_other, int64_t n);

// Tr() with positional substitution: %1, %2 … are replaced by `args` in order.
// Positional rather than sequential because a translation is free to reorder
// them, which is precisely what concatenating fragments at the call site cannot
// express. A %n with no matching argument is left as written.
std::string TrF(const Ctx &ctx, const std::string &key, const std::vector<std::string> &args);

} // namespace qmweb
} // namespace duckdb
