#pragma once

#include "quackmail/dkim.hpp"
#include "quackmail/spf.hpp"

#include <string>
#include <vector>

namespace quackmail {
namespace dmarc {

// The disposition a domain asks receivers to apply to unauthenticated mail.
enum class Policy {
	None,       // p=none — monitor only
	Quarantine, // p=quarantine — accept but treat as suspicious
	Reject,     // p=reject — refuse at SMTP time
};

enum class Result {
	None,      // the From: domain publishes no DMARC record
	Pass,      // SPF or DKIM authenticated and aligned
	Fail,      // neither authenticated in an aligned way
	TempError, // the record could not be retrieved
	PermError, // the record is unusable
};

struct Eval {
	Result result = Result::None;
	Policy policy = Policy::None; // the effective policy after p=/sp=/pct=
	bool spf_aligned = false;
	bool dkim_aligned = false;
	std::string from_domain;   // the RFC5322.From domain that was evaluated
	std::string policy_domain; // where the record was found (may be the org domain)
	std::string record;        // the raw DMARC TXT record
	std::string info;          // human-readable detail

	// True when the sender's own published policy asks us to refuse the message.
	bool ShouldReject() const {
		return result == Result::Fail && policy == Policy::Reject;
	}
	// True when the sender asks us to accept but treat the message as suspect.
	bool ShouldQuarantine() const {
		return result == Result::Fail && policy == Policy::Quarantine;
	}
};

// Evaluate DMARC for a message. `from_domain` is the domain of the RFC5322.From
// header; `spf_domain` is the identity SPF actually checked (the MAIL FROM
// domain, or the HELO name for a null sender). `msg_id` seeds the deterministic
// pct= sampling so that repeated evaluation of one message is stable.
Eval Evaluate(const std::string &from_domain, const std::string &spf_domain, spf::Result spf_result,
              const std::vector<dkim::VerifyResult> &dkim_results, const std::string &msg_id = "");

// The organizational domain of `domain`.
//
// NOTE: no Public Suffix List is bundled, so this is "the last two labels" plus
// a small table of common multi-label suffixes (co.uk, com.au, ...). For a
// domain under an unlisted multi-label suffix, relaxed alignment comes out
// stricter than a PSL-backed implementation would make it — it never comes out
// looser, so it cannot turn a Fail into a Pass.
std::string OrganizationalDomain(const std::string &domain);

std::string ResultName(Result r);
std::string PolicyName(Policy p);

} // namespace dmarc
} // namespace quackmail
