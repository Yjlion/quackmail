/*
 * port_hook.c — user-space LD_PRELOAD shim to drive the official Citadel text
 * client against QuackCit on a box where a real Citadel server owns port 504.
 *
 * The `citadel` client prefers the local server's IPv6/IPv4/unix socket on the
 * Citadel port 504. This shim rewrites its socket()/connect() so the connection
 * goes to QuackCit at 127.0.0.1:5040 instead — no root, no iptables, and the real
 * Citadel (used as the parity oracle) is left untouched.
 *
 *   gcc -shared -fPIC -o port_hook.so test/parity/port_hook.c -ldl
 *   LD_PRELOAD=$PWD/port_hook.so citadel -h localhost
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <string.h>

typedef int (*socket_t)(int, int, int);
typedef int (*connect_t)(int, const struct sockaddr *, socklen_t);

/* Normalise the client's server socket to AF_INET so we can redirect it. */
int socket(int domain, int type, int protocol) {
	static socket_t real = 0;
	if (!real) real = (socket_t)dlsym(RTLD_NEXT, "socket");
	if ((domain == AF_INET6 || domain == AF_UNIX) && (type & SOCK_STREAM))
		return real(AF_INET, type, 0);
	return real(domain, type, protocol);
}

/* Redirect any connect() aimed at the Citadel port (504) or the local unix
 * socket to QuackCit on 127.0.0.1:5040. */
int connect(int fd, const struct sockaddr *addr, socklen_t len) {
	static connect_t real = 0;
	if (!real) real = (connect_t)dlsym(RTLD_NEXT, "connect");
	int hit = 0;
	if (addr) {
		if (addr->sa_family == AF_UNIX)
			hit = 1;
		else if (addr->sa_family == AF_INET &&
		         ntohs(((const struct sockaddr_in *)addr)->sin_port) == 504)
			hit = 1;
		else if (addr->sa_family == AF_INET6 &&
		         ntohs(((const struct sockaddr_in6 *)addr)->sin6_port) == 504)
			hit = 1;
	}
	if (hit) {
		struct sockaddr_in r;
		memset(&r, 0, sizeof(r));
		r.sin_family = AF_INET;
		r.sin_port = htons(5040);
		r.sin_addr.s_addr = inet_addr("127.0.0.1");
		return real(fd, (const struct sockaddr *)&r, sizeof(r));
	}
	return real(fd, addr, len);
}
