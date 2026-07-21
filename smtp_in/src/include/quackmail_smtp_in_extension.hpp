#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Inbound SMTP MTA: accepts mail over SMTP (with optional STARTTLS/implicit TLS
// and SASL AUTH) and stores it into the shared quackmail_messages tables.
class QuackmailSmtpInExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
