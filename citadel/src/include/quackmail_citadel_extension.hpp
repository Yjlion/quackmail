#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Native Citadel protocol server (see https://www.citadel.org/protocol.txt).
// A stateful, session-oriented BBS protocol on TCP 504 (default dev port 5040):
// users navigate floors and rooms and read/post messages, all backed by the
// shared citadel_* tables.
class QuackmailCitadelExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
