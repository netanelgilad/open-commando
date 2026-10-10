// Virtual datagram network (see compat/winsock.h).
//
// The browser has no UDP, so the engine's datagram sockets are emulated
// here: each socket is a queue of datagrams bound to a virtual port, and
// this endpoint has one virtual IPv4 address given by the page's network
// layer (web/net.js). Outgoing datagrams go to the page with
// Rebel_JS_Net_Send; the page carries them over whatever transport it uses
// (another tab, a WebSocket hub, a WebRTC data channel...) and incoming ones
// are pulled from it whenever the engine polls a socket. The engine's
// netcode (its own reliability layer, ack/resend, bandwidth control) runs
// unchanged on top. Stream (TCP) sockets fall through to libc.
//
// The libc entry points are wrapped at link time (--wrap, see
// Platform/CMakeLists.txt), so the engine's code is untouched.
#include "winsock.h"

#include <emscripten.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace {

const int FIRST_FD = 0x4000; // well above real descriptors
const int MAX_SOCKETS = 256;
const size_t MAX_QUEUED = 1024; // datagrams per socket; more are dropped, as UDP would
const uint16_t FIRST_EPHEMERAL = 49152;

struct Datagram {
  uint32_t from_ip; // network byte order
  uint16_t from_port; // host byte order
  std::vector<uint8_t> data;
};

struct VSocket {
  bool used = false;
  uint16_t port = 0; // host byte order, 0 = unbound
  std::deque<Datagram> queue;
};

VSocket Sockets[MAX_SOCKETS];
uint32_t LocalIp = htonl(INADDR_LOOPBACK); // set by the page once its transport has an address
uint16_t NextEphemeral = FIRST_EPHEMERAL;

VSocket *Get(int fd) {
  int i = fd - FIRST_FD;
  if (i < 0 || i >= MAX_SOCKETS || !Sockets[i].used)
    return nullptr;
  return &Sockets[i];
}

bool Is_Local(uint32_t ip) {
  return ip == LocalIp || (ntohl(ip) >> 24) == 127 || ip == htonl(INADDR_ANY);
}

VSocket *Find_Bound(uint16_t port) {
  for (VSocket &s : Sockets)
    if (s.used && s.port == port)
      return &s;
  return nullptr;
}

void Enqueue(uint32_t from_ip, uint16_t from_port, uint16_t to_port, const uint8_t *data, int len) {
  VSocket *s = Find_Bound(to_port);
  if (!s || s->queue.size() >= MAX_QUEUED)
    return;
  s->queue.push_back(Datagram{from_ip, from_port, std::vector<uint8_t>(data, data + len)});
}

bool Bind_Ephemeral(VSocket *s) {
  for (int tries = 0; tries < 16384; tries++) {
    uint16_t p = NextEphemeral++;
    if (NextEphemeral == 0)
      NextEphemeral = FIRST_EPHEMERAL;
    if (!Find_Bound(p)) {
      s->port = p;
      return true;
    }
  }
  return false;
}

} // namespace

// Page side (web/net.js): Module.rebelNet. Datagrams arrive in a JS queue
// and are copied in here when the engine polls, so the page never calls
// into the game in the middle of a frame.
EM_JS(void, Rebel_JS_Net_Send, (uint32_t to_ip, int to_port, int from_port, const uint8_t *data, int len), {
  const net = Module.rebelNet;
  if (net) net.send(to_ip >>> 0, to_port, from_port, HEAPU8.slice(data, data + len));
});

// Copies the next incoming datagram into buf; returns its length, or -1 if
// none is waiting. *from_ip (network order), *from_port and *to_port are set.
EM_JS(int, Rebel_JS_Net_Poll, (uint8_t *buf, int cap, uint32_t *from_ip, int *from_port, int *to_port), {
  const net = Module.rebelNet;
  const d = net && net.incoming.shift();
  if (!d) return -1;
  const n = Math.min(d.data.length, cap);
  HEAPU8.set(d.data.subarray(0, n), buf);
  HEAPU32[from_ip >> 2] = d.fromIp;
  HEAP32[from_port >> 2] = d.fromPort;
  HEAP32[to_port >> 2] = d.toPort;
  return n;
});

