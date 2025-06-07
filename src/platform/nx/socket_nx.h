#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <switch/services/bsd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <stddef.h>

#if defined(HAVE_IPTOS_THROUGHPUT) && HAVE_IPTOS_THROUGHPUT
    #include <netinet/ip.h>
#endif

#include <poll.h>

struct FtpSocketPollFd {
    struct pollfd s;
};

struct FtpSocket {
    int s;
};

int _convert_errno(int bsdErrno);

// taken from libnx socket.c
static inline int bsd_errno(int ret) {
    int errno_;
    if(ret != -1)
        return ret; // Nothing to do
    else {
        if(g_bsdErrno == -1) {
            // We're using -1 to signal Switch error codes.
            // Note: all of the bsd:u/s handlers return 0.
            switch(g_bsdResult) {
                case 0xD201:
                    errno_ = ENFILE;
                    break;
                case 0xD401:
                    errno_ = EFAULT;
                    break;
                case 0x10601:
                    errno_ = EBUSY;
                    break;
                default:
                    errno_ = EPIPE;
                    break;
            }
        }
        else
            errno_ = _convert_errno(g_bsdErrno); /* Nintendo actually used the Linux errno definitions for their FreeBSD build :)
                                                    but we still need to convert to newlib errno */
    }

    errno = errno_;
    return -1;
}

static inline int ftp_socket_open_nx(struct FtpSocket* sock, int domain, int type, int protocol) {
    return sock->s = bsdSocket(domain, type, protocol);
}

static inline int ftp_socket_recv_nx(struct FtpSocket* sock, void* buf, size_t size, int flags) {
    return bsd_errno(bsdRecv(sock->s, buf, size, flags));
}

static inline int ftp_socket_send_nx(struct FtpSocket* sock, const void* buf, size_t size, int flags) {
    return bsd_errno(bsdSend(sock->s, buf, size, flags));
}

static inline int ftp_socket_close_nx(struct FtpSocket* sock) {
    if (sock->s) {
        bsdShutdown(sock->s, SHUT_RDWR);
        bsdClose(sock->s);
        sock->s = 0;
    }
    return 0;
}

static inline int ftp_socket_accept_nx(struct FtpSocket* sock_out, struct FtpSocket* listen_sock, struct sockaddr* addr, size_t* addrlen) {
    socklen_t len = *addrlen;
    const int rc = bsd_errno(sock_out->s = bsdAccept(listen_sock->s, addr, &len));
    *addrlen = len;
    return rc;
}

static inline int ftp_socket_bind_nx(struct FtpSocket* sock, struct sockaddr* addr, size_t addrlen) {
    return bsd_errno(bsdBind(sock->s, addr, addrlen));
}

static inline int ftp_socket_connect_nx(struct FtpSocket* sock, struct sockaddr* addr, size_t addrlen) {
    return bsd_errno(bsdConnect(sock->s, addr, addrlen));
}

static inline int ftp_socket_listen_nx(struct FtpSocket* sock, int backlog) {
    return bsd_errno(bsdListen(sock->s, backlog));
}

static inline int ftp_socket_getsockname_nx(struct FtpSocket* sock, struct sockaddr* addr, size_t* addrlen) {
    socklen_t len = *addrlen;
    const int rc = bsd_errno(bsdGetSockName(sock->s, addr, &len));
    *addrlen = len;
    return rc;
}

static inline int ftp_socket_set_reuseaddr_enable_nx(struct FtpSocket* sock, int enable) {
#if defined(HAVE_SO_REUSEADDR) && HAVE_SO_REUSEADDR
    const int option = 1;
    return bsd_errno(bsdSetSockOpt(sock->s, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)));
#else
    return 0;
#endif
}

static inline int ftp_socket_set_nodelay_enable_nx(struct FtpSocket* sock, int enable) {
#if defined(HAVE_TCP_NODELAY) && HAVE_TCP_NODELAY
    const int option = 1;
    return bsd_errno(bsdSetSockOpt(sock->s, IPPROTO_TCP, TCP_NODELAY, &option, sizeof(option)));
#else
    return 0;
#endif
}

static inline int ftp_socket_set_keepalive_enable_nx(struct FtpSocket* sock, int enable) {
#if defined(HAVE_SO_KEEPALIVE) && HAVE_SO_KEEPALIVE
    const int option = 1;
    return bsd_errno(bsdSetSockOpt(sock->s, SOL_SOCKET, SO_KEEPALIVE, &option, sizeof(option)));
#else
    return 0;
#endif
}

static inline int ftp_socket_set_throughput_enable_nx(struct FtpSocket* sock, int enable) {
#if defined(HAVE_IPTOS_THROUGHPUT) && HAVE_IPTOS_THROUGHPUT
    const int option = IPTOS_THROUGHPUT;
    return bsd_errno(bsdSetSockOpt(sock->s, IPPROTO_IP, IP_TOS, &option, sizeof(option)));
#else
    return 0;
#endif
}

#define O_NONBLOCK_NX 0x800

static inline int ftp_socket_set_nonblocking_enable_nx(struct FtpSocket* sock, int enable) {
    return bsd_errno(bsdFcntl(sock->s, F_SETFL, O_NONBLOCK_NX));
}

static inline int ftp_socket_poll_nx(struct FtpSocketPollEntry* entries, struct FtpSocketPollFd* _fds, size_t nfds, int timeout) {
    struct pollfd* fds = (struct pollfd*)_fds;

    for (size_t i = 0; i < nfds; i++) {
        if (entries[i].fd) {
            fds[i].fd = entries[i].fd->s;
            fds[i].events = 0;
            if (entries[i].events & FtpSocketPollType_IN) {
                fds[i].events |= POLLIN;
            }
            if (entries[i].events & FtpSocketPollType_OUT) {
                fds[i].events |= POLLOUT;
            }
        } else {
            fds[i].fd = -1;
        }
    }

    int rc = bsd_errno(bsdPoll(fds, nfds, timeout));
    if (rc < 0) {
        return rc;
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

#define ftp_socket_open ftp_socket_open_nx
#define ftp_socket_recv ftp_socket_recv_nx
#define ftp_socket_send ftp_socket_send_nx
#define ftp_socket_close ftp_socket_close_nx
#define ftp_socket_accept ftp_socket_accept_nx
#define ftp_socket_bind ftp_socket_bind_nx
#define ftp_socket_connect ftp_socket_connect_nx
#define ftp_socket_listen ftp_socket_listen_nx
#define ftp_socket_getsockname ftp_socket_getsockname_nx
#define ftp_socket_set_reuseaddr_enable ftp_socket_set_reuseaddr_enable_nx
#define ftp_socket_set_nodelay_enable ftp_socket_set_nodelay_enable_nx
#define ftp_socket_set_keepalive_enable ftp_socket_set_keepalive_enable_nx
#define ftp_socket_set_throughput_enable ftp_socket_set_throughput_enable_nx
#define ftp_socket_set_nonblocking_enable ftp_socket_set_nonblocking_enable_nx
#define ftp_socket_poll ftp_socket_poll_nx

#ifdef __cplusplus
}
#endif
