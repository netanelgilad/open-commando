/*
** Win32 core services on POSIX/Emscripten: errors, debug output, time,
** memory, process info, string helpers and the MSVC CRT extras.
*/
#include "windows.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <string>
#include <sys/time.h>
#include <unistd.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#endif

/* ------------------------------------------------------------------ errors */

static thread_local DWORD LastError = 0;
extern "C" DWORD GetLastError(void) { return LastError; }
extern "C" void SetLastError(DWORD err) { LastError = err; }

DWORD Rebel_Errno_To_Win32(int e) {
  switch (e) {
  case 0: return ERROR_SUCCESS;
  case ENOENT: return ERROR_FILE_NOT_FOUND;
  case ENOTDIR: return ERROR_PATH_NOT_FOUND;
  case EACCES:
  case EPERM: return ERROR_ACCESS_DENIED;
  case EEXIST: return ERROR_ALREADY_EXISTS;
  case EBADF: return ERROR_INVALID_HANDLE;
  default: return 31; /* ERROR_GEN_FAILURE */
  }
}

/* ------------------------------------------------------------------- debug */

#ifdef __EMSCRIPTEN__
// Maps MB_* button sets onto window.confirm/alert; returns an ID* code.
EM_JS(int, Rebel_JS_Message_Box, (const char *text, const char *caption, int type), {
  var t = UTF8ToString(text);
  var c = UTF8ToString(caption);
  var buttons = type & 0xF;
  if (typeof window === 'undefined') return 1;
  var msg = c + "\n\n" + t;
  if (buttons == 1) return window.confirm(msg) ? 1 : 2;  // OK/Cancel
  if (buttons == 5) return window.confirm(msg) ? 4 : 2;  // Retry/Cancel
  if (buttons == 3 || buttons == 4) return window.confirm(msg) ? 6 : 7; // Yes/No
  if (buttons == 2) { console.error(msg); return 5; }   // Abort/Retry/Ignore: ignore
  window.alert(msg);
  return 1;
});
#endif

extern "C" void OutputDebugStringA(LPCSTR s) {
  if (s)
    fputs(s, stderr);
}

extern "C" void OutputDebugStringW(LPCWSTR s) {
  if (s)
    fprintf(stderr, "%ls", s);
}

extern "C" void DebugBreak(void) {
#ifdef __EMSCRIPTEN__
  EM_ASM({ debugger; });
#endif
}

extern "C" BOOL IsDebuggerPresent(void) { return FALSE; }

extern "C" int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type) {
  fprintf(stderr, "[MessageBox] %s: %s\n", caption ? caption : "", text ? text : "");
#ifdef __EMSCRIPTEN__
  return Rebel_JS_Message_Box(text ? text : "", caption ? caption : "", (int)type);
#else
  (void)type;
  return IDOK;
#endif
}

extern "C" int MessageBoxW(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type) {
  char t[2048], c[256];
  snprintf(t, sizeof(t), "%ls", text ? text : L"");
  snprintf(c, sizeof(c), "%ls", caption ? caption : L"");
  return MessageBoxA(hwnd, t, c, type);
}

extern "C" DWORD FormatMessageA(DWORD flags, LPCVOID, DWORD msgid, DWORD, LPSTR buf, DWORD size, va_list *) {
  char tmp[64];
  snprintf(tmp, sizeof(tmp), "Error %lu", (unsigned long)msgid);
  if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
    *(LPSTR *)buf = strdup(tmp);
    return strlen(tmp);
  }
  if (!buf || !size)
    return 0;
  snprintf(buf, size, "%s", tmp);
  return strlen(buf);
}

/* -------------------------------------------------------------------- time */

static unsigned long long Now_Micros() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
static const unsigned long long BootMicros = Now_Micros();

extern "C" DWORD GetTickCount(void) { return (DWORD)((Now_Micros() - BootMicros) / 1000ULL); }
extern "C" DWORD timeGetTime(void) { return GetTickCount(); }
extern "C" UINT timeBeginPeriod(UINT) { return 0; }
extern "C" UINT timeEndPeriod(UINT) { return 0; }

extern "C" BOOL QueryPerformanceCounter(LARGE_INTEGER *count) {
  count->QuadPart = (LONGLONG)(Now_Micros() - BootMicros);
  return TRUE;
}

extern "C" BOOL QueryPerformanceFrequency(LARGE_INTEGER *freq) {
  freq->QuadPart = 1000000;
  return TRUE;
}

// The game build links with ASYNCIFY: sleeping on the browser main thread
// unwinds to the event loop and resumes later instead of blocking.
extern "C" void Rebel_Yield(DWORD ms) {
#ifdef __EMSCRIPTEN__
  if (emscripten_has_asyncify())
    emscripten_sleep(ms);
#else
  (void)ms;
#endif
}

extern "C" void Sleep(DWORD ms) {
#ifdef __EMSCRIPTEN__
  if (emscripten_is_main_browser_thread()) {
    Rebel_Yield(ms);
    return;
  }
#endif
  usleep((useconds_t)ms * 1000);
}

static const unsigned long long EPOCH_DIFF_100NS = 116444736000000000ULL; // 1601 -> 1970

static void Tm_To_SystemTime(const struct tm &t, int ms, LPSYSTEMTIME st) {
  st->wYear = (WORD)(t.tm_year + 1900);
  st->wMonth = (WORD)(t.tm_mon + 1);
  st->wDayOfWeek = (WORD)t.tm_wday;
  st->wDay = (WORD)t.tm_mday;
  st->wHour = (WORD)t.tm_hour;
  st->wMinute = (WORD)t.tm_min;
  st->wSecond = (WORD)t.tm_sec;
  st->wMilliseconds = (WORD)ms;
}