EM_JS(uint32_t, Rebel_JS_Net_Address, (void), {
  const net = Module.rebelNet;
  return net && net.address ? net.address >>> 0 : 0;
});

static void Pump(void) {
  uint32_t a = Rebel_JS_Net_Address();
  if (a)
    LocalIp = a;
  static uint8_t buf[65536];
  for (;;) {
    uint32_t from_ip;
    int from_port, to_port;
    int n = Rebel_JS_Net_Poll(buf, sizeof(buf), &from_ip, &from_port, &to_port);
    if (n < 0)
      break;
    Enqueue(from_ip, (uint16_t)from_port, (uint16_t)to_port, buf, n);
  }
}

extern "C" {
int __real_socket(int, int, int);
int __real_bind(int, const struct sockaddr *, socklen_t);
ssize_t __real_sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
ssize_t __real_recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
int __real_setsockopt(int, int, int, const void *, socklen_t);
int __real_getsockopt(int, int, int, void *, socklen_t *);
int __real_getsockname(int, struct sockaddr *, socklen_t *);
}

extern "C" {

bool Rebel_Net_Is_Socket(int fd) { return Get(fd) != nullptr; }

int __wrap_socket(int af, int type, int protocol) {
  if (af != AF_INET || (type & 0xff) != SOCK_DGRAM)
    return __real_socket(af, type, protocol);
  for (int i = 0; i < MAX_SOCKETS; i++) {
    if (!Sockets[i].used) {
      Sockets[i] = VSocket();
      Sockets[i].used = true;
      return FIRST_FD + i;
    }
  }
  errno = EMFILE;
  return -1;
}

int Rebel_Net_Close(int fd) {
  VSocket *s = Get(fd);
  if (!s)
    return -1;
  *s = VSocket();
  return 0;
}

int __wrap_bind(int fd, const struct sockaddr *addr, socklen_t len) {
  VSocket *s = Get(fd);
  if (!s)
    return __real_bind(fd, addr, len);
  if (!addr || len < (socklen_t)sizeof(sockaddr_in)) {
    errno = EINVAL;
    return -1;
  }
  if (s->port) {
    errno = EINVAL;
    return -1;
  }
  uint16_t port = ntohs(((const sockaddr_in *)addr)->sin_port);
  if (port == 0)
    return Bind_Ephemeral(s) ? 0 : (errno = EADDRINUSE, -1);
  if (Find_Bound(port)) {
    errno = EADDRINUSE;
    return -1;
  }
  s->port = port;
  return 0;
}

ssize_t __wrap_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen) {
  VSocket *s = Get(fd);
  if (!s)
    return __real_sendto(fd, buf, len, flags, to, tolen);
  if (!to || tolen < (socklen_t)sizeof(sockaddr_in)) {
    errno = EDESTADDRREQ;
    return -1;
  }
  if (len > 65507) {
    errno = EMSGSIZE;
    return -1;
  }
  if (!s->port && !Bind_Ephemeral(s)) {
    errno = EADDRINUSE;
    return -1;
  }
  const sockaddr_in *sin = (const sockaddr_in *)to;
  uint32_t ip = sin->sin_addr.s_addr;
  uint16_t port = ntohs(sin->sin_port);
  if (Is_Local(ip)) {
    Enqueue(LocalIp, s->port, port, (const uint8_t *)buf, (int)len);
  } else {
    // The page also loops broadcasts back to this endpoint's other sockets.
    Rebel_JS_Net_Send(ip, port, s->port, (const uint8_t *)buf, (int)len);
  }
  return (ssize_t)len;
}

