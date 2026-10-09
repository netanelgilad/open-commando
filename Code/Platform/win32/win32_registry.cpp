/*
** Win32 registry replacement.
**
** Renegade keeps its settings under HKLM/HKCU\Software\Westwood\Renegade.
** Keys are case-insensitive paths; values keep their type and raw bytes.
** The store is a single text file (REBEL_REGISTRY_PATH, default
** /persist/registry.txt) rewritten on every change; on the web /persist is
** an IndexedDB-backed mount synced by the page.
**
** File format, one value per line:
**   <key path>\t<value name>\t<type>\t<hex bytes>
*/
#include "windows.h"
#include "win32_handles.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

struct Value {
  DWORD type = REG_NONE;
  std::vector<BYTE> data;
};

struct NoCase {
  bool operator()(const std::string &a, const std::string &b) const { return strcasecmp(a.c_str(), b.c_str()) < 0; }
};

using ValueMap = std::map<std::string, Value, NoCase>;
using KeyMap = std::map<std::string, ValueMap, NoCase>;

// Built on first use: game code runs before main() (static constructors)
// and may reach this before this file's globals are initialized.
std::recursive_mutex &Lock_Instance() {
  static std::recursive_mutex *instance = new std::recursive_mutex();
  return *instance;
}
#define Lock Lock_Instance()
// Built on first use: game code runs before main() (static constructors)
// and may reach this before this file's globals are initialized.
KeyMap &Keys_Instance() {
  static KeyMap *instance = new KeyMap();
  return *instance;
}
#define Keys Keys_Instance()
bool Loaded = false;

const char *Store_Path() {
  const char *p = getenv("REBEL_REGISTRY_PATH");
  return p ? p : "/persist/registry.txt";
}

std::string Root_Name(HKEY root) {
  switch ((ULONG_PTR)root) {
  case 0x80000000UL: return "HKCR";
  case 0x80000001UL: return "HKCU";
  case 0x80000002UL: return "HKLM";
  case 0x80000003UL: return "HKU";
  default: return "";
  }
}

struct KeyObject : RebelHandle {
  std::string path;
  KeyObject() : RebelHandle(Kind::RegistryKey) {}
};

std::string Normalize(std::string s) {
  for (char &c : s)
    if (c == '/')
      c = '\\';
  while (!s.empty() && s.back() == '\\')
    s.pop_back();
  while (!s.empty() && s.front() == '\\')
    s.erase(0, 1);
  return s;
}

// Resolves an HKEY (predefined root or opened key) plus subkey to a path.
bool Key_Path(HKEY key, LPCSTR sub, std::string &out) {
  std::string base = Root_Name(key);
  if (base.empty()) {
    RebelHandle *rh = Rebel_Handle((HANDLE)key);
    if (!rh || rh->kind != RebelHandle::Kind::RegistryKey)
      return false;
    base = static_cast<KeyObject *>(rh)->path;
  }
  std::string s = sub ? Normalize(sub) : "";
  out = s.empty() ? base : base + "\\" + s;
  return true;
}

void Load() {
  if (Loaded)
    return;
  Loaded = true;
  FILE *f = fopen(Store_Path(), "rb");
  if (!f)
    return;
  char line[16384];
  while (fgets(line, sizeof(line), f)) {
    char *tab1 = strchr(line, '\t');
    if (!tab1)
      continue;
    char *tab2 = strchr(tab1 + 1, '\t');
    if (!tab2)
      continue;
    char *tab3 = strchr(tab2 + 1, '\t');
    if (!tab3)
      continue;
    *tab1 = *tab2 = *tab3 = 0;
    Value v;
    v.type = (DWORD)strtoul(tab2 + 1, nullptr, 10);
    for (char *p = tab3 + 1; isxdigit((unsigned char)p[0]) && isxdigit((unsigned char)p[1]); p += 2) {
      char hex[3] = {p[0], p[1], 0};
      v.data.push_back((BYTE)strtoul(hex, nullptr, 16));
    }
    Keys[line][tab1 + 1] = v;
  }
  fclose(f);
}

