/*
** Windows path semantics on a POSIX filesystem.
**
** The engine passes paths like "Data\\Always.dat" and relies on Windows'
** case-insensitive lookup. The Emscripten filesystem is case-sensitive and
** only understands '/', so every libc entry point that takes a path is
** wrapped at link time (-Wl,--wrap=<fn>, see Platform/CMakeLists.txt) and
** routed through Rebel_Resolve_Path() first.
**
** Resolution: backslashes become '/', drive letters ("C:") are dropped, then
** each component is matched case-insensitively against the real directory
** entries. Components that do not exist yet (new files) keep their case.
*/
#include "rebel_path.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
FILE *__real_fopen(const char *, const char *);
FILE *__real_freopen(const char *, const char *, FILE *);
int __real_open(const char *, int, ...);
int __real_stat(const char *, struct stat *);
int __real_lstat(const char *, struct stat *);
int __real_access(const char *, int);
DIR *__real_opendir(const char *);
int __real_mkdir(const char *, mode_t);
int __real_rmdir(const char *);
int __real_unlink(const char *);
int __real_remove(const char *);
int __real_rename(const char *, const char *);
int __real_chdir(const char *);
int __real_truncate(const char *, off_t);
}

static bool Needs_Work(const char *path) {
  if (!path || !*path)
    return false;
  if (strchr(path, '\\') || (path[0] && path[1] == ':'))
    return true;
  // Absolute paths outside the persistent mount (/rebel, /dev, /tmp...) are
  // in-memory and already exact. Everything else goes through the cached
  // listings, which costs no OPFS round trip once a directory is known.
  if (path[0] == '/' && strncmp(path, "/opfs", 5) != 0)
    return false;
  return true;
}

// Directory listings are cached (each listing is a round trip to OPFS) and
// dropped whenever anything is created, removed or renamed.
static std::mutex &Cache_Lock() {
  static std::mutex *m = new std::mutex();
  return *m;
}
static std::map<std::string, std::vector<std::string>> &Dir_Cache() {
  static auto *c = new std::map<std::string, std::vector<std::string>>();
  return *c;
}
static void Invalidate_Dir_Cache() {
  std::lock_guard<std::mutex> g(Cache_Lock());
  Dir_Cache().clear();
}

// `listed` is set when the directory could be listed (so a miss is real).
static bool Match_Component(const std::string &dir, const std::string &name, std::string &out, bool *listed) {
  std::string key = dir.empty() ? "." : dir;
  std::vector<std::string> names;
  {
    std::lock_guard<std::mutex> g(Cache_Lock());
    auto it = Dir_Cache().find(key);
    if (it != Dir_Cache().end())
      names = it->second;
  }
  bool have = !names.empty();
  if (!have) {
    std::lock_guard<std::mutex> g(Cache_Lock());
    have = Dir_Cache().count(key) != 0; // an empty directory listed before
  }
  if (!have) {
    DIR *d = __real_opendir(key.c_str());
    if (!d)
      return false;
    while (dirent *e = readdir(d))
      names.push_back(e->d_name);
    closedir(d);
    std::lock_guard<std::mutex> g(Cache_Lock());
    Dir_Cache()[key] = names;
  }
  if (listed)
    *listed = true;
  for (const std::string &n : names)
    if (strcasecmp(n.c_str(), name.c_str()) == 0) {
      out = n;
      return true;
    }
  return false;
}

std::string Rebel_Resolve_Path(const char *path) { return Rebel_Resolve_Path(path, nullptr); }

std::string Rebel_Resolve_Path(const char *path, bool *known_missing) {
  if (known_missing)
    *known_missing = false;
  if (!path)
    return std::string();
  if (!Needs_Work(path))
    return std::string(path);

  std::string p(path);
  for (char &c : p)
    if (c == '\\')
      c = '/';
  if (p.size() >= 2 && p[1] == ':') // drop drive letter
    p.erase(0, 2);

  const bool absolute = !p.empty() && p[0] == '/';
  std::string result = absolute ? "/" : "";
  bool missing = false;
  size_t i = absolute ? 1 : 0;
  while (i <= p.size()) {
    size_t j = p.find('/', i);
    if (j == std::string::npos)
      j = p.size();
    std::string comp = p.substr(i, j - i);
    i = j + 1;
    if (comp.empty() || comp == ".")
      continue;
    std::string base = result;
    if (!base.empty() && base.back() == '/')
      base.pop_back();
    std::string actual = comp;
    if (comp != ".." && !missing) {
      bool listed = false;
      if (!Match_Component(base.empty() && absolute ? "/" : base, comp, actual, &listed)) {
        missing = true; // keep caller's spelling for the rest
        if (known_missing && listed)
          *known_missing = true;
      }
    }
    if (!result.empty() && result.back() != '/')
      result += '/';
    result += actual;
  }
  if (result.empty())
    result = ".";
  return result;
}