extern "C" void GetSystemTime(LPSYSTEMTIME st) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  time_t s = tv.tv_sec;
  struct tm t;
  gmtime_r(&s, &t);
  Tm_To_SystemTime(t, tv.tv_usec / 1000, st);
}

extern "C" void GetLocalTime(LPSYSTEMTIME st) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  time_t s = tv.tv_sec;
  struct tm t;
  localtime_r(&s, &t);
  Tm_To_SystemTime(t, tv.tv_usec / 1000, st);
}

void Rebel_Unix_To_FileTime(long long unix_seconds, long nanos, LPFILETIME ft) {
  unsigned long long v = (unsigned long long)unix_seconds * 10000000ULL + nanos / 100 + EPOCH_DIFF_100NS;
  ft->dwLowDateTime = (DWORD)(v & 0xFFFFFFFFULL);
  ft->dwHighDateTime = (DWORD)(v >> 32);
}

static long long FileTime_To_Unix(const FILETIME *ft, int *ms) {
  unsigned long long v = ((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
  v -= EPOCH_DIFF_100NS;
  if (ms)
    *ms = (int)((v / 10000ULL) % 1000ULL);
  return (long long)(v / 10000000ULL);
}

extern "C" void GetSystemTimeAsFileTime(LPFILETIME ft) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  Rebel_Unix_To_FileTime(tv.tv_sec, tv.tv_usec * 1000L, ft);
}

extern "C" BOOL SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft) {
  struct tm t = {};
  t.tm_year = st->wYear - 1900;
  t.tm_mon = st->wMonth - 1;
  t.tm_mday = st->wDay;
  t.tm_hour = st->wHour;
  t.tm_min = st->wMinute;
  t.tm_sec = st->wSecond;
  Rebel_Unix_To_FileTime((long long)timegm(&t), st->wMilliseconds * 1000000L, ft);
  return TRUE;
}

extern "C" BOOL FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st) {
  int ms = 0;
  time_t s = (time_t)FileTime_To_Unix(ft, &ms);
  struct tm t;
  gmtime_r(&s, &t);
  Tm_To_SystemTime(t, ms, st);
  return TRUE;
}

extern "C" BOOL FileTimeToLocalFileTime(const FILETIME *ft, LPFILETIME out) {
  *out = *ft; // the browser reports UTC; local offset applied at display time
  return TRUE;
}

extern "C" LONG CompareFileTime(const FILETIME *a, const FILETIME *b) {
  unsigned long long x = ((unsigned long long)a->dwHighDateTime << 32) | a->dwLowDateTime;
  unsigned long long y = ((unsigned long long)b->dwHighDateTime << 32) | b->dwLowDateTime;
  return x < y ? -1 : (x > y ? 1 : 0);
}

extern "C" BOOL FileTimeToDosDateTime(const FILETIME *ft, LPWORD date, LPWORD tme) {
  SYSTEMTIME st;
  FileTimeToSystemTime(ft, &st);
  if (st.wYear < 1980)
    return FALSE;
  *date = (WORD)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
  *tme = (WORD)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
  return TRUE;
}

extern "C" BOOL DosDateTimeToFileTime(WORD date, WORD tme, LPFILETIME ft) {
  SYSTEMTIME st = {};
  st.wYear = (WORD)(((date >> 9) & 0x7F) + 1980);
  st.wMonth = (WORD)((date >> 5) & 0x0F);
  st.wDay = (WORD)(date & 0x1F);
  st.wHour = (WORD)((tme >> 11) & 0x1F);
  st.wMinute = (WORD)((tme >> 5) & 0x3F);
  st.wSecond = (WORD)((tme & 0x1F) * 2);
  return SystemTimeToFileTime(&st, ft);
}

extern "C" DWORD GetTimeZoneInformation(LPTIME_ZONE_INFORMATION tz) {
  memset(tz, 0, sizeof(*tz));
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  tz->Bias = (LONG)(-lt.tm_gmtoff / 60);
  return TIME_ZONE_ID_UNKNOWN;
}

/* ------------------------------------------------------------------ memory */

extern "C" LPVOID VirtualAlloc(LPVOID, SIZE_T size, DWORD type, DWORD) {
  if (!(type & (MEM_COMMIT | MEM_RESERVE)))
    return nullptr;
  return calloc(1, size);
}
extern "C" BOOL VirtualFree(LPVOID addr, SIZE_T, DWORD type) {
  if (type & MEM_RELEASE)
    free(addr);
  return TRUE;
}
extern "C" BOOL VirtualProtect(LPVOID, SIZE_T, DWORD prot, PDWORD old) {
  if (old)
    *old = prot;
  return TRUE;
}

// Global/Local handles are plain pointers with a size header.
struct GlobalBlock {
  SIZE_T size;
  SIZE_T pad;
};
extern "C" HGLOBAL GlobalAlloc(UINT flags, SIZE_T size) {
  GlobalBlock *b = (GlobalBlock *)((flags & GMEM_ZEROINIT) ? calloc(1, sizeof(GlobalBlock) + size)
                                                           : malloc(sizeof(GlobalBlock) + size));
  if (!b)
    return nullptr;
  b->size = size;
  return (HGLOBAL)(b + 1);
}
extern "C" HGLOBAL GlobalFree(HGLOBAL h) {
  if (h)
    free(((GlobalBlock *)h) - 1);
  return nullptr;
}
extern "C" LPVOID GlobalLock(HGLOBAL h) { return h; }
extern "C" BOOL GlobalUnlock(HGLOBAL) { return TRUE; }
extern "C" SIZE_T GlobalSize(HGLOBAL h) { return h ? (((GlobalBlock *)h) - 1)->size : 0; }
extern "C" HLOCAL LocalAlloc(UINT flags, SIZE_T size) { return GlobalAlloc(flags, size); }
extern "C" HLOCAL LocalFree(HLOCAL h) { return GlobalFree(h); }

