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
#include <string>
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
  // Fast path: nothing Windows-ish and the exact path exists.
  if (!path || !*path)
    return false;
  if (strchr(path, '\\') || (path[0] && path[1] == ':'))
    return true;
  struct stat st;
  return __real_stat(path, &st) != 0;
}

static bool Match_Component(const std::string &dir, const std::string &name, std::string &out) {
  DIR *d = __real_opendir(dir.empty() ? "." : dir.c_str());
  if (!d)
    return false;
  bool found = false;
  while (dirent *e = readdir(d)) {
    if (strcasecmp(e->d_name, name.c_str()) == 0) {
      out = e->d_name;
      found = true;
      break;
    }
  }
  closedir(d);
  return found;
}

std::string Rebel_Resolve_Path(const char *path) {
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
      if (!Match_Component(base.empty() && absolute ? "/" : base, comp, actual))
        missing = true; // keep caller's spelling for the rest
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

FILE *__wrap_fopen(const char *path, const char *mode) { return __real_fopen(Rebel_Resolve_Path(path).c_str(), mode); }
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
  return __real_open(Rebel_Resolve_Path(path).c_str(), flags, mode);
}
int __wrap_stat(const char *path, struct stat *st) { return __real_stat(Rebel_Resolve_Path(path).c_str(), st); }
int __wrap_lstat(const char *path, struct stat *st) { return __real_lstat(Rebel_Resolve_Path(path).c_str(), st); }
int __wrap_access(const char *path, int mode) { return __real_access(Rebel_Resolve_Path(path).c_str(), mode); }
DIR *__wrap_opendir(const char *path) { return __real_opendir(Rebel_Resolve_Path(path).c_str()); }
int __wrap_mkdir(const char *path, mode_t mode) { return __real_mkdir(Rebel_Resolve_Path(path).c_str(), mode); }
int __wrap_rmdir(const char *path) { return __real_rmdir(Rebel_Resolve_Path(path).c_str()); }
int __wrap_unlink(const char *path) { return __real_unlink(Rebel_Resolve_Path(path).c_str()); }
int __wrap_remove(const char *path) { return __real_remove(Rebel_Resolve_Path(path).c_str()); }
int __wrap_rename(const char *a, const char *b) {
  return __real_rename(Rebel_Resolve_Path(a).c_str(), Rebel_Resolve_Path(b).c_str());
}
int __wrap_chdir(const char *path) { return __real_chdir(Rebel_Resolve_Path(path).c_str()); }
int __wrap_truncate(const char *path, off_t len) { return __real_truncate(Rebel_Resolve_Path(path).c_str(), len); }

} // extern "C"
