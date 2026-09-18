#pragma once

// The SSH listener (qm_ssh): the BBS shell and SFTP over one SSH connection.
//
// `ssh user@host` lands in the same shell telnet does — the user is already
// authenticated, by password or by a key registered in their profile
// (core/sshkeys.hpp), so the name and password prompts are skipped. `sftp
// user@host` gets the file areas (sftp.hpp). Nothing else: no exec, no port
// forwarding, no agent forwarding.
//
// It lives in the telnet extension rather than a module of its own because the
// shell is this extension's C++, and extensions never share C++ state.

#include "duckdb.hpp"
#include "quackmail/net.hpp"

#include <string>

namespace quackmail {
namespace sshd {

// What the shell needs to start: who, and what the pty-req said.
struct ShellRequest {
	std::string username; // canonical, as stored
	std::string peer_ip;
	std::string term; // empty when the client asked for no pty (ssh -T)
	int cols;
	int rows;

	ShellRequest();
};

// Runs the BBS shell over `stream` for an already-authenticated user. Supplied
// by the telnet extension, which owns the shell.
using ShellRunner = void (*)(duckdb::DatabaseInstance &db, net::ClientStream &stream, const ShellRequest &req);

// Serve one SSH connection to completion.
void Serve(duckdb::DatabaseInstance &db, net::ClientStream &stream, ShellRunner shell);

// The SSH user name as typed → the stored user it means: exact, then
// case-insensitive, then with '_' or '.' standing in for spaces ("joe_user"
// for "Joe User", which a shell cannot pass unquoted). Disabled accounts do not
// resolve. Exposed for the tests of that mapping.
bool ResolveUser(duckdb::Connection &con, const std::string &typed, std::string &canonical);

} // namespace sshd
} // namespace quackmail