// Written to a temporary file and renamed over the store, so a tab closed
// mid-write can't leave a truncated registry (all settings lost).
void Save() {
  std::string tmp = std::string(Store_Path()) + ".tmp";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f)
    return; // no persistent mount (e.g. node tests): keep in memory only
  for (auto &k : Keys) {
    if (k.second.empty()) {
      fprintf(f, "%s\t\t0\t\n", k.first.c_str()); // keep empty keys alive
      continue;
    }
    for (auto &v : k.second) {
      fprintf(f, "%s\t%s\t%lu\t", k.first.c_str(), v.first.c_str(), (unsigned long)v.second.type);
      for (BYTE b : v.second.data)
        fprintf(f, "%02x", b);
      fputc('\n', f);
    }
  }
  bool ok = fflush(f) == 0 && !ferror(f);
  fclose(f);
  if (!ok || rename(tmp.c_str(), Store_Path()) != 0) {
    fprintf(stderr, "[registry] could not save %s\n", Store_Path());
    remove(tmp.c_str());
  }
}

bool Key_Exists(const std::string &path) {
  if (Keys.count(path))
    return true;
  // A key also exists if any descendant does.
  std::string prefix = path + "\\";
  for (auto &k : Keys)
    if (k.first.size() > prefix.size() && strncasecmp(k.first.c_str(), prefix.c_str(), prefix.size()) == 0)
      return true;
  return false;
}

LONG Open(HKEY key, LPCSTR sub, bool create, PHKEY out, LPDWORD disp) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  Load();
  std::string path;
  if (!Key_Path(key, sub, path))
    return ERROR_INVALID_HANDLE;
  bool exists = Key_Exists(path);
  if (!exists && !create)
    return ERROR_FILE_NOT_FOUND;
  if (!exists) {
    Keys[path];
    Save();
  }
  if (disp)
    *disp = exists ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
  KeyObject *k = new KeyObject();
  k->path = path;
  *out = (HKEY)k;
  return ERROR_SUCCESS;
}

std::vector<std::string> Subkeys(const std::string &path) {
  std::set<std::string, NoCase> names;
  std::string prefix = path + "\\";
  for (auto &k : Keys) {
    if (k.first.size() <= prefix.size() || strncasecmp(k.first.c_str(), prefix.c_str(), prefix.size()) != 0)
      continue;
    std::string rest = k.first.substr(prefix.size());
    names.insert(rest.substr(0, rest.find('\\')));
  }
  return std::vector<std::string>(names.begin(), names.end());
}

std::wstring Wide(LPCWSTR s) { return s ? std::wstring(s) : std::wstring(); }
std::string Narrow(LPCWSTR s) {
  std::string out;
  for (const wchar_t *p = s; p && *p; ++p)
    out.push_back(*p < 0x80 ? (char)*p : '?');
  return out;
}

} // namespace

extern "C" LONG RegOpenKeyExA(HKEY key, LPCSTR sub, DWORD, REGSAM, PHKEY out) {
  return Open(key, sub, false, out, nullptr);
}
extern "C" LONG RegOpenKeyA(HKEY key, LPCSTR sub, PHKEY out) { return Open(key, sub, false, out, nullptr); }
extern "C" LONG RegCreateKeyExA(HKEY key, LPCSTR sub, DWORD, LPSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY out,
                                LPDWORD disp) {
  return Open(key, sub, true, out, disp);
}
extern "C" LONG RegCreateKeyA(HKEY key, LPCSTR sub, PHKEY out) { return Open(key, sub, true, out, nullptr); }

extern "C" LONG RegCloseKey(HKEY key) {
  if (!Root_Name(key).empty())
    return ERROR_SUCCESS;
  return CloseHandle((HANDLE)key) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
}

