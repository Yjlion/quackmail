#pragma once

#include "duckdb.hpp"

namespace duckdb {

// ManageSieve (RFC 5804) script management (stub). Stores/activates Sieve
// scripts in quackmail_sieve_scripts, which the delivery path then applies.
class QuackmailManagesieveExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
