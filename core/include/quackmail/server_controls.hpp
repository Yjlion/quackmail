#pragma once

#include "duckdb.hpp"
#include "quackmail/server_controller.hpp"

#include <string>

namespace quackmail {

// Register the three standard control table functions for a protocol server:
//   <prefix>_start(host VARCHAR, port INTEGER, [tls_cert, tls_key, implicit_tls, starttls])
//   <prefix>_stop()
//   <prefix>_status()
// All three return a single status row:
//   (action VARCHAR, running BOOLEAN, host VARCHAR, port BIGINT, connections BIGINT, note VARCHAR)
//
// The controller and handler are carried to the generic bind via
// TableFunction.function_info, so one implementation serves every extension.
void RegisterServerControls(duckdb::ExtensionLoader &loader, const std::string &prefix,
                            int default_port, ServerController &controller, ConnHandler handler);

} // namespace quackmail