extern "C" LONG RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD, LPDWORD type, LPBYTE data, LPDWORD size) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  Load();
  std::string path;
  if (!Key_Path(key, nullptr, path))
    return ERROR_INVALID_HANDLE;
  auto k = Keys.find(path);
  if (k == Keys.end())
    return ERROR_FILE_NOT_FOUND;
  auto v = k->second.find(name ? name : "");
  if (v == k->second.end())
    return ERROR_FILE_NOT_FOUND;
  if (type)
    *type = v->second.type;
  DWORD need = (DWORD)v->second.data.size();
  if (!size)
    return data ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
  if (!data) {
    *size = need;
    return ERROR_SUCCESS;
  }
  if (*size < need) {
    *size = need;
    return ERROR_MORE_DATA;
  }
  memcpy(data, v->second.data.data(), need);
  *size = need;
  return ERROR_SUCCESS;
}

extern "C" LONG RegSetValueExA(HKEY key, LPCSTR name, DWORD, DWORD type, const BYTE *data, DWORD size) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  Load();
  std::string path;
  if (!Key_Path(key, nullptr, path))
    return ERROR_INVALID_HANDLE;
  Value v;
  v.type = type;
  if (data && size)
    v.data.assign(data, data + size);
  Keys[path][name ? name : ""] = v;
  Save();
  return ERROR_SUCCESS;
}

// Wide variants store REG_SZ as UTF-16LE bytes, matching what Windows stores.
extern "C" LONG RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  DWORD t = 0, raw_size = 0;
  std::string n = Narrow(name);
  LONG r = RegQueryValueExA(key, n.c_str(), reserved, &t, nullptr, &raw_size);
  if (r != ERROR_SUCCESS)
    return r;
  if (type)
    *type = t;
  if (t != REG_SZ && t != REG_EXPAND_SZ && t != REG_MULTI_SZ)
    return RegQueryValueExA(key, n.c_str(), reserved, type, data, size);
  // Convert stored UTF-16LE to the in-memory wchar_t (32-bit) layout.
  std::vector<BYTE> raw(raw_size);
  RegQueryValueExA(key, n.c_str(), reserved, &t, raw.data(), &raw_size);
  DWORD chars = raw_size / 2;
  DWORD need = chars * sizeof(wchar_t);
  if (!size)
    return ERROR_SUCCESS;
  // Callers size buffers in Windows units (2 bytes/char): report that.
  if (!data) {
    *size = raw_size;
    return ERROR_SUCCESS;
  }
  DWORD capacity_chars = *size / 2;
  if (capacity_chars < chars) {
    *size = raw_size;
    return ERROR_MORE_DATA;
  }
  wchar_t *out = (wchar_t *)data;
  for (DWORD i = 0; i < chars; i++)
    out[i] = (wchar_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
  (void)need;
  *size = raw_size;
  return ERROR_SUCCESS;
}

extern "C" LONG RegSetValueExW(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD size) {
  std::string n = Narrow(name);
  if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ)
    return RegSetValueExA(key, n.c_str(), reserved, type, data, size);
  // `size` is in Windows bytes (2 per char); the buffer holds 32-bit wchar_t.
  const wchar_t *w = (const wchar_t *)data;
  DWORD chars = size / 2;
  std::vector<BYTE> raw(chars * 2);
  for (DWORD i = 0; i < chars; i++) {
    raw[2 * i] = (BYTE)(w[i] & 0xFF);
    raw[2 * i + 1] = (BYTE)((w[i] >> 8) & 0xFF);
  }
  return RegSetValueExA(key, n.c_str(), reserved, type, raw.data(), (DWORD)raw.size());
}

extern "C" LONG RegDeleteValueA(HKEY key, LPCSTR name) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  Load();
  std::string path;
  if (!Key_Path(key, nullptr, path))
    return ERROR_INVALID_HANDLE;
  auto k = Keys.find(path);
  if (k == Keys.end() || !k->second.erase(name ? name : ""))
    return ERROR_FILE_NOT_FOUND;
  Save();
  return ERROR_SUCCESS;
}

