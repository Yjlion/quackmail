#include "quackmail/server_controller.hpp"

#include <cerrno>
#include <cstring>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace quackmail {

ServerController::~ServerController() {
	std::string err;
	Stop(err);
}

static bool ParseHost(const std::string &host, in_addr_t &out) {
	if (host.empty() || host == "0.0.0.0" || host == "*") {
		out = INADDR_ANY;
		return true;
	}
	std::string h = (host == "localhost") ? "127.0.0.1" : host;
	struct in_addr addr;
	if (inet_pton(AF_INET, h.c_str(), &addr) == 1) {
		out = addr.s_addr;
		return true;
	}
	return false;
}

bool ServerController::Start(duckdb::DatabaseInstance &db, const std::string &host, int port,
                            const tls::TlsConfig &tls_config, ConnHandler handler, std::string &err) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (running_.load()) {
		err = "server already running on port " + std::to_string(port_);
		return false;
	}

	in_addr_t bind_addr;
	if (!ParseHost(host, bind_addr)) {
		err = "invalid host: " + host;
		return false;
	}

	if (tls::Enabled(tls_config)) {
		if (!tls_ctx_.Init(tls_config, err)) {
			return false;
		}
	}

	int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		err = std::string("socket() failed: ") + std::strerror(errno);
		return false;
	}
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	struct sockaddr_in sa;
	std::memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	sa.sin_addr.s_addr = bind_addr;
	if (::bind(fd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)) < 0) {
		err = std::string("bind() failed on port ") + std::to_string(port) + ": " + std::strerror(errno);
		::close(fd);
		return false;
	}
	if (::listen(fd, 16) < 0) {
		err = std::string("listen() failed: ") + std::strerror(errno);
		::close(fd);
		return false;
	}

	listen_fd_ = fd;
	host_ = host;
	port_ = port;
	db_ = &db;
	handler_ = handler;
	tls_config_ = tls_config;
	stop_.store(false);
	running_.store(true);
	conn_count_.store(0);

	accept_thread_ = std::thread(&ServerController::AcceptLoop, this);
	return true;
}

void ServerController::AcceptLoop() {
	while (!stop_.load()) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		int cfd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr *>(&peer), &plen);
		if (cfd < 0) {
			if (stop_.load()) {
				break;
			}
			continue;
		}
		conn_count_.fetch_add(1);
		active_conns_.fetch_add(1);

		duckdb::DatabaseInstance *db = db_;
		ConnHandler handler = handler_;
		bool implicit = tls_config_.implicit;
		SSL_CTX *ctx = tls_ctx_.Get();

		std::thread([this, cfd, db, handler, implicit, ctx]() {
			net::ClientStream stream(cfd);
			bool ok = true;
			if (implicit) {
				std::string terr;
				ok = stream.AcceptTls(ctx, terr);
			}
			if (ok && handler && db) {
				handler(*db, stream);
			}
			active_conns_.fetch_sub(1);
		}).detach();
	}
	running_.store(false);
}

bool ServerController::Stop(std::string &err) {
	std::thread to_join;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!running_.load() && !accept_thread_.joinable()) {
			err = "server not running";
			return false;
		}
		stop_.store(true);
		if (listen_fd_ >= 0) {
			::shutdown(listen_fd_, SHUT_RDWR);
			::close(listen_fd_);
			listen_fd_ = -1;
		}
		to_join = std::move(accept_thread_);
	}
	if (to_join.joinable()) {
		to_join.join();
	}
	running_.store(false);
	return true;
}

bool ServerController::IsRunning() {
	return running_.load();
}
std::string ServerController::Host() {
	std::lock_guard<std::mutex> lock(mutex_);
	return host_;
}
int ServerController::Port() {
	std::lock_guard<std::mutex> lock(mutex_);
	return port_;
}
uint64_t ServerController::Connections() {
	return conn_count_.load();
}
bool ServerController::ImplicitTls() {
	return tls_config_.implicit;
}
bool ServerController::StartTlsEnabled() {
	return tls_config_.starttls;
}
SSL_CTX *ServerController::TlsCtx() {
	return tls_ctx_.Get();
}

} // namespace quackmail
