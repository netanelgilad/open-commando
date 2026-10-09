/*
** Win32 file API over POSIX file descriptors. Paths go through the wrapped
** libc calls (path_resolve.cpp), so Windows spelling works everywhere.
*/
#include "windows.h"
#include "win32_handles.h"
#include "rebel_path.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

namespace {

// Read-only descriptors are pooled: opening and closing an OPFS access
// handle costs a few milliseconds each, and the engine reopens the same mix
// files for every asset. A closed read-only handle leaves its descriptor in
// the pool for the next open of that path; writes, deletes and renames of the
// path (path_resolve.cpp) close it.
struct FdPool {
  struct Entry {
    int fd = -1;
    int users = 0;
    unsigned long long last_use = 0;
    bool stale = false; // forgotten while in use: close on last release
  };
  std::mutex lock;
  std::map<std::string, Entry> entries;
  unsigned long long clock = 0;
  static constexpr size_t MAX_IDLE = 64;

  int Acquire(const std::string &key) {
    std::lock_guard<std::mutex> g(lock);
    auto it = entries.find(key);
    if (it == entries.end() || it->second.stale)
      return -1;
    it->second.users++;
    it->second.last_use = ++clock;
    return it->second.fd;
  }
  // Takes ownership of a freshly opened fd (already counted as one user).
  void Add(const std::string &key, int fd) {
    std::lock_guard<std::mutex> g(lock);
    Entry &e = entries[key];
    if (e.fd >= 0 && e.fd != fd) { // raced: keep the existing one as is
      ::close(fd);
      return;
    }
    e.fd = fd;
    e.users = 1;
    e.stale = false;
    e.last_use = ++clock;
    Trim();
  }
  void Release(const std::string &key, int fd) {
    std::lock_guard<std::mutex> g(lock);
    auto it = entries.find(key);
    if (it == entries.end() || it->second.fd != fd) {
      ::close(fd);
      return;
    }
    if (--it->second.users <= 0 && it->second.stale) {
      ::close(fd);
      entries.erase(it);
    }
    Trim();
  }
  void Forget(const std::string &key) {
    std::lock_guard<std::mutex> g(lock);
    auto it = entries.find(key);
    if (it == entries.end())
      return;
    if (it->second.users > 0) {
      it->second.stale = true;
      return;
    }
    ::close(it->second.fd);
    entries.erase(it);
  }
  void Trim() { // caller holds the lock
    size_t idle = 0;
    for (auto &e : entries)
      idle += e.second.users <= 0;
    while (idle > MAX_IDLE) {
      auto oldest = entries.end();
      for (auto it = entries.begin(); it != entries.end(); ++it)
        if (it->second.users <= 0 && (oldest == entries.end() || it->second.last_use < oldest->second.last_use))
          oldest = it;
      if (oldest == entries.end())
        break;
      ::close(oldest->second.fd);
      entries.erase(oldest);
      idle--;
    }
  }
};
FdPool &Pool() {
  static FdPool *p = new FdPool(); // construct on first use (static init order)
  return *p;
}

struct FileObject : RebelHandle {
  int fd = -1;
  std::string pool_key; // set when fd belongs to the read-only pool
  // Read-only handles read through a window buffer: the engine issues many
  // tiny reads, and each syscall is a round trip to the OPFS worker.
  bool read_only = false;
  long long pos = 0;      // logical file position (read-only handles)
  long long size = 0;
  std::vector<char> window;
  long long window_start = 0;
  size_t window_len = 0;
  static constexpr size_t WINDOW = 256 * 1024;
  FileObject() : RebelHandle(Kind::File) {}
  void Close() override {
    if (fd >= 0) {
      if (!pool_key.empty())
        Pool().Release(pool_key, fd);
      else
        ::close(fd);
    }
    fd = -1;
  }
  ~FileObject() override { Close(); }
};

struct FindObject : RebelHandle {
  std::string dir;
  std::vector<std::string> names;
  size_t next = 0;
  FindObject() : RebelHandle(Kind::Find) {}
};

FileObject *As_File(HANDLE h) {
  RebelHandle *rh = Rebel_Handle(h);
  return rh && rh->kind == RebelHandle::Kind::File ? static_cast<FileObject *>(rh) : nullptr;
}

void Fail_Errno() { SetLastError(Rebel_Errno_To_Win32(errno)); }

DWORD Attributes_From_Stat(const struct stat &st) {
  DWORD a = 0;
  if (S_ISDIR(st.st_mode))
    a |= FILE_ATTRIBUTE_DIRECTORY;
  if (!(st.st_mode & S_IWUSR))
    a |= FILE_ATTRIBUTE_READONLY;
  return a ? a : FILE_ATTRIBUTE_NORMAL;
}

void Fill_Find_Data(const std::string &dir, const std::string &name, LPWIN32_FIND_DATA data) {
  memset(data, 0, sizeof(*data));
  snprintf(data->cFileName, sizeof(data->cFileName), "%s", name.c_str());
  struct stat st;
  std::string full = dir.empty() ? name : dir + "/" + name;
  if (::stat(full.c_str(), &st) == 0) {
    data->dwFileAttributes = Attributes_From_Stat(st);
    data->nFileSizeLow = (DWORD)st.st_size;
    data->nFileSizeHigh = (DWORD)((unsigned long long)st.st_size >> 32);
    Rebel_Unix_To_FileTime(st.st_mtime, 0, &data->ftLastWriteTime);
    data->ftCreationTime = data->ftLastWriteTime;
    data->ftLastAccessTime = data->ftLastWriteTime;
  } else {
    data->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  }
}

bool Next_Match(FindObject *f, LPWIN32_FIND_DATA data) {
  if (f->next >= f->names.size())
    return false;
  Fill_Find_Data(f->dir, f->names[f->next++], data);
  return true;
}

} // namespace