extern "C" LONG RegDeleteKeyA(HKEY key, LPCSTR sub) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  Load();
  std::string path;
  if (!Key_Path(key, sub, path))
    return ERROR_INVALID_HANDLE;
  if (!Subkeys(path).empty())
    return ERROR_ACCESS_DENIED; // Win32 refuses to delete keys with children
  if (!Keys.erase(path))
    return ERROR_FILE_NOT_FOUND;
  Save();
  return ERROR_SUCCESS;
}

extern "C" LONG RegEnumValueA(HKEY key, DWORD index, LPSTR name, LPDWORD namelen, LPDWORD, LPDWORD type, LPBYTE data,
                              LPDWORD datalen) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  Load();
  std::string path;
  if (!Key_Path(key, nullptr, path))
    return ERROR_INVALID_HANDLE;
  auto k = Keys.find(path);
  if (k == Keys.end() || index >= k->second.size())
    return ERROR_NO_MORE_ITEMS;
  auto v = k->second.begin();
  std::advance(v, index);
  if (*namelen <= v->first.size())
    return ERROR_MORE_DATA;
  strcpy(name, v->first.c_str());
  *namelen = (DWORD)v->first.size();
  if (type)
    *type = v->second.type;
  if (datalen) {
    DWORD need = (DWORD)v->second.data.size();
    if (data) {
      if (*datalen < need) {
        *datalen = need;
        return ERROR_MORE_DATA;
      }
      memcpy(data, v->second.data.data(), need);
    }
    *datalen = need;
  }
  return ERROR_SUCCESS;
}

extern "C" LONG RegEnumKeyExA(HKEY key, DWORD index, LPSTR name, LPDWORD namelen, LPDWORD, LPSTR, LPDWORD,
                              LPFILETIME) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  Load();
  std::string path;
  if (!Key_Path(key, nullptr, path))
    return ERROR_INVALID_HANDLE;
  std::vector<std::string> subs = Subkeys(path);
  if (index >= subs.size())
    return ERROR_NO_MORE_ITEMS;
  if (*namelen <= subs[index].size())
    return ERROR_MORE_DATA;
  strcpy(name, subs[index].c_str());
  *namelen = (DWORD)subs[index].size();
  return ERROR_SUCCESS;
}

extern "C" LONG RegEnumKeyA(HKEY key, DWORD index, LPSTR name, DWORD namelen) {
  return RegEnumKeyExA(key, index, name, &namelen, nullptr, nullptr, nullptr, nullptr);
}

extern "C" LONG RegQueryInfoKeyA(HKEY key, LPSTR, LPDWORD, LPDWORD, LPDWORD subkeys, LPDWORD maxsubkeylen, LPDWORD,
                                 LPDWORD values, LPDWORD maxvaluenamelen, LPDWORD maxvaluelen, LPDWORD, LPFILETIME) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  Load();
  std::string path;
  if (!Key_Path(key, nullptr, path))
    return ERROR_INVALID_HANDLE;
  std::vector<std::string> subs = Subkeys(path);
  if (subkeys)
    *subkeys = (DWORD)subs.size();
  if (maxsubkeylen) {
    size_t m = 0;
    for (auto &s : subs)
      m = std::max(m, s.size());
    *maxsubkeylen = (DWORD)m;
  }
  auto k = Keys.find(path);
  size_t count = 0, maxn = 0, maxv = 0;
  if (k != Keys.end()) {
    count = k->second.size();
    for (auto &v : k->second) {
      maxn = std::max(maxn, v.first.size());
      maxv = std::max(maxv, v.second.data.size());
    }
  }
  if (values)
    *values = (DWORD)count;
  if (maxvaluenamelen)
    *maxvaluenamelen = (DWORD)maxn;
  if (maxvaluelen)
    *maxvaluelen = (DWORD)maxv;
  return ERROR_SUCCESS;
}

extern "C" LONG RegFlushKey(HKEY) { return ERROR_SUCCESS; }