extern "C" HANDLE GetProcessHeap(void) { return (HANDLE)1; }
extern "C" LPVOID HeapAlloc(HANDLE, DWORD flags, SIZE_T size) {
  return (flags & HEAP_ZERO_MEMORY) ? calloc(1, size) : malloc(size);
}
extern "C" BOOL HeapFree(HANDLE, DWORD, LPVOID p) {
  free(p);
  return TRUE;
}

extern "C" BOOL IsBadReadPtr(const void *p, UINT_PTR) { return p == nullptr; }
extern "C" BOOL IsBadWritePtr(LPVOID p, UINT_PTR) { return p == nullptr; }
extern "C" BOOL IsBadCodePtr(FARPROC p) { return p == nullptr; }

extern "C" void GlobalMemoryStatus(LPMEMORYSTATUS ms) {
  // wasm32 address space: report 2GB total, 1GB free (values only feed logs
  // and texture-quality heuristics).
  memset(ms, 0, sizeof(*ms));
  ms->dwLength = sizeof(*ms);
  ms->dwTotalPhys = 0x80000000UL;
  ms->dwAvailPhys = 0x40000000UL;
  ms->dwTotalPageFile = 0x80000000UL;
  ms->dwAvailPageFile = 0x40000000UL;
  ms->dwTotalVirtual = 0x80000000UL;
  ms->dwAvailVirtual = 0x40000000UL;
}

extern "C" void GetSystemInfo(LPSYSTEM_INFO info) {
  memset(info, 0, sizeof(*info));
  info->dwPageSize = 65536;
  info->dwAllocationGranularity = 65536;
  info->dwNumberOfProcessors = 1;
  info->dwProcessorType = 586;
  info->wProcessorLevel = 6;
}

/* ---------------------------------------------------------- process / env */

static HANDLE const PseudoProcess = (HANDLE)(LONG_PTR)-1;
static HANDLE const PseudoThread = (HANDLE)(LONG_PTR)-2;

extern "C" DWORD GetModuleFileNameA(HMODULE, LPSTR buf, DWORD size) {
  // The game derives its install directory from this; the web runtime
  // mounts the game data at /game.
  const char *dir = getenv("REBEL_GAME_DIR");
  if (!buf || !size)
    return 0;
  snprintf(buf, size, "%s/Game.exe", dir ? dir : "/game");
  return (DWORD)strlen(buf);
}
extern "C" HMODULE GetModuleHandleA(LPCSTR) { return (HMODULE)0x400000; }
// "DLLs" the web build provides in-process. The engine loads D3D9.DLL and
// DINPUT8.DLL dynamically; referencing the entry points here also keeps
// their objects linked.
struct IDirect3D9;
extern "C" IDirect3D9 *Direct3DCreate9(UINT sdk_version);
extern "C" HRESULT DirectInput8Create(HINSTANCE, DWORD, const GUID &, LPVOID *, void *);
// Scripts library entry points (Scripts/DLLmain.cpp). Weak: tools and tests
// that don't link the scripts still build; the game links them in full.
class ScriptClass;
class ScriptCommandsClass;
__attribute__((weak)) ScriptClass *Create_Script(const char *name);
__attribute__((weak)) void Destroy_Script(ScriptClass *script);
__attribute__((weak)) bool Set_Script_Commands(ScriptCommandsClass *commands);
__attribute__((weak)) void Set_Request_Destroy_Func(void (*function)(ScriptClass *));
namespace {
struct BuiltinExport {
  const char *module;
  const char *name;
  FARPROC fn;
};
const BuiltinExport BuiltinExports[] = {
    {"d3d9.dll", "Direct3DCreate9", (FARPROC)&Direct3DCreate9},
    {"dinput8.dll", "DirectInput8Create", (FARPROC)&DirectInput8Create},
#define REBEL_SCRIPTS_MODULE(dll)                                                                                      \
  {dll, "Create_Script", (FARPROC)&Create_Script}, {dll, "Destroy_Script", (FARPROC)&Destroy_Script},                  \
      {dll, "Set_Script_Commands", (FARPROC)&Set_Script_Commands},                                                     \
      {dll, "Set_Request_Destroy_Func", (FARPROC)&Set_Request_Destroy_Func}
    REBEL_SCRIPTS_MODULE("scripts.dll"),
    REBEL_SCRIPTS_MODULE("scriptsd.dll"),
    REBEL_SCRIPTS_MODULE("scriptsp.dll"),
#undef REBEL_SCRIPTS_MODULE
};
const char *Module_Base_Name(const char *name) {
  const char *base = name;
  for (const char *p = name; *p; ++p)
    if (*p == '\\' || *p == '/')
      base = p + 1;
  return base;
}
} // namespace

extern "C" HMODULE LoadLibraryA(LPCSTR name) {
  if (name) {
    const char *base = Module_Base_Name(name);
    for (const BuiltinExport &e : BuiltinExports)
      if (strcasecmp(e.module, base) == 0 && e.fn)
        return (HMODULE)(const void *)e.module;
  }
  fprintf(stderr, "[win32] LoadLibrary(%s) -> not available\n", name ? name : "");
  SetLastError(ERROR_FILE_NOT_FOUND);
  return nullptr;
}
extern "C" BOOL FreeLibrary(HMODULE) { return TRUE; }
extern "C" FARPROC GetProcAddress(HMODULE mod, LPCSTR name) {
  for (const BuiltinExport &e : BuiltinExports)
    if (mod && strcasecmp(e.module, (const char *)mod) == 0 && name && strcmp(e.name, name) == 0)
      return e.fn;
  return nullptr;
}
extern "C" HANDLE GetCurrentProcess(void) { return PseudoProcess; }
extern "C" DWORD GetCurrentProcessId(void) { return 1; }
extern "C" HANDLE GetCurrentThread(void) { return PseudoThread; }
extern "C" BOOL SetPriorityClass(HANDLE, DWORD) { return TRUE; }
extern "C" DWORD GetPriorityClass(HANDLE) { return NORMAL_PRIORITY_CLASS; }
extern "C" void ExitProcess(UINT code) { exit((int)code); }
extern "C" BOOL TerminateProcess(HANDLE, UINT code) {
  exit((int)code);
  return TRUE;
}