extern "C" HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD,
                              HANDLE) {
  int flags = 0;
  if ((access & GENERIC_READ) && (access & GENERIC_WRITE))
    flags = O_RDWR;
  else if (access & GENERIC_WRITE)
    flags = O_WRONLY;
  else
    flags = O_RDONLY;
  switch (disposition) {
  case CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
  case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
  case OPEN_ALWAYS: flags |= O_CREAT; break;
  case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
  default: break;
  }
  int fd = -1;
  std::string pool_key;
  if (flags == O_RDONLY) {
    pool_key = Rebel_Resolve_Path(name);
    fd = Pool().Acquire(pool_key);
  }
  bool pooled = fd >= 0;
#ifdef __EMSCRIPTEN__
  // WasmFS serves read-only OPFS opens from a Blob: every read is an async
  // slice().arrayBuffer() round trip on the OPFS thread (the level loader
  // waited on those 60% of the time). Read-write opens get a synchronous
  // access handle instead, so plain reads open that way too; nothing is
  // written through it. Falls back if the file is locked (another tab).
  if (flags == O_RDONLY && fd < 0)
    fd = ::open(name, O_RDWR);
#endif
  if (fd < 0)
    fd = ::open(name, flags, 0644);
  if (fd < 0) {
    Fail_Errno();
    return INVALID_HANDLE_VALUE;
  }
  if (!pool_key.empty() && !pooled)
    Pool().Add(pool_key, fd);
  struct stat st{};
  // Windows only opens directories as files with FILE_FLAG_BACKUP_SEMANTICS,
  // which the engine never uses ("Data\\" + empty name must fail).
  if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
    if (!pool_key.empty()) {
      Pool().Release(pool_key, fd);
      Pool().Forget(pool_key);
    } else {
      ::close(fd);
    }
    SetLastError(ERROR_ACCESS_DENIED);
    return INVALID_HANDLE_VALUE;
  }
  FileObject *f = new FileObject();
  f->fd = fd;
  f->pool_key = pool_key;
  if ((flags & O_ACCMODE) == O_RDONLY) {
    f->read_only = true;
    f->size = (long long)st.st_size; // from the fstat above
  }
  SetLastError(ERROR_SUCCESS);
  return f;
}

