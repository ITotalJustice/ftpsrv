#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <network.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>

struct FtpSocketPollFd {
    struct pollsd s;
};

struct FtpSocket {
    int s;
};

#ifndef SHUT_RDWR
    #define SHUT_RDWR 2
#endif

static inline int ftp_socket_setsockopt_wii(int s, int level, int optname, const void* optval, size_t optlen) {
    const int rc = net_setsockopt(s, level, optname, optval, optlen);
    if (rc < 0) {
        errno = -rc;
        return -1;
    }
    return rc;
}

static inline int ftp_socket_fcntl_wii(int s, int cmd, int flags) {
    const int rc = net_fcntl(s, cmd, flags);
    if (rc < 0) {
        errno = -rc;
        return -1;
    }
    return rc;
}

static inline int ftp_socket_open_wii(struct FtpSocket* sock, int domain, int type, int protocol) {
    const int rc = sock->s = net_socket(domain, type, protocol);
    if (rc < 0) {
        errno = -rc;
        return -1;
    }
    return rc;
}

static inline int ftp_socket_recv_wii(struct FtpSocket* sock, void* buf, size_t size, int flags) {
    const int rc = net_recv(sock->s, buf, size, flags);
    if (rc < 0) {
        errno = -rc;
        return -1;
    }
    return rc;
}

static inline int ftp_socket_send_wii(struct FtpSocket* sock, const void* buf, size_t size, int flags) {
    const int rc = net_send(sock->s, buf, size, flags);
    if (rc < 0) {
        errno = -rc;
        return -1;
    }
    return rc;
}

static inline int ftp_socket_close_wii(struct FtpSocket* sock) {
    if (sock->s) {
        net_shutdown(sock->s, SHUT_RDWR);
        net_close(sock->s);
        sock->s = 0;
    }
    return 0;
}

static inline int ftp_socket_accept_wii(struct FtpSocket* sock_out, struct FtpSocket* listen_sock, struct sockaddr* addr, size_t* addrlen) {
    socklen_t len = *addrlen;
    const int rc = sock_out->s = net_accept(listen_sock->s, addr, &len);
    if (rc < 0) {
        errno = -rc;
        return -1;
    }
    *addrlen = len;
    return rc;
}

static inline int ftp_socket_bind_wii(struct FtpSocket* sock, struct sockaddr* addr, size_t addrlen) {
    const int rc = net_bind(sock->s, addr, addrlen);
    if (rc < 0) {
        errno = -rc;
        return -1;
    }
    return rc;
}

static inline int ftp_socket_connect_wii(struct FtpSocket* sock, struct sockaddr* addr, size_t addrlen) {
    const int rc = net_connect(sock->s, addr, addrlen);
    if (rc < 0) {
        errno = -rc;
        return -1;
    }
    return rc;
}

static inline int ftp_socket_listen_wii(struct FtpSocket* sock, int backlog) {
    const int rc = net_listen(sock->s, backlog);
    if (rc < 0) {
        errno = -rc;
        return -1;
    }
    return rc;
}

static inline int ftp_socket_getsockname_wii(struct FtpSocket* sock, struct sockaddr* addr, size_t* addrlen) {
    socklen_t len = *addrlen;
    const int rc = net_getsockname(sock->s, addr, &len);
    if (rc < 0) {
        errno = -rc;
        return -1;
    }
    *addrlen = len;
    return rc;
}

