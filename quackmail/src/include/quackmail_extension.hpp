#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Umbrella extension: ensures the shared schema, and provides version, status,
// and local-user management (qm_user_add / qm_user_remove) used by the AUTH
// paths of the protocol extensions.
class QuackmailExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