extern "C" BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD read_out, LPOVERLAPPED) {
  FileObject *f = As_File(h);
  if (read_out)
    *read_out = 0;
  if (!f) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  if (f->read_only) {
    char *out = (char *)buf;
    size_t total = 0;
    while (total < n && f->pos < f->size) {
      if (f->pos >= f->window_start && f->pos < f->window_start + (long long)f->window_len) {
        size_t off = (size_t)(f->pos - f->window_start);
        size_t take = std::min(n - total, f->window_len - off);
        memcpy(out + total, f->window.data() + off, take);
        total += take;
        f->pos += (long long)take;
        continue;
      }
      size_t want = n - total;
      if (want >= FileObject::WINDOW) { // big read: straight into the caller's buffer
        ssize_t r = ::pread(f->fd, out + total, want, (off_t)f->pos);
        if (r <= 0)
          break;
        total += (size_t)r;
        f->pos += r;
        continue;
      }
      f->window.resize(FileObject::WINDOW);
      ssize_t r = ::pread(f->fd, f->window.data(), FileObject::WINDOW, (off_t)f->pos);
      if (r <= 0) {
        f->window_len = 0;
        break;
      }
      f->window_start = f->pos;
      f->window_len = (size_t)r;
    }
    if (read_out)
      *read_out = (DWORD)total;
    return TRUE;
  }
  size_t total = 0;
  while (total < n) {
    ssize_t r = ::read(f->fd, (char *)buf + total, n - total);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      Fail_Errno();
      return FALSE;
    }
    if (r == 0)
      break;
    total += (size_t)r;
  }
  if (read_out)
    *read_out = (DWORD)total;
  return TRUE;
}

extern "C" BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPOVERLAPPED) {
  FileObject *f = As_File(h);
  if (written)
    *written = 0;
  if (!f) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  size_t total = 0;
  while (total < n) {
    ssize_t w = ::write(f->fd, (const char *)buf + total, n - total);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      Fail_Errno();
      return FALSE;
    }
    total += (size_t)w;
  }
  if (written)
    *written = (DWORD)total;
  return TRUE;
}

extern "C" DWORD SetFilePointer(HANDLE h, LONG dist, PLONG disthigh, DWORD method) {
  FileObject *f = As_File(h);
  if (!f) {
    SetLastError(ERROR_INVALID_HANDLE);
    return INVALID_SET_FILE_POINTER;
  }
  long long off = disthigh ? (((long long)*disthigh) << 32) | (unsigned long)dist : (long long)dist;
  int whence = method == FILE_CURRENT ? SEEK_CUR : (method == FILE_END ? SEEK_END : SEEK_SET);
  if (f->read_only) {
    long long base = method == FILE_CURRENT ? f->pos : (method == FILE_END ? f->size : 0);
    long long np = base + off;
    if (np < 0) {
      SetLastError(87);
      return INVALID_SET_FILE_POINTER;
    }
    f->pos = np;
    if (disthigh)
      *disthigh = (LONG)((unsigned long long)np >> 32);
    SetLastError(ERROR_SUCCESS);
    return (DWORD)np;
  }
  off_t pos = ::lseek(f->fd, (off_t)off, whence);
  if (pos < 0) {
    Fail_Errno();
    return INVALID_SET_FILE_POINTER;
  }
  if (disthigh)
    *disthigh = (LONG)((unsigned long long)pos >> 32);
  SetLastError(ERROR_SUCCESS);
  return (DWORD)pos;
}

extern "C" BOOL SetEndOfFile(HANDLE h) {
  FileObject *f = As_File(h);
  if (!f)
    return FALSE;
  off_t pos = ::lseek(f->fd, 0, SEEK_CUR);
  return ftruncate(f->fd, pos) == 0;
}

extern "C" DWORD GetFileSize(HANDLE h, LPDWORD high) {
  FileObject *f = As_File(h);
  if (f && f->read_only) { // size known at open; fstat is an OPFS round trip
    if (high)
      *high = (DWORD)((unsigned long long)f->size >> 32);
    return (DWORD)f->size;
  }
  struct stat st;
  if (!f || fstat(f->fd, &st) != 0) {
    SetLastError(ERROR_INVALID_HANDLE);
    return INVALID_FILE_SIZE;
  }
  if (high)
    *high = (DWORD)((unsigned long long)st.st_size >> 32);
  return (DWORD)st.st_size;
}