static char CommandLine[1024] = "Game.exe";
extern "C" void Rebel_Set_Command_Line(const char *cmd) { snprintf(CommandLine, sizeof(CommandLine), "%s", cmd); }
extern "C" LPSTR GetCommandLineA(void) { return CommandLine; }

extern "C" DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size) {
  const char *v = getenv(name);
  if (!v) {
    SetLastError(203); /* ERROR_ENVVAR_NOT_FOUND */
    return 0;
  }
  DWORD len = (DWORD)strlen(v);
  if (!buf || size <= len)
    return len + 1;
  memcpy(buf, v, len + 1);
  return len;
}

extern "C" BOOL GetComputerNameA(LPSTR buf, LPDWORD size) {
  snprintf(buf, *size, "BROWSER");
  *size = (DWORD)strlen(buf);
  return TRUE;
}
extern "C" BOOL GetUserNameA(LPSTR buf, LPDWORD size) {
  snprintf(buf, *size, "Player");
  *size = (DWORD)strlen(buf) + 1;
  return TRUE;
}

extern "C" BOOL GetVersionExA(LPOSVERSIONINFO info) {
  // Report Windows XP SP2: the newest OS the 2002 code knows how to branch on.
  info->dwMajorVersion = 5;
  info->dwMinorVersion = 1;
  info->dwBuildNumber = 2600;
  info->dwPlatformId = VER_PLATFORM_WIN32_NT;
  snprintf(info->szCSDVersion, sizeof(info->szCSDVersion), "Rebel Web");
  return TRUE;
}
extern "C" DWORD GetVersion(void) { return 0x0A280105; }

extern "C" UINT GetWindowsDirectoryA(LPSTR buf, UINT size) {
  snprintf(buf, size, "/windows");
  return (UINT)strlen(buf);
}
extern "C" UINT GetSystemDirectoryA(LPSTR buf, UINT size) {
  snprintf(buf, size, "/windows/system");
  return (UINT)strlen(buf);
}

extern "C" LCID GetUserDefaultLCID(void) { return 0x0409; }
extern "C" LANGID GetUserDefaultLangID(void) { return 0x0409; }
extern "C" LANGID GetSystemDefaultLangID(void) { return 0x0409; }
extern "C" UINT GetACP(void) { return 1252; }

extern "C" int LoadStringA(HINSTANCE, UINT id, LPSTR buf, int max) {
  if (buf && max > 0)
    buf[0] = 0;
  (void)id;
  return 0;
}
extern "C" int LoadStringW(HINSTANCE, UINT id, LPWSTR buf, int max) {
  if (buf && max > 0)
    buf[0] = 0;
  (void)id;
  return 0;
}

