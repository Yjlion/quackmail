#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Outbound SMTP relay (stub). Drains the quackmail_outbound queue and relays
// messages to remote MTAs. Full relay/MX logic is a later iteration.
class QuackmailSmtpOutExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
