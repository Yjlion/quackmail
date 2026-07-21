#pragma once

#include "duckdb.hpp"

namespace duckdb {

// POP3 retrieval (stub). Serves messages from quackmail_messages. Full command
// set is a later iteration.
class QuackmailPop3Extension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