ssize_t __wrap_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen) {
  VSocket *s = Get(fd);
  if (!s)
    return __real_recvfrom(fd, buf, len, flags, from, fromlen);
  Pump();
  if (s->queue.empty()) {
    errno = EAGAIN; // the engine only uses non-blocking datagram sockets
    return -1;
  }
  Datagram &d = s->queue.front();
  size_t n = d.data.size() < len ? d.data.size() : len;
  memcpy(buf, d.data.data(), n);
  if (from && fromlen && *fromlen >= (socklen_t)sizeof(sockaddr_in)) {
    sockaddr_in *sin = (sockaddr_in *)from;
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = d.from_ip;
    sin->sin_port = htons(d.from_port);
    *fromlen = sizeof(sockaddr_in);
  }
  if (!(flags & MSG_PEEK))
    s->queue.pop_front();
  return (ssize_t)n;
}

int Rebel_Net_Pending_Bytes(int fd) {
  VSocket *s = Get(fd);
  if (!s)
    return 0;
  Pump();
  return s->queue.empty() ? 0 : (int)s->queue.front().data.size();
}

int __wrap_setsockopt(int fd, int level, int name, const void *val, socklen_t len) {
  if (!Get(fd))
    return __real_setsockopt(fd, level, name, val, len);
  return 0; // SO_BROADCAST, buffer sizes: nothing to configure
}

int __wrap_getsockopt(int fd, int level, int name, void *val, socklen_t *len) {
  if (!Get(fd))
    return __real_getsockopt(fd, level, name, val, len);
  if (!val || !len || *len < (socklen_t)sizeof(int)) {
    errno = EINVAL;
    return -1;
  }
  // Buffer sizes (the engine reads them back after setting them): report
  // the queue's capacity in bytes, roughly.
  *(int *)val = (level == SOL_SOCKET && (name == SO_SNDBUF || name == SO_RCVBUF)) ? (int)(MAX_QUEUED * 512) : 0;
  *len = sizeof(int);
  return 0;
}

int __wrap_getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
  VSocket *s = Get(fd);
  if (!s)
    return __real_getsockname(fd, addr, len);
  if (!addr || !len || *len < (socklen_t)sizeof(sockaddr_in)) {
    errno = EINVAL;
    return -1;
  }
  Pump();
  sockaddr_in *sin = (sockaddr_in *)addr;
  memset(sin, 0, sizeof(*sin));
  sin->sin_family = AF_INET;
  sin->sin_addr.s_addr = LocalIp;
  sin->sin_port = htons(s->port);
  *len = sizeof(sockaddr_in);
  return 0;
}

int __wrap_gethostname(char *name, size_t len) {
  const char host[] = "rebel";
  if (!name || len < sizeof(host)) {
    errno = EINVAL;
    return -1;
  }
  memcpy(name, host, sizeof(host));
  return 0;
}

// The local host name resolves to this endpoint's virtual address; dotted
// quads resolve to themselves. There is no DNS.
struct hostent *__wrap_gethostbyname(const char *name) {
  static in_addr_t addr;
  static char *addr_list[2];
  static char *aliases[1];
  static char host_name[256];
  static hostent h;
  if (!name)
    return nullptr;
  in_addr_t a = inet_addr(name);
  if (a == INADDR_NONE) {
    if (strcmp(name, "rebel") != 0 && strcmp(name, "localhost") != 0)
      return nullptr;
    Pump();
    a = strcmp(name, "localhost") == 0 ? htonl(INADDR_LOOPBACK) : LocalIp;
  }
  addr = a;
  strncpy(host_name, name, sizeof(host_name) - 1);
  addr_list[0] = (char *)&addr;
  addr_list[1] = nullptr;
  aliases[0] = nullptr;
  h.h_name = host_name;
  h.h_aliases = aliases;
  h.h_addrtype = AF_INET;
  h.h_length = 4;
  h.h_addr_list = addr_list;
  return &h;
}

} // extern "C"
