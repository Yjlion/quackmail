#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Periodic background work. Unlike every other module this one owns no
// listener: it owns clocks. Mailing-list distribution walks each list room's
// new messages and fans them out onto the outbound queue; the fetcher polls
// remote POP3/IMAP/RSS sources and posts what is new into a room.
//
// Both are PeriodicWorkers, and both also expose a one-shot "run now" table
// function — which is what makes them testable without waiting on a timer.
class QuackmailSpoolExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
