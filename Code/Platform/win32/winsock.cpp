// Winsock entry points on POSIX sockets (see compat/winsock.h).
#include "winsock.h"

#include <cerrno>
#include <cstring>

static thread_local int SocketError = 0;

static int Errno_To_Wsa(int e) {
  switch (e) {
  case 0: return 0;
  case EINTR: return WSAEINTR;
  case EBADF: return WSAEBADF;
  case EACCES: return WSAEACCES;
  case EFAULT: return WSAEFAULT;
  case EINVAL: return WSAEINVAL;
  case EMFILE: return WSAEMFILE;
  case EAGAIN: return WSAEWOULDBLOCK;
  case EINPROGRESS: return WSAEINPROGRESS;
  case EALREADY: return WSAEALREADY;
  case ENOTSOCK: return WSAENOTSOCK;
  case EDESTADDRREQ: return WSAEDESTADDRREQ;
  case EMSGSIZE: return WSAEMSGSIZE;
  case EPROTOTYPE: return WSAEPROTOTYPE;
  case ENOPROTOOPT: return WSAENOPROTOOPT;
  case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
  case EOPNOTSUPP: return WSAEOPNOTSUPP;
  case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
  case EADDRINUSE: return WSAEADDRINUSE;
  case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
  case ENETDOWN: return WSAENETDOWN;
  case ENETUNREACH: return WSAENETUNREACH;
  case ENETRESET: return WSAENETRESET;
  case ECONNABORTED: return WSAECONNABORTED;
  case ECONNRESET: return WSAECONNRESET;
  case ENOBUFS: return WSAENOBUFS;
  case EISCONN: return WSAEISCONN;
  case ENOTCONN: return WSAENOTCONN;
  case ETIMEDOUT: return WSAETIMEDOUT;
  case ECONNREFUSED: return WSAECONNREFUSED;
  case EHOSTUNREACH: return WSAEHOSTUNREACH;
  default: return WSAEINVAL;
  }
}

extern "C" int WSAStartup(WORD version, LPWSADATA data) {
  if (data) {
    memset(data, 0, sizeof(*data));
    data->wVersion = version;
    data->wHighVersion = MAKEWORD(2, 2);
    strcpy(data->szDescription, "Rebel POSIX sockets");
    data->iMaxSockets = 1024;
    data->iMaxUdpDg = 65467;
  }
  return 0;
}

extern "C" int WSACleanup(void) { return 0; }

extern "C" int WSAGetLastError(void) {
  // The engine calls this right after a failing socket call; errno is fresh.
  return SocketError ? SocketError : Errno_To_Wsa(errno);
}

extern "C" void WSASetLastError(int err) {
  SocketError = err;
  if (err == 0)
    errno = 0;
}

extern "C" int closesocket(SOCKET s) {
  if (Rebel_Net_Is_Socket(s))
    return Rebel_Net_Close(s);
  return close(s) == 0 ? 0 : SOCKET_ERROR;
}

extern "C" int ioctlsocket(SOCKET s, long cmd, unsigned long *arg) {
  SocketError = 0;
  if (Rebel_Net_Is_Socket(s)) {
    if (cmd == (long)FIONBIO)
      return 0; // virtual sockets never block
    if (cmd == (long)FIONREAD) {
      *arg = (unsigned long)Rebel_Net_Pending_Bytes(s);
      return 0;
    }
    SocketError = WSAEINVAL;
    return SOCKET_ERROR;
  }
  if (cmd == (long)FIONBIO) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0)
      return SOCKET_ERROR;
    flags = (arg && *arg) ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(s, F_SETFL, flags) == 0 ? 0 : SOCKET_ERROR;
  }
  if (cmd == (long)FIONREAD) {
    int n = 0;
    if (ioctl(s, FIONREAD, &n) != 0)
      return SOCKET_ERROR;
    *arg = (unsigned long)n;
    return 0;
  }
  SocketError = WSAEINVAL;
  return SOCKET_ERROR;
}
