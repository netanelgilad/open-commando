/*
** Winsock on POSIX sockets.
**
** Under Emscripten, POSIX sockets are proxied over WebSockets (or a
** WebRTC transport later); here we only map the Winsock names and error
** codes onto the BSD API.
*/
#pragma once

#include "windows.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)

typedef struct sockaddr SOCKADDR, *PSOCKADDR, *LPSOCKADDR;
typedef struct sockaddr_in SOCKADDR_IN, *PSOCKADDR_IN, *LPSOCKADDR_IN;
typedef struct in_addr IN_ADDR, *PIN_ADDR, *LPIN_ADDR;
typedef struct hostent HOSTENT, *PHOSTENT, *LPHOSTENT;
typedef struct linger LINGER;

#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffff
#endif

#define WSADESCRIPTION_LEN 256
#define WSASYS_STATUS_LEN 128
typedef struct WSAData {
    WORD wVersion;
    WORD wHighVersion;
    char szDescription[WSADESCRIPTION_LEN + 1];
    char szSystemStatus[WSASYS_STATUS_LEN + 1];
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char *lpVendorInfo;
} WSADATA, *LPWSADATA;

/* Winsock error numbers (Windows values, as the engine compares them). */
#define WSABASEERR 10000
#define WSAEINTR (WSABASEERR + 4)
#define WSAEBADF (WSABASEERR + 9)
#define WSAEACCES (WSABASEERR + 13)
#define WSAEFAULT (WSABASEERR + 14)
#define WSAEINVAL (WSABASEERR + 22)
#define WSAEMFILE (WSABASEERR + 24)
#define WSAEWOULDBLOCK (WSABASEERR + 35)
#define WSAEINPROGRESS (WSABASEERR + 36)
#define WSAEALREADY (WSABASEERR + 37)
#define WSAENOTSOCK (WSABASEERR + 38)
#define WSAEDESTADDRREQ (WSABASEERR + 39)
#define WSAEMSGSIZE (WSABASEERR + 40)
#define WSAEPROTOTYPE (WSABASEERR + 41)
#define WSAENOPROTOOPT (WSABASEERR + 42)
#define WSAEPROTONOSUPPORT (WSABASEERR + 43)
#define WSAESOCKTNOSUPPORT (WSABASEERR + 44)
#define WSAEOPNOTSUPP (WSABASEERR + 45)
#define WSAEPFNOSUPPORT (WSABASEERR + 46)
#define WSAEAFNOSUPPORT (WSABASEERR + 47)
#define WSAEADDRINUSE (WSABASEERR + 48)
#define WSAEADDRNOTAVAIL (WSABASEERR + 49)
#define WSAENETDOWN (WSABASEERR + 50)
#define WSAENETUNREACH (WSABASEERR + 51)
#define WSAENETRESET (WSABASEERR + 52)
#define WSAECONNABORTED (WSABASEERR + 53)
#define WSAECONNRESET (WSABASEERR + 54)
#define WSAENOBUFS (WSABASEERR + 55)
#define WSAEISCONN (WSABASEERR + 56)
#define WSAENOTCONN (WSABASEERR + 57)
#define WSAESHUTDOWN (WSABASEERR + 58)
#define WSAETOOMANYREFS (WSABASEERR + 59)
#define WSAETIMEDOUT (WSABASEERR + 60)
#define WSAECONNREFUSED (WSABASEERR + 61)
#define WSAELOOP (WSABASEERR + 62)
#define WSAENAMETOOLONG (WSABASEERR + 63)
#define WSAEHOSTDOWN (WSABASEERR + 64)
#define WSAEHOSTUNREACH (WSABASEERR + 65)
#define WSAENOTEMPTY (WSABASEERR + 66)
#define WSAEPROCLIM (WSABASEERR + 67)
#define WSAEUSERS (WSABASEERR + 68)
#define WSAEDQUOT (WSABASEERR + 69)
#define WSAESTALE (WSABASEERR + 70)
#define WSAEREMOTE (WSABASEERR + 71)
#define WSAEDISCON (WSABASEERR + 101)
#define WSASYSNOTREADY (WSABASEERR + 91)
#define WSAVERNOTSUPPORTED (WSABASEERR + 92)
#define WSANOTINITIALISED (WSABASEERR + 93)
#define WSAHOST_NOT_FOUND (WSABASEERR + 1001)

#ifdef __cplusplus
extern "C" {
#endif
int WSAStartup(WORD version, LPWSADATA data);
int WSACleanup(void);
int WSAGetLastError(void);
void WSASetLastError(int err);
int closesocket(SOCKET s);
int ioctlsocket(SOCKET s, long cmd, unsigned long *arg);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
/* Winsock takes address/option lengths as int*; POSIX uses socklen_t*. */
inline ssize_t recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen) {
  socklen_t l = fromlen ? (socklen_t)*fromlen : 0;
  ssize_t r = ::recvfrom(s, buf, (size_t)len, flags, from, fromlen ? &l : nullptr);
  if (fromlen)
    *fromlen = (int)l;
  return r;
}
inline int getsockopt(SOCKET s, int level, int name, char *val, int *len) {
  socklen_t l = len ? (socklen_t)*len : 0;
  int r = ::getsockopt(s, level, name, val, len ? &l : nullptr);
  if (len)
    *len = (int)l;
  return r;
}
inline int getsockname(SOCKET s, struct sockaddr *addr, int *len) {
  socklen_t l = len ? (socklen_t)*len : 0;
  int r = ::getsockname(s, addr, len ? &l : nullptr);
  if (len)
    *len = (int)l;
  return r;
}
inline int getpeername(SOCKET s, struct sockaddr *addr, int *len) {
  socklen_t l = len ? (socklen_t)*len : 0;
  int r = ::getpeername(s, addr, len ? &l : nullptr);
  if (len)
    *len = (int)l;
  return r;
}
inline SOCKET accept(SOCKET s, struct sockaddr *addr, int *len) {
  socklen_t l = len ? (socklen_t)*len : 0;
  SOCKET r = ::accept(s, addr, len ? &l : nullptr);
  if (len)
    *len = (int)l;
  return r;
}
#endif
