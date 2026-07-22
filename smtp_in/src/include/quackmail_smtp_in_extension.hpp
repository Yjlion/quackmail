#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Inbound SMTP gateway: accepts mail over SMTP (with optional STARTTLS/implicit
// TLS and SASL AUTH) and delivers it into each recipient's Citadel Mail room
// (or a Sieve fileinto room) in the shared citadel_* tables.
class QuackmailSmtpInExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