extern "C" {

FILE *__wrap_fopen(const char *path, const char *mode) {
  bool missing = false;
  std::string p = Rebel_Resolve_Path(path, &missing);
  bool writes = mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
  if (missing && !(mode && (strchr(mode, 'w') || strchr(mode, 'a')))) {
    errno = ENOENT;
    return nullptr;
  }
  if (writes)
    Rebel_File_Pool_Forget(p);
  FILE *f = __real_fopen(p.c_str(), mode);
  if (mode && (strchr(mode, 'w') || strchr(mode, 'a')))
    Invalidate_Dir_Cache();
  return f;
}
FILE *__wrap_freopen(const char *path, const char *mode, FILE *f) {
  return __real_freopen(path ? Rebel_Resolve_Path(path).c_str() : nullptr, mode, f);
}
int __wrap_open(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
  }
  bool missing = false;
  std::string p = Rebel_Resolve_Path(path, &missing);
  if (missing && !(flags & O_CREAT)) {
    errno = ENOENT;
    return -1;
  }
  if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC))
    Rebel_File_Pool_Forget(p);
  int fd = __real_open(p.c_str(), flags, mode);
  if (flags & O_CREAT)
    Invalidate_Dir_Cache();
  return fd;
}
// Lookups of paths known to be absent fail here, without an OPFS round trip.
#define REBEL_RESOLVE_OR_ENOENT(path, var)                                                                             \
  bool var##_missing = false;                                                                                          \
  std::string var = Rebel_Resolve_Path(path, &var##_missing);                                                          \
  if (var##_missing) {                                                                                                 \
    errno = ENOENT;                                                                                                    \
    return -1;                                                                                                         \
  }
int __wrap_stat(const char *path, struct stat *st) {
  REBEL_RESOLVE_OR_ENOENT(path, p);
  return __real_stat(p.c_str(), st);
}
int __wrap_lstat(const char *path, struct stat *st) {
  REBEL_RESOLVE_OR_ENOENT(path, p);
  return __real_lstat(p.c_str(), st);
}
int __wrap_access(const char *path, int mode) {
  REBEL_RESOLVE_OR_ENOENT(path, p);
  return __real_access(p.c_str(), mode);
}
DIR *__wrap_opendir(const char *path) { return __real_opendir(Rebel_Resolve_Path(path).c_str()); }
int __wrap_mkdir(const char *path, mode_t mode) { int r = __real_mkdir(Rebel_Resolve_Path(path).c_str(), mode);
  Invalidate_Dir_Cache();
  return r;
}
int __wrap_rmdir(const char *path) { int r = __real_rmdir(Rebel_Resolve_Path(path).c_str());
  Invalidate_Dir_Cache();
  return r;
}
int __wrap_unlink(const char *path) {
  std::string p = Rebel_Resolve_Path(path);
  Rebel_File_Pool_Forget(p);
  int r = __real_unlink(p.c_str());
  Invalidate_Dir_Cache();
  return r;
}
int __wrap_remove(const char *path) {
  std::string p = Rebel_Resolve_Path(path);
  Rebel_File_Pool_Forget(p);
  int r = __real_remove(p.c_str());
  Invalidate_Dir_Cache();
  return r;
}
int __wrap_rename(const char *a, const char *b) {
  std::string pa = Rebel_Resolve_Path(a), pb = Rebel_Resolve_Path(b);
  Rebel_File_Pool_Forget(pa);
  Rebel_File_Pool_Forget(pb);
  int r = __real_rename(pa.c_str(), pb.c_str());
  Invalidate_Dir_Cache();
  return r;
}
int __wrap_chdir(const char *path) { return __real_chdir(Rebel_Resolve_Path(path).c_str()); }
int __wrap_truncate(const char *path, off_t len) {
  std::string p = Rebel_Resolve_Path(path);
  Rebel_File_Pool_Forget(p);
  return __real_truncate(p.c_str(), len);
}

} // extern "C"