extern "C" BOOL GetFileTime(HANDLE h, LPFILETIME c, LPFILETIME a, LPFILETIME w) {
  FileObject *f = As_File(h);
  struct stat st;
  if (!f || fstat(f->fd, &st) != 0)
    return FALSE;
  FILETIME ft;
  Rebel_Unix_To_FileTime(st.st_mtime, 0, &ft);
  if (c)
    *c = ft;
  if (a)
    *a = ft;
  if (w)
    *w = ft;
  return TRUE;
}

extern "C" BOOL SetFileTime(HANDLE, const FILETIME *, const FILETIME *, const FILETIME *) { return TRUE; }

extern "C" BOOL GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info) {
  FileObject *f = As_File(h);
  struct stat st;
  if (!f || fstat(f->fd, &st) != 0)
    return FALSE;
  memset(info, 0, sizeof(*info));
  info->dwFileAttributes = Attributes_From_Stat(st);
  Rebel_Unix_To_FileTime(st.st_mtime, 0, &info->ftLastWriteTime);
  info->ftCreationTime = info->ftLastAccessTime = info->ftLastWriteTime;
  info->nFileSizeLow = (DWORD)st.st_size;
  info->nFileSizeHigh = (DWORD)((unsigned long long)st.st_size >> 32);
  info->nNumberOfLinks = 1;
  info->nFileIndexLow = (DWORD)st.st_ino;
  return TRUE;
}

extern "C" BOOL FlushFileBuffers(HANDLE h) {
  FileObject *f = As_File(h);
  return f && fsync(f->fd) == 0;
}

extern "C" BOOL DeleteFileA(LPCSTR name) {
  if (::unlink(name) != 0) {
    Fail_Errno();
    return FALSE;
  }
  return TRUE;
}

extern "C" BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
  int in = ::open(from, O_RDONLY);
  if (in < 0) {
    Fail_Errno();
    return FALSE;
  }
  int out = ::open(to, O_WRONLY | O_CREAT | (fail_if_exists ? O_EXCL : O_TRUNC), 0644);
  if (out < 0) {
    Fail_Errno();
    ::close(in);
    return FALSE;
  }
  char buf[65536];
  ssize_t r;
  bool ok = true;
  while ((r = ::read(in, buf, sizeof(buf))) > 0)
    if (::write(out, buf, (size_t)r) != r) {
      ok = false;
      break;
    }
  ::close(in);
  ::close(out);
  return ok && r >= 0;
}

extern "C" BOOL MoveFileA(LPCSTR from, LPCSTR to) {
  if (::rename(from, to) != 0) {
    Fail_Errno();
    return FALSE;
  }
  return TRUE;
}

extern "C" DWORD GetFileAttributesA(LPCSTR name) {
  struct stat st;
  if (::stat(name, &st) != 0) {
    Fail_Errno();
    return INVALID_FILE_ATTRIBUTES;
  }
  return Attributes_From_Stat(st);
}

extern "C" BOOL SetFileAttributesA(LPCSTR name, DWORD attr) {
  struct stat st;
  if (::stat(name, &st) != 0)
    return FALSE;
  mode_t mode = st.st_mode;
  if (attr & FILE_ATTRIBUTE_READONLY)
    mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
  else
    mode |= S_IWUSR;
  return chmod(Rebel_Resolve_Path(name).c_str(), mode) == 0;
}

extern "C" BOOL CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES) {
  if (::mkdir(name, 0755) != 0) {
    Fail_Errno();
    return FALSE;
  }
  return TRUE;
}

extern "C" BOOL RemoveDirectoryA(LPCSTR name) {
  if (::rmdir(name) != 0) {
    Fail_Errno();
    return FALSE;
  }
  return TRUE;
}

extern "C" DWORD GetCurrentDirectoryA(DWORD len, LPSTR buf) {
  char tmp[MAX_PATH];
  if (!getcwd(tmp, sizeof(tmp)))
    return 0;
  DWORD n = (DWORD)strlen(tmp);
  if (!buf || len <= n)
    return n + 1;
  memcpy(buf, tmp, n + 1);
  return n;
}

extern "C" BOOL SetCurrentDirectoryA(LPCSTR dir) {
  if (::chdir(dir) != 0) {
    Fail_Errno();
    return FALSE;
  }
  return TRUE;
}

