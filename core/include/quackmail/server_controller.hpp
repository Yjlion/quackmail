#pragma once

#include "duckdb.hpp"
#include "quackmail/net.hpp"
#include "quackmail/tls.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace quackmail {

// Per-connection handler. Runs on its own thread; gets the DuckDB database
// instance (to open an internal Connection) and the client stream.
using ConnHandler = void (*)(duckdb::DatabaseInstance &db, net::ClientStream &stream);

// Owns a listening socket + accept thread for one protocol server. Each
// extension keeps a single global instance. Coordination between extensions
// happens through DuckDB tables, not through this object.
class ServerController {
public:
	ServerController() = default;
	~ServerController();

	bool Start(duckdb::DatabaseInstance &db, const std::string &host, int port,
	           const tls::TlsConfig &tls_config, ConnHandler handler, std::string &err);
	bool Stop(std::string &err);

	// Re-read the certificate and key from the paths Start was given and put
	// the new material in service. Existing connections are untouched and the
	// listening socket is not disturbed: AcceptLoop reads the context afresh on
	// every accept, so the next connection gets the new certificate.
	//
	// False with `err` set when the listener was started without certificate
	// paths, or when the new material does not load — in which case the old
	// certificate stays in service rather than the listener losing TLS.
	bool ReloadTls(std::string &err);

	bool IsRunning();
	std::string Host();
	int Port();
	uint64_t Connections();          // total connections accepted
	bool ImplicitTls();
	bool StartTlsEnabled();
	SSL_CTX *TlsCtx();               // for handlers performing STARTTLS

	// Refuse new connections while this many are already open; 0 (the default)
	// means unlimited, so every protocol that does not set it is unchanged.
	//
	// Only HTTP needs this today, and it needs it because it is the only
	// protocol here that keeps a connection alive across requests: with one
	// thread per connection, a browser opening six sockets per tab and holding
	// them idle is a thread-exhaustion vector that `Connection: close` used to
	// make impossible. Persistence and this cap belong together.
	void SetMaxConnections(int n);
	int MaxConnections();

private:
	void AcceptLoop();

	std::mutex mutex_;
	std::thread accept_thread_;
	std::atomic<bool> running_ {false};
	std::atomic<bool> stop_ {false};
	std::atomic<uint64_t> conn_count_ {0};
	std::atomic<int> active_conns_ {0};
	std::atomic<int> max_conns_ {0};

	int listen_fd_ = -1;
	std::string host_;
	int port_ = 0;
	duckdb::DatabaseInstance *db_ = nullptr;
	ConnHandler handler_ = nullptr;
	tls::TlsConfig tls_config_;
	tls::TlsContext tls_ctx_;
};

} // namespace quackmail