/* --------------------------------------------------------------- resources
**
** The game's dialogs live in Commando/chat.rc. The build compiles it with
** llvm-rc into a Win32 .res file embedded at REBEL_RESOURCE_FILE; this parses
** that file once and serves FindResource/LoadResource/LockResource from it.
** .res layout: a sequence of entries, each DataSize, HeaderSize, then TYPE
** and NAME (0xFFFF+ordinal or a UTF-16 string), then data, DWORD-aligned.
*/
#include <vector>
namespace {
struct ResEntry {
  bool type_is_id, name_is_id;
  WORD type_id, name_id;
  std::wstring type_name, name_name;
  const BYTE *data;
  DWORD size;
};
std::vector<BYTE> ResFile;
std::vector<ResEntry> ResEntries;
bool ResLoaded = false;

const BYTE *Read_Res_Name(const BYTE *p, const BYTE *end, bool &is_id, WORD &id, std::wstring &name) {
  if (p + 2 > end)
    return end;
  WORD first = (WORD)(p[0] | (p[1] << 8));
  if (first == 0xFFFF) {
    is_id = true;
    id = (WORD)(p[2] | (p[3] << 8));
    return p + 4;
  }
  is_id = false;
  while (p + 2 <= end) {
    WORD c = (WORD)(p[0] | (p[1] << 8));
    p += 2;
    if (!c)
      break;
    name.push_back((wchar_t)c);
  }
  return p;
}

void Load_Resources() {
  if (ResLoaded)
    return;
  ResLoaded = true;
  const char *path = getenv("REBEL_RESOURCE_FILE");
  FILE *f = fopen(path ? path : "/rebel/chat.res", "rb");
  if (!f) {
    fprintf(stderr, "[win32] resource file not found; dialogs unavailable\n");
    return;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  ResFile.resize(n > 0 ? (size_t)n : 0);
  size_t got = fread(ResFile.data(), 1, ResFile.size(), f);
  fclose(f);
  ResFile.resize(got);
  const BYTE *base = ResFile.data(), *end = base + ResFile.size();
  const BYTE *p = base;
  while (p + 8 <= end) {
    DWORD data_size = p[0] | (p[1] << 8) | (p[2] << 16) | ((DWORD)p[3] << 24);
    DWORD header_size = p[4] | (p[5] << 8) | (p[6] << 16) | ((DWORD)p[7] << 24);
    if (header_size < 8 || p + header_size + data_size > end)
      break;
    ResEntry e{};
    const BYTE *q = Read_Res_Name(p + 8, p + header_size, e.type_is_id, e.type_id, e.type_name);
    q = Read_Res_Name(q, p + header_size, e.name_is_id, e.name_id, e.name_name);
    e.data = p + header_size;
    e.size = data_size;
    if (data_size)
      ResEntries.push_back(e);
    size_t next = (size_t)(p - base) + header_size + data_size;
    p = base + ((next + 3) & ~(size_t)3);
  }
}

bool Res_Name_Matches(LPCSTR want, bool is_id, WORD id, const std::wstring &name) {
  if ((ULONG_PTR)want < 0x10000)
    return is_id && id == (WORD)(ULONG_PTR)want;
  if (is_id)
    return want[0] == '#' && atoi(want + 1) == id;
  if (strlen(want) != name.size())
    return false;
  for (size_t i = 0; i < name.size(); i++)
    if (towupper((wint_t)name[i]) != towupper((wint_t)(unsigned char)want[i]))
      return false;
  return true;
}
} // namespace

extern "C" HRSRC FindResourceA(HMODULE, LPCSTR name, LPCSTR type) {
  Load_Resources();
  for (const ResEntry &e : ResEntries)
    if (Res_Name_Matches(type, e.type_is_id, e.type_id, e.type_name) &&
        Res_Name_Matches(name, e.name_is_id, e.name_id, e.name_name))
      return (HRSRC)&e;
  SetLastError(1813); /* ERROR_RESOURCE_TYPE_NOT_FOUND */
  return nullptr;
}
extern "C" HGLOBAL LoadResource(HMODULE, HRSRC res) { return res ? (HGLOBAL)((const ResEntry *)res)->data : nullptr; }
extern "C" LPVOID LockResource(HGLOBAL res) { return res; }
extern "C" DWORD SizeofResource(HMODULE, HRSRC res) { return res ? ((const ResEntry *)res)->size : 0; }
extern "C" DWORD GetFileVersionInfoSizeA(LPCSTR, LPDWORD handle) {
  if (handle)
    *handle = 0;
  return 0;
}
extern "C" BOOL GetFileVersionInfoA(LPCSTR, DWORD, DWORD, LPVOID) { return FALSE; }
extern "C" BOOL VerQueryValueA(LPCVOID, LPCSTR, LPVOID *, PUINT) { return FALSE; }

/* ----------------------------------------------------------------- strings */

// Code pages: CP_ACP is treated as Windows-1252 for the high half, which is
// what Renegade's Western-language data assumes. CP_UTF8 is real UTF-8.
static const wchar_t Cp1252High[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                       0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
                                       0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                       0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

static wchar_t Cp1252_To_Wide(unsigned char c) {
  if (c >= 0x80 && c < 0xA0)
    return Cp1252High[c - 0x80];
  return (wchar_t)c;
}

static char Wide_To_Cp1252(wchar_t w, bool *lossy) {
  if (w < 0x80 || (w >= 0xA0 && w < 0x100))
    return (char)w;
  for (int i = 0; i < 32; i++)
    if (Cp1252High[i] == w)
      return (char)(0x80 + i);
  if (lossy)
    *lossy = true;
  return '?';
}

extern "C" int MultiByteToWideChar(UINT cp, DWORD, LPCSTR src, int srclen, LPWSTR dst, int dstlen) {
  if (!src)
    return 0;
  if (srclen < 0)
    srclen = (int)strlen(src) + 1;
  std::wstring out;
  if (cp == CP_UTF8) {
    const unsigned char *s = (const unsigned char *)src;
    int i = 0;
    while (i < srclen) {
      unsigned c = s[i];
      unsigned cp32;
      int n;
      if (c < 0x80) { cp32 = c; n = 1; }
      else if ((c & 0xE0) == 0xC0) { cp32 = c & 0x1F; n = 2; }
      else if ((c & 0xF0) == 0xE0) { cp32 = c & 0x0F; n = 3; }
      else if ((c & 0xF8) == 0xF0) { cp32 = c & 0x07; n = 4; }
      else { cp32 = 0xFFFD; n = 1; }
      for (int k = 1; k < n; k++) {
        if (i + k >= srclen || (s[i + k] & 0xC0) != 0x80) { cp32 = 0xFFFD; n = k; break; }
        cp32 = (cp32 << 6) | (s[i + k] & 0x3F);
      }
      out.push_back((wchar_t)cp32);
      i += n;
    }
  } else {
    for (int i = 0; i < srclen; i++)
      out.push_back(Cp1252_To_Wide((unsigned char)src[i]));
  }
  if (dstlen == 0)
    return (int)out.size();
  if ((int)out.size() > dstlen) {
    SetLastError(122); /* ERROR_INSUFFICIENT_BUFFER */
    return 0;
  }
  memcpy(dst, out.data(), out.size() * sizeof(wchar_t));
  return (int)out.size();
}

extern "C" int WideCharToMultiByte(UINT cp, DWORD, LPCWSTR src, int srclen, LPSTR dst, int dstlen, LPCSTR,
                                   LPBOOL useddef) {
  if (!src)
    return 0;
  if (srclen < 0)
    srclen = (int)wcslen(src) + 1;
  std::string out;
  bool lossy = false;
  for (int i = 0; i < srclen; i++) {
    unsigned w = (unsigned)src[i];
    if (cp == CP_UTF8) {
      if (w < 0x80) out.push_back((char)w);
      else if (w < 0x800) { out.push_back((char)(0xC0 | (w >> 6))); out.push_back((char)(0x80 | (w & 0x3F))); }
      else if (w < 0x10000) {
        out.push_back((char)(0xE0 | (w >> 12)));
        out.push_back((char)(0x80 | ((w >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (w & 0x3F)));
      } else {
        out.push_back((char)(0xF0 | (w >> 18)));
        out.push_back((char)(0x80 | ((w >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((w >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (w & 0x3F)));
      }
    } else {
      out.push_back(Wide_To_Cp1252((wchar_t)w, &lossy));
    }
  }
  if (useddef)
    *useddef = lossy;
  if (dstlen == 0)
    return (int)out.size();
  if ((int)out.size() > dstlen) {
    SetLastError(122);
    return 0;
  }
  memcpy(dst, out.data(), out.size());
  return (int)out.size();
}

extern "C" int lstrlenA(LPCSTR s) { return s ? (int)strlen(s) : 0; }
extern "C" int lstrlenW(LPCWSTR s) { return s ? (int)wcslen(s) : 0; }
extern "C" LPSTR lstrcpyA(LPSTR d, LPCSTR s) { return strcpy(d, s); }
extern "C" LPWSTR lstrcpyW(LPWSTR d, LPCWSTR s) { return wcscpy(d, s); }
extern "C" LPSTR lstrcpynA(LPSTR d, LPCSTR s, int n) {
  if (n <= 0)
    return d;
  strncpy(d, s, n - 1);
  d[n - 1] = 0;
  return d;
}
extern "C" LPWSTR lstrcpynW(LPWSTR d, LPCWSTR s, int n) {
  if (n <= 0)
    return d;
  wcsncpy(d, s, n - 1);
  d[n - 1] = 0;
  return d;
}
extern "C" LPSTR lstrcatA(LPSTR d, LPCSTR s) { return strcat(d, s); }
extern "C" LPWSTR lstrcatW(LPWSTR d, LPCWSTR s) { return wcscat(d, s); }
extern "C" int lstrcmpA(LPCSTR a, LPCSTR b) { return strcmp(a, b); }
extern "C" int lstrcmpW(LPCWSTR a, LPCWSTR b) { return wcscmp(a, b); }
extern "C" int lstrcmpiA(LPCSTR a, LPCSTR b) { return strcasecmp(a, b); }
extern "C" int lstrcmpiW(LPCWSTR a, LPCWSTR b) { return wcscasecmp(a, b); }
extern "C" LPSTR CharUpperA(LPSTR s) {
  if ((ULONG_PTR)s < 0x10000) // single character form
    return (LPSTR)(ULONG_PTR)toupper((int)(ULONG_PTR)s);
  return strupr(s);
}
extern "C" LPSTR CharLowerA(LPSTR s) {
  if ((ULONG_PTR)s < 0x10000)
    return (LPSTR)(ULONG_PTR)tolower((int)(ULONG_PTR)s);
  return strlwr(s);
}
extern "C" LPSTR CharNextA(LPCSTR s) { return (LPSTR)(*s ? s + 1 : s); }
extern "C" BOOL IsCharAlphaA(CHAR c) { return isalpha((unsigned char)c) != 0; }
extern "C" BOOL IsCharAlphaNumericA(CHAR c) { return isalnum((unsigned char)c) != 0; }
extern "C" int wsprintfA(LPSTR buf, LPCSTR fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsprintf(buf, fmt, ap);
  va_end(ap);
  return n;
}
extern "C" int wvsprintfA(LPSTR buf, LPCSTR fmt, va_list args) { return vsprintf(buf, fmt, args); }

static int Format_Date(const SYSTEMTIME *t, DWORD flags, char *out, int n) {
  SYSTEMTIME now;
  if (!t) {
    GetLocalTime(&now);
    t = &now;
  }
  static const char *months[] = {"January", "February", "March", "April", "May", "June",
                                 "July", "August", "September", "October", "November", "December"};
  if (flags & DATE_LONGDATE)
    return snprintf(out, n, "%s %d, %d", months[(t->wMonth + 11) % 12], t->wDay, t->wYear);
  return snprintf(out, n, "%d/%d/%04d", t->wMonth, t->wDay, t->wYear);
}
static int Format_Time(const SYSTEMTIME *t, DWORD flags, char *out, int n) {
  SYSTEMTIME now;
  if (!t) {
    GetLocalTime(&now);
    t = &now;
  }
  int h = t->wHour % 12;
  if (h == 0)
    h = 12;
  const char *ampm = t->wHour < 12 ? "AM" : "PM";
  if (flags & TIME_NOSECONDS)
    return snprintf(out, n, "%d:%02d %s", h, t->wMinute, ampm);
  return snprintf(out, n, "%d:%02d:%02d %s", h, t->wMinute, t->wSecond, ampm);
}
extern "C" int GetDateFormatA(LCID, DWORD f, const SYSTEMTIME *t, LPCSTR, LPSTR buf, int n) {
  char tmp[128];
  int len = Format_Date(t, f, tmp, sizeof(tmp)) + 1;
  if (n == 0)
    return len;
  snprintf(buf, n, "%s", tmp);
  return len;
}
extern "C" int GetTimeFormatA(LCID, DWORD f, const SYSTEMTIME *t, LPCSTR, LPSTR buf, int n) {
  char tmp[128];
  int len = Format_Time(t, f, tmp, sizeof(tmp)) + 1;
  if (n == 0)
    return len;
  snprintf(buf, n, "%s", tmp);
  return len;
}
extern "C" int GetDateFormatW(LCID l, DWORD f, const SYSTEMTIME *t, LPCWSTR, LPWSTR buf, int n) {
  char tmp[128];
  int len = GetDateFormatA(l, f, t, nullptr, tmp, sizeof(tmp));
  if (n == 0)
    return len;
  swprintf(buf, n, L"%s", tmp);
  return len;
}
extern "C" int GetTimeFormatW(LCID l, DWORD f, const SYSTEMTIME *t, LPCWSTR, LPWSTR buf, int n) {
  char tmp[128];
  int len = GetTimeFormatA(l, f, t, nullptr, tmp, sizeof(tmp));
  if (n == 0)
    return len;
  swprintf(buf, n, L"%s", tmp);
  return len;
}

/* --------------------------------------------------------- MSVC CRT extras */

extern "C" char *strupr(char *s) {
  for (char *p = s; p && *p; ++p)
    *p = (char)toupper((unsigned char)*p);
  return s;
}
extern "C" char *strlwr(char *s) {
  for (char *p = s; p && *p; ++p)
    *p = (char)tolower((unsigned char)*p);
  return s;
}
extern "C" wchar_t *wcsupr(wchar_t *s) {
  for (wchar_t *p = s; p && *p; ++p)
    *p = (wchar_t)towupper(*p);
  return s;
}
extern "C" wchar_t *wcslwr(wchar_t *s) {
  for (wchar_t *p = s; p && *p; ++p)
    *p = (wchar_t)towlower(*p);
  return s;
}
extern "C" char *strrev(char *s) {
  if (!s)
    return s;
  size_t n = strlen(s);
  for (size_t i = 0; i < n / 2; i++) {
    char t = s[i];
    s[i] = s[n - 1 - i];
    s[n - 1 - i] = t;
  }
  return s;
}

static char *Unsigned_To_String(unsigned long long v, char *buf, int radix, bool neg) {
  char tmp[72];
  int i = 0;
  if (radix < 2 || radix > 36)
    radix = 10;
  do {
    int d = (int)(v % radix);
    tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    v /= radix;
  } while (v);
  char *p = buf;
  if (neg)
    *p++ = '-';
  while (i)
    *p++ = tmp[--i];
  *p = 0;
  return buf;
}
extern "C" char *itoa(int value, char *buf, int radix) {
  bool neg = radix == 10 && value < 0;
  return Unsigned_To_String(neg ? -(long long)value : (unsigned int)value, buf, radix, neg);
}
extern "C" char *ltoa(long value, char *buf, int radix) {
  bool neg = radix == 10 && value < 0;
  return Unsigned_To_String(neg ? -(long long)value : (unsigned long)value, buf, radix, neg);
}
extern "C" char *ultoa(unsigned long value, char *buf, int radix) { return Unsigned_To_String(value, buf, radix, false); }
extern "C" char *_i64toa(long long value, char *buf, int radix) {
  bool neg = radix == 10 && value < 0;
  return Unsigned_To_String(neg ? -(unsigned long long)value : (unsigned long long)value, buf, radix, neg);
}

extern "C" void _splitpath(const char *path, char *drive, char *dir, char *fname, char *ext) {
  if (drive)
    drive[0] = 0;
  if (dir)
    dir[0] = 0;
  if (fname)
    fname[0] = 0;
  if (ext)
    ext[0] = 0;
  if (!path)
    return;
  const char *p = path;
  if (p[0] && p[1] == ':') {
    if (drive) {
      drive[0] = p[0];
      drive[1] = ':';
      drive[2] = 0;
    }
    p += 2;
  }
  const char *last_slash = nullptr;
  for (const char *q = p; *q; ++q)
    if (*q == '\\' || *q == '/')
      last_slash = q;
  const char *name = last_slash ? last_slash + 1 : p;
  if (dir && last_slash) {
    size_t n = (size_t)(last_slash - p + 1);
    memcpy(dir, p, n);
    dir[n] = 0;
  }
  const char *dot = strrchr(name, '.');
  if (!dot)
    dot = name + strlen(name);
  if (fname) {
    size_t n = (size_t)(dot - name);
    memcpy(fname, name, n);
    fname[n] = 0;
  }
  if (ext)
    strcpy(ext, dot);
}

extern "C" void _makepath(char *path, const char *drive, const char *dir, const char *fname, const char *ext) {
  path[0] = 0;
  if (drive && *drive) {
    strcat(path, drive);
    if (drive[strlen(drive) - 1] != ':')
      strcat(path, ":");
  }
  if (dir && *dir) {
    strcat(path, dir);
    char last = dir[strlen(dir) - 1];
    if (last != '\\' && last != '/')
      strcat(path, "\\");
  }
  if (fname)
    strcat(path, fname);
  if (ext && *ext) {
    if (*ext != '.')
      strcat(path, ".");
    strcat(path, ext);
  }
}

extern "C" char *_fullpath(char *abs, const char *rel, size_t max) {
  char cwd[MAX_PATH];
  if (!abs) {
    max = MAX_PATH;
    abs = (char *)malloc(max);
  }
  if (rel && (rel[0] == '/' || rel[0] == '\\' || (rel[0] && rel[1] == ':')))
    snprintf(abs, max, "%s", rel);
  else
    snprintf(abs, max, "%s/%s", getcwd(cwd, sizeof(cwd)) ? cwd : "", rel ? rel : "");
  return abs;
}

// _rotl/_rotr are compiler builtins under -fms-extensions.

extern "C" int _wtoi(const wchar_t *s) { return (int)wcstol(s, nullptr, 10); }
extern "C" long _wtol(const wchar_t *s) { return wcstol(s, nullptr, 10); }
extern "C" unsigned char *_mbsinc(const unsigned char *s) { return (unsigned char *)(s + 1); }
extern "C" size_t _mbslen(const unsigned char *s) { return strlen((const char *)s); }
extern "C" size_t _mbsnccnt(const unsigned char *s, size_t n) {
  size_t len = strlen((const char *)s);
  return len < n ? len : n;
}
extern "C" BOOL IsDBCSLeadByte(BYTE) { return FALSE; }
extern "C" int GetKeyboardLayoutList(int n, HKL *list) {
  if (n > 0 && list)
    list[0] = GetKeyboardLayout(0);
  return 1;
}

/* ------------------------------------------------ processes and console */

// No child processes in a browser tab.
extern "C" BOOL CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                               LPVOID, LPCSTR, LPSTARTUPINFO, LPPROCESS_INFORMATION pi) {
  fprintf(stderr, "[win32] CreateProcess(%s %s) not available\n", app ? app : "", cmd ? cmd : "");
  if (pi)
    memset(pi, 0, sizeof(*pi));
  SetLastError(ERROR_FILE_NOT_FOUND);
  return FALSE;
}
extern "C" HANDLE OpenProcess(DWORD, BOOL, DWORD) { return nullptr; }
extern "C" DWORD GetProcessVersion(DWORD) { return 0x00050001; }
extern "C" BOOL GetProcessTimes(HANDLE, LPFILETIME c, LPFILETIME e, LPFILETIME k, LPFILETIME u) {
  FILETIME zero = {0, 0};
  if (c)
    GetSystemTimeAsFileTime(c);
  if (e)
    *e = zero;
  if (k)
    *k = zero;
  if (u)
    *u = zero;
  return TRUE;
}
extern "C" BOOL GetExitCodeProcess(HANDLE, LPDWORD code) {
  if (code)
    *code = 0;
  return FALSE;
}
extern "C" HANDLE OpenMutexA(DWORD, BOOL, LPCSTR) {
  SetLastError(ERROR_FILE_NOT_FOUND); // no other instance can exist
  return nullptr;
}

// The dedicated-server console maps to stdout/stdin.
extern "C" BOOL AllocConsole(void) { return TRUE; }
extern "C" BOOL FreeConsole(void) { return TRUE; }
extern "C" HANDLE GetStdHandle(DWORD which) { return (HANDLE)(ULONG_PTR)(0x5000 + (which & 0xFF)); }
extern "C" BOOL SetConsoleTextAttribute(HANDLE, WORD) { return TRUE; }
extern "C" BOOL SetConsoleCursorPosition(HANDLE, COORD) { return TRUE; }
extern "C" BOOL SetConsoleTitleA(LPCSTR) { return TRUE; }
extern "C" BOOL GetConsoleScreenBufferInfo(HANDLE, CONSOLE_SCREEN_BUFFER_INFO *info) {
  memset(info, 0, sizeof(*info));
  info->dwSize.X = 80;
  info->dwSize.Y = 25;
  info->srWindow.Right = 79;
  info->srWindow.Bottom = 24;
  return TRUE;
}
extern "C" BOOL WriteConsoleA(HANDLE, const void *buf, DWORD n, LPDWORD written, LPVOID) {
  fwrite(buf, 1, n, stdout);
  if (written)
    *written = n;
  return TRUE;
}
extern "C" BOOL ReadConsoleA(HANDLE, LPVOID, DWORD, LPDWORD read, LPVOID) {
  if (read)
    *read = 0;
  return FALSE;
}

extern "C" DWORD GetLogicalDriveStringsA(DWORD len, LPSTR buf) {
  static const char drives[] = "C:\\\0"; // one fixed drive, no CD-ROM
  if (!buf || len < sizeof(drives))
    return sizeof(drives);
  memcpy(buf, drives, sizeof(drives));
  return sizeof(drives) - 1;
}
extern "C" HWND GetTopWindow(HWND) { return nullptr; }
extern "C" HINSTANCE FindExecutableA(LPCSTR, LPCSTR, LPSTR result) {
  if (result)
    result[0] = 0;
  return (HINSTANCE)SE_ERR_NOASSOC;
}

wchar_t *_itow(int value, wchar_t *buf, int radix) {
  char tmp[40];
  itoa(value, tmp, radix);
  size_t i = 0;
  for (; tmp[i]; i++)
    buf[i] = (wchar_t)tmp[i];
  buf[i] = 0;
  return buf;
}

extern "C" HRSRC FindResourceExA(HMODULE mod, LPCSTR type, LPCSTR name, WORD) {
  return FindResourceA(mod, name, type); // one language in the embedded .res
}
// Accelerators are handled by the engine's own key bindings on the web.
extern "C" HACCEL LoadAcceleratorsA(HINSTANCE, LPCSTR) { return nullptr; }
extern "C" BOOL SetConsoleScreenBufferSize(HANDLE, COORD) { return TRUE; }
extern "C" BOOL FillConsoleOutputAttribute(HANDLE, WORD, DWORD len, COORD, LPDWORD written) {
  if (written)
    *written = len;
  return TRUE;
}
extern "C" BOOL FillConsoleOutputCharacterA(HANDLE, CHAR, DWORD len, COORD, LPDWORD written) {
  if (written)
    *written = len;
  return TRUE;
}
extern "C" DWORD WaitForInputIdle(HANDLE, DWORD) { return WAIT_FAILED; }
extern "C" LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER) { return nullptr; }
extern "C" DWORD SearchPathA(LPCSTR path, LPCSTR file, LPCSTR ext, DWORD len, LPSTR buf, LPSTR *filepart) {
  char name[MAX_PATH];
  snprintf(name, sizeof(name), "%s%s%s", path && *path ? path : "", path && *path ? "/" : "", file ? file : "");
  if (ext && !strchr(file ? file : "", '.'))
    strncat(name, ext, sizeof(name) - strlen(name) - 1);
  if (GetFileAttributesA(name) == INVALID_FILE_ATTRIBUTES)
    return 0;
  return GetFullPathNameA(name, len, buf, filepart);
}
extern "C" DWORD GetWindowThreadProcessId(HWND, LPDWORD pid) {
  if (pid)
    *pid = GetCurrentProcessId();
  return GetCurrentThreadId();
}
// No interactive console: input functions report "no key".
extern "C" int _getch(void) { return 0; }
extern "C" int _getche(void) { return 0; }
extern "C" int _kbhit(void) { return 0; }
extern "C" int cprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vprintf(fmt, ap);
  va_end(ap);
  return n;
}