extern "C" DWORD GetFullPathNameA(LPCSTR name, DWORD len, LPSTR buf, LPSTR *filepart) {
  char tmp[MAX_PATH * 2];
  _fullpath(tmp, name, sizeof(tmp));
  DWORD n = (DWORD)strlen(tmp);
  if (!buf || len <= n)
    return n + 1;
  memcpy(buf, tmp, n + 1);
  if (filepart) {
    char *slash = nullptr;
    for (char *p = buf; *p; ++p)
      if (*p == '/' || *p == '\\')
        slash = p;
    *filepart = slash ? slash + 1 : buf;
  }
  return n;
}

extern "C" DWORD GetTempPathA(DWORD len, LPSTR buf) {
  const char *t = "/tmp/";
  DWORD n = (DWORD)strlen(t);
  if (!buf || len <= n)
    return n + 1;
  memcpy(buf, t, n + 1);
  return n;
}

extern "C" UINT GetTempFileNameA(LPCSTR path, LPCSTR prefix, UINT unique, LPSTR out) {
  static UINT counter = 1;
  UINT u = unique ? unique : counter++;
  snprintf(out, MAX_PATH, "%s/%.3s%04X.tmp", path ? path : "/tmp", prefix ? prefix : "", u & 0xFFFF);
  if (!unique) {
    int fd = ::open(out, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0)
      ::close(fd);
  }
  return u;
}

extern "C" HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATA data) {
  std::string p(pattern ? pattern : "");
  for (char &c : p)
    if (c == '\\')
      c = '/';
  size_t slash = p.rfind('/');
  std::string dir = slash == std::string::npos ? "" : p.substr(0, slash);
  std::string mask = slash == std::string::npos ? p : p.substr(slash + 1);
  if (mask == "*.*")
    mask = "*";

  std::string real_dir = Rebel_Resolve_Path(dir.empty() ? "." : dir.c_str());
  DIR *d = ::opendir(real_dir.c_str());
  if (!d) {
    SetLastError(ERROR_PATH_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  FindObject *f = new FindObject();
  f->dir = real_dir;
  while (dirent *e = readdir(d)) {
    if (fnmatch(mask.c_str(), e->d_name, FNM_CASEFOLD) == 0)
      f->names.push_back(e->d_name);
  }
  closedir(d);
  if (!Next_Match(f, data)) {
    Rebel_Handle_Release(f);
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  return f;
}

extern "C" BOOL FindNextFileA(HANDLE h, LPWIN32_FIND_DATA data) {
  RebelHandle *rh = Rebel_Handle(h);
  if (!rh || rh->kind != RebelHandle::Kind::Find) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  if (!Next_Match(static_cast<FindObject *>(rh), data)) {
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
  }
  return TRUE;
}

extern "C" BOOL FindClose(HANDLE h) {
  RebelHandle *rh = Rebel_Handle(h);
  if (!rh || rh->kind != RebelHandle::Kind::Find)
    return FALSE;
  Rebel_Handle_Release(rh);
  return TRUE;
}

extern "C" UINT GetDriveTypeA(LPCSTR) { return DRIVE_FIXED; }
extern "C" DWORD GetLogicalDrives(void) { return 1u << 2; } // C:
extern "C" BOOL GetDiskFreeSpaceA(LPCSTR, LPDWORD spc, LPDWORD bps, LPDWORD freec, LPDWORD totalc) {
  // Report 1GB free so "not enough disk space" checks pass.
  if (spc)
    *spc = 8;
  if (bps)
    *bps = 512;
  if (freec)
    *freec = 262144;
  if (totalc)
    *totalc = 524288;
  return TRUE;
}
extern "C" BOOL GetVolumeInformationA(LPCSTR, LPSTR name, DWORD namelen, LPDWORD serial, LPDWORD maxlen,
                                      LPDWORD flags, LPSTR fsname, DWORD fsnamelen) {
  if (name && namelen)
    snprintf(name, namelen, "RENEGADE");
  if (serial)
    *serial = 0x52454E47;
  if (maxlen)
    *maxlen = 255;
  if (flags)
    *flags = 0;
  if (fsname && fsnamelen)
    snprintf(fsname, fsnamelen, "WEBFS");
  return TRUE;
}

void Rebel_File_Pool_Forget(const std::string &resolved_path) { Pool().Forget(resolved_path); }