static inline int ftp_socket_set_reuseaddr_enable_wii(struct FtpSocket* sock, int enable) {
#if defined(HAVE_SO_REUSEADDR) && HAVE_SO_REUSEADDR
    const int option = 1;
    return ftp_socket_setsockopt_wii(sock->s, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
#else
    return 0;
#endif
}

static inline int ftp_socket_set_nodelay_enable_wii(struct FtpSocket* sock, int enable) {
#if defined(HAVE_TCP_NODELAY) && HAVE_TCP_NODELAY
    const int option = 1;
    return ftp_socket_setsockopt_wii(sock->s, IPPROTO_TCP, TCP_NODELAY, &option, sizeof(option));
#else
    return 0;
#endif
}

static inline int ftp_socket_set_keepalive_enable_wii(struct FtpSocket* sock, int enable) {
#if defined(HAVE_SO_KEEPALIVE) && HAVE_SO_KEEPALIVE
    const int option = 1;
    return ftp_socket_setsockopt_wii(sock->s, SOL_SOCKET, SO_KEEPALIVE, &option, sizeof(option));
#else
    return 0;
#endif
}

static inline int ftp_socket_set_throughput_enable_wii(struct FtpSocket* sock, int enable) {
#if defined(HAVE_IPTOS_THROUGHPUT) && HAVE_IPTOS_THROUGHPUT
    const int option = IPTOS_THROUGHPUT;
    return ftp_socket_setsockopt_wii(sock->s, IPPROTO_IP, IP_TOS, &option, sizeof(option));
#else
    return 0;
#endif
}

static inline int ftp_socket_set_nonblocking_enable_wii(struct FtpSocket* sock, int enable) {
    int rc = ftp_socket_fcntl_wii(sock->s, F_GETFL, 0);
    if (rc >= 0) {
        rc = ftp_socket_fcntl_wii(sock->s, F_SETFL, rc | O_NONBLOCK);
    }
    errno = -rc;
    return rc;
}

static inline int ftp_socket_poll_wii(struct FtpSocketPollEntry* entries, struct FtpSocketPollFd* _fds, size_t nfds, int timeout) {
    struct pollsd* fds = (struct pollsd*)_fds;

    for (size_t i = 0; i < nfds; i++) {
        if (entries[i].fd) {
            fds[i].socket = entries[i].fd->s;
            fds[i].events = 0;
            if (entries[i].events & FtpSocketPollType_IN) {
                fds[i].events |= POLLIN;
            }
            if (entries[i].events & FtpSocketPollType_OUT) {
                fds[i].events |= POLLOUT;
            }
        } else {
            fds[i].socket = -1;
        }
    }

    int rc = net_poll(fds, nfds, timeout);
    if (rc < 0) {
        errno = -rc;
        return -1;
    }

    for (size_t i = 0; i < nfds; i++) {
        if (entries[i].fd) {
            entries[i].revents = 0;
            if (fds[i].revents & POLLIN) {
                entries[i].revents |= FtpSocketPollType_IN;
            }
            if (fds[i].revents & POLLOUT) {
                entries[i].revents |= FtpSocketPollType_OUT;
            }
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                entries[i].revents |= FtpSocketPollType_ERROR;
            }
        }
    }

    return rc;
}

#define ftp_socket_open ftp_socket_open_wii
#define ftp_socket_recv ftp_socket_recv_wii
#define ftp_socket_send ftp_socket_send_wii
#define ftp_socket_close ftp_socket_close_wii
#define ftp_socket_accept ftp_socket_accept_wii
#define ftp_socket_bind ftp_socket_bind_wii
#define ftp_socket_connect ftp_socket_connect_wii
#define ftp_socket_listen ftp_socket_listen_wii
#define ftp_socket_getsockname ftp_socket_getsockname_wii
#define ftp_socket_set_reuseaddr_enable ftp_socket_set_reuseaddr_enable_wii
#define ftp_socket_set_nodelay_enable ftp_socket_set_nodelay_enable_wii
#define ftp_socket_set_keepalive_enable ftp_socket_set_keepalive_enable_wii
#define ftp_socket_set_throughput_enable ftp_socket_set_throughput_enable_wii
#define ftp_socket_set_nonblocking_enable ftp_socket_set_nonblocking_enable_wii
#define ftp_socket_poll ftp_socket_poll_wii

#ifdef __cplusplus
}
#endif
