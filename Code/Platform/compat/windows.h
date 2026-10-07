/*
** Rebel portable Win32 subset.
**
** Declares the slice of the Win32 API that the Renegade engine uses. Types
** follow the Win32 ILP32 ABI (the web target is wasm32, so DWORD == unsigned
** long == 32 bits). Implementations live in Code/Platform/win32/.
*/
#pragma once

#include "rebel_portable.h"
#include "tchar.h"

#define WINAPI
#define APIENTRY
#define CALLBACK
#define PASCAL
#define FAR
#define NEAR
#define far
#define near
#define CONST const
#define IN
#define OUT
#define OPTIONAL
#define UNALIGNED
#define VOID void

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned long ULONG;
typedef short SHORT;
typedef unsigned short USHORT;
typedef int INT;
typedef unsigned int UINT;
typedef char CHAR;
typedef unsigned char UCHAR;
typedef wchar_t WCHAR;
typedef float FLOAT;
typedef double DOUBLE;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef unsigned long long DWORD64;
typedef long long INT64;
typedef unsigned long long UINT64;
typedef int INT32;
typedef unsigned int UINT32;
typedef long LONG_PTR;
typedef unsigned long ULONG_PTR;
typedef int INT_PTR;
typedef unsigned int UINT_PTR;
typedef ULONG_PTR DWORD_PTR;
typedef ULONG_PTR SIZE_T;
typedef LONG_PTR SSIZE_T;
typedef BYTE BOOLEAN;
typedef void *PVOID;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef BOOL *LPBOOL;
typedef BOOL *PBOOL;
typedef BYTE *LPBYTE;
typedef BYTE *PBYTE;
typedef WORD *LPWORD;
typedef DWORD *LPDWORD;
typedef DWORD *PDWORD;
typedef LONG *LPLONG;
typedef LONG *PLONG;
typedef INT *LPINT;
typedef UINT *PUINT;
typedef char *LPSTR;
typedef char *PSTR;
typedef const char *LPCSTR;
typedef const char *PCSTR;
typedef wchar_t *LPWSTR;
typedef wchar_t *PWSTR;
typedef const wchar_t *LPCWSTR;
typedef const wchar_t *PCWSTR;
typedef char TCHAR;
typedef char *LPTSTR;
typedef const char *LPCTSTR;
typedef WORD ATOM;
typedef DWORD COLORREF;
typedef DWORD LCID;
typedef WORD LANGID;
typedef LONG HRESULT;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;

#define DECLARE_HANDLE(name) struct name##__ { int unused; }; typedef struct name##__ *name
typedef void *HANDLE;
typedef HANDLE *PHANDLE;
typedef HANDLE *LPHANDLE;
DECLARE_HANDLE(HWND);
DECLARE_HANDLE(HINSTANCE);
DECLARE_HANDLE(HDC);
DECLARE_HANDLE(HGLRC);
DECLARE_HANDLE(HKEY);
DECLARE_HANDLE(HMENU);
DECLARE_HANDLE(HICON);
DECLARE_HANDLE(HBITMAP);
DECLARE_HANDLE(HBRUSH);
DECLARE_HANDLE(HFONT);
DECLARE_HANDLE(HPEN);
DECLARE_HANDLE(HRGN);
DECLARE_HANDLE(HPALETTE);
DECLARE_HANDLE(HMONITOR);
DECLARE_HANDLE(HRSRC);
DECLARE_HANDLE(HIMC);
DECLARE_HANDLE(HKL);
DECLARE_HANDLE(HACCEL);
typedef HINSTANCE HMODULE;
typedef HICON HCURSOR;
typedef HANDLE HGLOBAL;
typedef HANDLE HLOCAL;
typedef HANDLE HGDIOBJ;
typedef HKEY *PHKEY;
typedef int HFILE;
typedef int (*FARPROC)();
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef INT_PTR (*DLGPROC)(HWND, UINT, WPARAM, LPARAM);
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
typedef DWORD ACCESS_MASK;
typedef ACCESS_MASK REGSAM;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL 0
#endif

#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INFINITE 0xFFFFFFFF
#define WAIT_OBJECT_0 0
#define WAIT_ABANDONED 0x80
#define WAIT_TIMEOUT 258
#define WAIT_FAILED ((DWORD)0xFFFFFFFF)

#define MAKEWORD(a, b) ((WORD)(((BYTE)((DWORD_PTR)(a) & 0xff)) | ((WORD)((BYTE)((DWORD_PTR)(b) & 0xff))) << 8))
#define MAKELONG(a, b) ((LONG)(((WORD)((DWORD_PTR)(a) & 0xffff)) | ((DWORD)((WORD)((DWORD_PTR)(b) & 0xffff))) << 16))
#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l) ((WORD)((DWORD_PTR)(l) >> 16))
#define LOBYTE(w) ((BYTE)((DWORD_PTR)(w) & 0xff))
#define HIBYTE(w) ((BYTE)((DWORD_PTR)(w) >> 8))
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)MAKELONG(l, h))
#define MAKEWPARAM(l, h) ((WPARAM)(DWORD)MAKELONG(l, h))
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(rgb) (LOBYTE(rgb))
#define GetGValue(rgb) (LOBYTE(((WORD)(rgb)) >> 8))
#define GetBValue(rgb) (LOBYTE((rgb) >> 16))
#define MAKEINTRESOURCE(i) ((LPSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCEA MAKEINTRESOURCE
#define UNREFERENCED_PARAMETER(P) (void)(P)
#define ZeroMemory(d, l) memset((d), 0, (l))
#define FillMemory(d, l, f) memset((d), (f), (l))
#define CopyMemory(d, s, l) memcpy((d), (s), (l))
#define MoveMemory(d, s, l) memmove((d), (s), (l))
#define TEXT(x) x
#define _T(x) x

#ifndef min
#ifndef __cplusplus
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#endif

/* HRESULT */
#define S_OK ((HRESULT)0L)
#define S_FALSE ((HRESULT)1L)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_POINTER ((HRESULT)0x80004003L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define E_UNEXPECTED ((HRESULT)0x8000FFFFL)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define MAKE_HRESULT(sev, fac, code) ((HRESULT)(((unsigned long)(sev) << 31) | ((unsigned long)(fac) << 16) | ((unsigned long)(code))))

/* Errors */
#define ERROR_SUCCESS 0L
#define NO_ERROR 0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NO_MORE_FILES 18L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_MORE_DATA 234L
#define ERROR_NO_MORE_ITEMS 259L
#define ERROR_IO_PENDING 997L

/* Files */
#define GENERIC_READ 0x80000000L
#define GENERIC_WRITE 0x40000000L
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_SYSTEM 0x00000004
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#define FILE_FLAG_WRITE_THROUGH 0x80000000
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;

typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATA, WIN32_FIND_DATAA, *LPWIN32_FIND_DATA, *PWIN32_FIND_DATA;

typedef struct _BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD dwVolumeSerialNumber, nFileSizeHigh, nFileSizeLow, nNumberOfLinks, nFileIndexHigh, nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION, *LPBY_HANDLE_FILE_INFORMATION;

typedef struct _SECURITY_ATTRIBUTES {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _OVERLAPPED {
    ULONG_PTR Internal, InternalHigh;
    DWORD Offset, OffsetHigh;
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    struct { DWORD LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; };
    struct { DWORD LowPart; DWORD HighPart; } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct tagPOINT { LONG x, y; } POINT, *PPOINT, *LPPOINT;
typedef struct tagSIZE { LONG cx, cy; } SIZE, *PSIZE, *LPSIZE;
typedef struct tagRECT { LONG left, top, right, bottom; } RECT, *PRECT, *LPRECT;
typedef const RECT *LPCRECT;

typedef struct _GUID {
    unsigned long Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char Data4[8];
} GUID, IID, CLSID;
typedef GUID *LPGUID;
typedef const GUID *LPCGUID;
#ifdef __cplusplus
typedef const GUID &REFGUID;
typedef const IID &REFIID;
typedef const CLSID &REFCLSID;
inline bool operator==(const GUID &a, const GUID &b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }
inline bool operator!=(const GUID &a, const GUID &b) { return !(a == b); }
#define IsEqualGUID(a, b) ((a) == (b))
#else
typedef const GUID *REFGUID;
typedef const IID *REFIID;
typedef const CLSID *REFCLSID;
#endif
#define IsEqualIID IsEqualGUID
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    static const GUID name = {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}

/* Window messages and misc UI constants the game references */
typedef struct tagMSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
} MSG, *PMSG, *LPMSG;

#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_GETMINMAXINFO 0x0024
#define WM_NCACTIVATE 0x0086
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_DEADCHAR 0x0103
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_IME_STARTCOMPOSITION 0x010D
#define WM_IME_ENDCOMPOSITION 0x010E
#define WM_IME_COMPOSITION 0x010F
#define WM_IME_KEYLAST 0x010F
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MBUTTONDBLCLK 0x0209
#define WM_MOUSEWHEEL 0x020A
#define WM_IME_SETCONTEXT 0x0281
#define WM_IME_NOTIFY 0x0282
#define WM_IME_CONTROL 0x0283
#define WM_IME_COMPOSITIONFULL 0x0284
#define WM_IME_SELECT 0x0285
#define WM_IME_CHAR 0x0286
#define WM_IME_REQUEST 0x0288
#define WM_IME_KEYDOWN 0x0290
#define WM_IME_KEYUP 0x0291
#define WM_INPUTLANGCHANGE 0x0051
#define WM_USER 0x0400
#define WM_APP 0x8000
#define WHEEL_DELTA 120
#define SC_SCREENSAVE 0xF140
#define SC_MONITORPOWER 0xF170
#define SC_KEYMENU 0xF100
#define SC_CLOSE 0xF060
#define WA_INACTIVE 0
#define WA_ACTIVE 1
#define WA_CLICKACTIVE 2
#define SIZE_MINIMIZED 1
#define PM_NOREMOVE 0x0000
#define PM_REMOVE 0x0001
#define MB_OK 0x00000000L
#define MB_OKCANCEL 0x00000001L
#define MB_YESNO 0x00000004L
#define MB_RETRYCANCEL 0x00000005L
#define MB_ABORTRETRYIGNORE 0x00000002L
#define MB_YESNOCANCEL 0x00000003L
#define MB_ICONHAND 0x00000010L
#define MB_ICONERROR MB_ICONHAND
#define MB_ICONSTOP MB_ICONHAND
#define MB_ICONQUESTION 0x00000020L
#define MB_ICONEXCLAMATION 0x00000030L
#define MB_ICONWARNING MB_ICONEXCLAMATION
#define MB_ICONINFORMATION 0x00000040L
#define MB_SYSTEMMODAL 0x00001000L
#define MB_TASKMODAL 0x00002000L
#define MB_SETFOREGROUND 0x00010000L
#define MB_TOPMOST 0x00040000L
#define MB_APPLMODAL 0x00000000L
#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_NORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_MAXIMIZE 3
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_RESTORE 9
#define SW_SHOWDEFAULT 10
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_CANCEL 0x03
#define VK_MBUTTON 0x04
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_CLEAR 0x0C
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SELECT 0x29
#define VK_PRINT 0x2A
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_HELP 0x2F
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_APPS 0x5D
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_PROCESSKEY 0xE5

/* Registry */
#define HKEY_CLASSES_ROOT ((HKEY)(ULONG_PTR)((LONG)0x80000000))
#define HKEY_CURRENT_USER ((HKEY)(ULONG_PTR)((LONG)0x80000001))
#define HKEY_LOCAL_MACHINE ((HKEY)(ULONG_PTR)((LONG)0x80000002))
#define HKEY_USERS ((HKEY)(ULONG_PTR)((LONG)0x80000003))
#define KEY_QUERY_VALUE 0x0001
#define KEY_SET_VALUE 0x0002
#define KEY_CREATE_SUB_KEY 0x0004
#define KEY_ENUMERATE_SUB_KEYS 0x0008
#define KEY_READ 0x20019
#define KEY_WRITE 0x20006
#define KEY_ALL_ACCESS 0xF003F
#define REG_OPTION_NON_VOLATILE 0
#define REG_CREATED_NEW_KEY 1
#define REG_OPENED_EXISTING_KEY 2
#define REG_NONE 0
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_BINARY 3
#define REG_DWORD 4
#define REG_MULTI_SZ 7

/* Code pages */
#define CP_ACP 0
#define CP_OEMCP 1
#define CP_UTF8 65001
#define MB_PRECOMPOSED 0x00000001
#define WC_COMPOSITECHECK 0x00000200

/* Threads */
#define THREAD_PRIORITY_LOWEST -2
#define THREAD_PRIORITY_BELOW_NORMAL -1
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_IDLE -15
#define CREATE_SUSPENDED 0x00000004
#define STILL_ACTIVE 259
#define NORMAL_PRIORITY_CLASS 0x00000020
#define HIGH_PRIORITY_CLASS 0x00000080

typedef struct _RTL_CRITICAL_SECTION {
    void *impl; /* pthread_mutex_t*, allocated by InitializeCriticalSection */
    LONG LockCount;
    LONG RecursionCount;
    HANDLE OwningThread;
    HANDLE LockSemaphore;
    ULONG_PTR SpinCount;
} CRITICAL_SECTION, *LPCRITICAL_SECTION, *PCRITICAL_SECTION;

typedef struct _MEMORYSTATUS {
    DWORD dwLength, dwMemoryLoad;
    SIZE_T dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile, dwTotalVirtual, dwAvailVirtual;
} MEMORYSTATUS, *LPMEMORYSTATUS;

typedef struct _OSVERSIONINFOA {
    DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
    CHAR szCSDVersion[128];
} OSVERSIONINFO, OSVERSIONINFOA, *LPOSVERSIONINFO;
#define VER_PLATFORM_WIN32s 0
#define VER_PLATFORM_WIN32_WINDOWS 1
#define VER_PLATFORM_WIN32_NT 2

typedef struct _SYSTEM_INFO {
    WORD wProcessorArchitecture, wReserved;
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress, lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
    WORD wProcessorLevel, wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;

#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE_READWRITE 0x40
#define GMEM_FIXED 0x0000
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GPTR (GMEM_FIXED | GMEM_ZEROINIT)
#define GHND (GMEM_MOVEABLE | GMEM_ZEROINIT)
#define LMEM_FIXED 0
#define LMEM_ZEROINIT 0x0040
#define LPTR (LMEM_FIXED | LMEM_ZEROINIT)
#define HEAP_ZERO_MEMORY 0x00000008

#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x00000200
#define FORMAT_MESSAGE_FROM_STRING 0x00000400
#define FORMAT_MESSAGE_FROM_SYSTEM 0x00001000
#define LANG_NEUTRAL 0x00
#define SUBLANG_DEFAULT 0x01
#define MAKELANGID(p, s) ((((WORD)(s)) << 10) | (WORD)(p))
#define PRIMARYLANGID(lgid) ((WORD)(lgid) & 0x3ff)
#define SUBLANGID(lgid) ((WORD)(lgid) >> 10)
#define LANG_ENGLISH 0x09
#define LANG_GERMAN 0x07
#define LANG_FRENCH 0x0c
#define LANG_JAPANESE 0x11
#define LANG_KOREAN 0x12
#define LANG_CHINESE 0x04

#ifdef __cplusplus
extern "C" {
#endif

/* errors / debug */
DWORD GetLastError(void);
void SetLastError(DWORD err);
void OutputDebugStringA(LPCSTR s);
void OutputDebugStringW(LPCWSTR s);
#define OutputDebugString OutputDebugStringA
void DebugBreak(void);
int MessageBoxA(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type);
int MessageBoxW(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type);
#define MessageBox MessageBoxA
DWORD FormatMessageA(DWORD flags, LPCVOID src, DWORD msgid, DWORD langid, LPSTR buf, DWORD size, va_list *args);
#define FormatMessage FormatMessageA
BOOL IsDebuggerPresent(void);

/* time */
DWORD GetTickCount(void);
DWORD timeGetTime(void);
BOOL QueryPerformanceCounter(LARGE_INTEGER *count);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *freq);
void Sleep(DWORD ms);
void GetSystemTime(LPSYSTEMTIME st);
void GetLocalTime(LPSYSTEMTIME st);
void GetSystemTimeAsFileTime(LPFILETIME ft);
BOOL SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft);
BOOL FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st);
BOOL FileTimeToLocalFileTime(const FILETIME *ft, LPFILETIME out);
LONG CompareFileTime(const FILETIME *a, const FILETIME *b);
UINT timeBeginPeriod(UINT p);
UINT timeEndPeriod(UINT p);

/* files */
HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE templ);
#define CreateFile CreateFileA
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED ov);
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPOVERLAPPED ov);
DWORD SetFilePointer(HANDLE h, LONG dist, PLONG disthigh, DWORD method);
BOOL SetEndOfFile(HANDLE h);
DWORD GetFileSize(HANDLE h, LPDWORD high);
BOOL GetFileTime(HANDLE h, LPFILETIME c, LPFILETIME a, LPFILETIME w);
BOOL SetFileTime(HANDLE h, const FILETIME *c, const FILETIME *a, const FILETIME *w);
BOOL GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info);
BOOL FlushFileBuffers(HANDLE h);
BOOL CloseHandle(HANDLE h);
BOOL DeleteFileA(LPCSTR name);
#define DeleteFile DeleteFileA
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
#define CopyFile CopyFileA
BOOL MoveFileA(LPCSTR from, LPCSTR to);
#define MoveFile MoveFileA
DWORD GetFileAttributesA(LPCSTR name);
#define GetFileAttributes GetFileAttributesA
BOOL SetFileAttributesA(LPCSTR name, DWORD attr);
#define SetFileAttributes SetFileAttributesA
BOOL CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa);
#define CreateDirectory CreateDirectoryA
BOOL RemoveDirectoryA(LPCSTR name);
#define RemoveDirectory RemoveDirectoryA
DWORD GetCurrentDirectoryA(DWORD len, LPSTR buf);
#define GetCurrentDirectory GetCurrentDirectoryA
BOOL SetCurrentDirectoryA(LPCSTR dir);
#define SetCurrentDirectory SetCurrentDirectoryA
DWORD GetFullPathNameA(LPCSTR name, DWORD len, LPSTR buf, LPSTR *filepart);
#define GetFullPathName GetFullPathNameA
DWORD GetTempPathA(DWORD len, LPSTR buf);
#define GetTempPath GetTempPathA
UINT GetTempFileNameA(LPCSTR path, LPCSTR prefix, UINT unique, LPSTR out);
#define GetTempFileName GetTempFileNameA
HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATA data);
#define FindFirstFile FindFirstFileA
BOOL FindNextFileA(HANDLE h, LPWIN32_FIND_DATA data);
#define FindNextFile FindNextFileA
BOOL FindClose(HANDLE h);
UINT GetDriveTypeA(LPCSTR root);
#define GetDriveType GetDriveTypeA
DWORD GetLogicalDrives(void);
BOOL GetDiskFreeSpaceA(LPCSTR root, LPDWORD spc, LPDWORD bps, LPDWORD freec, LPDWORD totalc);
#define GetDiskFreeSpace GetDiskFreeSpaceA
BOOL GetVolumeInformationA(LPCSTR root, LPSTR name, DWORD namelen, LPDWORD serial, LPDWORD maxlen, LPDWORD flags, LPSTR fsname, DWORD fsnamelen);
#define GetVolumeInformation GetVolumeInformationA
#define DRIVE_UNKNOWN 0
#define DRIVE_NO_ROOT_DIR 1
#define DRIVE_REMOVABLE 2
#define DRIVE_FIXED 3
#define DRIVE_REMOTE 4
#define DRIVE_CDROM 5
#define DRIVE_RAMDISK 6

/* modules / process */
DWORD GetModuleFileNameA(HMODULE mod, LPSTR buf, DWORD size);
#define GetModuleFileName GetModuleFileNameA
HMODULE GetModuleHandleA(LPCSTR name);
#define GetModuleHandle GetModuleHandleA
HMODULE LoadLibraryA(LPCSTR name);
#define LoadLibrary LoadLibraryA
BOOL FreeLibrary(HMODULE mod);
FARPROC GetProcAddress(HMODULE mod, LPCSTR name);
HANDLE GetCurrentProcess(void);
DWORD GetCurrentProcessId(void);
HANDLE GetCurrentThread(void);
DWORD GetCurrentThreadId(void);
BOOL SetPriorityClass(HANDLE p, DWORD c);
DWORD GetPriorityClass(HANDLE p);
void ExitProcess(UINT code);
BOOL TerminateProcess(HANDLE p, UINT code);
LPSTR GetCommandLineA(void);
#define GetCommandLine GetCommandLineA
DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size);
#define GetEnvironmentVariable GetEnvironmentVariableA
BOOL GetComputerNameA(LPSTR buf, LPDWORD size);
#define GetComputerName GetComputerNameA
BOOL GetUserNameA(LPSTR buf, LPDWORD size);
#define GetUserName GetUserNameA
BOOL GetVersionExA(LPOSVERSIONINFO info);
#define GetVersionEx GetVersionExA
DWORD GetVersion(void);
void GetSystemInfo(LPSYSTEM_INFO info);
void GlobalMemoryStatus(LPMEMORYSTATUS ms);
UINT GetWindowsDirectoryA(LPSTR buf, UINT size);
#define GetWindowsDirectory GetWindowsDirectoryA
UINT GetSystemDirectoryA(LPSTR buf, UINT size);
#define GetSystemDirectory GetSystemDirectoryA
LCID GetUserDefaultLCID(void);
LANGID GetUserDefaultLangID(void);
LANGID GetSystemDefaultLangID(void);
UINT GetACP(void);
int LoadStringA(HINSTANCE inst, UINT id, LPSTR buf, int max);
#define LoadString LoadStringA
int LoadStringW(HINSTANCE inst, UINT id, LPWSTR buf, int max);

/* threads / sync */
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD id);
DWORD ResumeThread(HANDLE t);
DWORD SuspendThread(HANDLE t);
BOOL TerminateThread(HANDLE t, DWORD code);
BOOL GetExitCodeThread(HANDLE t, LPDWORD code);
BOOL SetThreadPriority(HANDLE t, int prio);
int GetThreadPriority(HANDLE t);
void ExitThread(DWORD code);
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owner, LPCSTR name);
#define CreateMutex CreateMutexA
BOOL ReleaseMutex(HANDLE m);
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name);
#define CreateEvent CreateEventA
BOOL SetEvent(HANDLE e);
BOOL ResetEvent(HANDLE e);
HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG max, LPCSTR name);
#define CreateSemaphore CreateSemaphoreA
BOOL ReleaseSemaphore(HANDLE s, LONG n, LPLONG prev);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms);
void InitializeCriticalSection(LPCRITICAL_SECTION cs);
void DeleteCriticalSection(LPCRITICAL_SECTION cs);
void EnterCriticalSection(LPCRITICAL_SECTION cs);
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs);
void LeaveCriticalSection(LPCRITICAL_SECTION cs);
LONG InterlockedIncrement(LONG volatile *v);
LONG InterlockedDecrement(LONG volatile *v);
LONG InterlockedExchange(LONG volatile *v, LONG x);
LONG InterlockedExchangeAdd(LONG volatile *v, LONG x);
LONG InterlockedCompareExchange(LONG volatile *v, LONG x, LONG cmp);
DWORD TlsAlloc(void);
BOOL TlsFree(DWORD i);
LPVOID TlsGetValue(DWORD i);
BOOL TlsSetValue(DWORD i, LPVOID v);
#define TLS_OUT_OF_INDEXES ((DWORD)0xFFFFFFFF)

/* memory */
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, PDWORD old);
HGLOBAL GlobalAlloc(UINT flags, SIZE_T size);
HGLOBAL GlobalFree(HGLOBAL h);
LPVOID GlobalLock(HGLOBAL h);
BOOL GlobalUnlock(HGLOBAL h);
SIZE_T GlobalSize(HGLOBAL h);
HLOCAL LocalAlloc(UINT flags, SIZE_T size);
HLOCAL LocalFree(HLOCAL h);
HANDLE GetProcessHeap(void);
LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size);
BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID p);
BOOL IsBadReadPtr(const void *p, UINT_PTR n);
BOOL IsBadWritePtr(LPVOID p, UINT_PTR n);
BOOL IsBadCodePtr(FARPROC p);

/* strings */
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int srclen, LPWSTR dst, int dstlen);
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int srclen, LPSTR dst, int dstlen, LPCSTR defchar, LPBOOL useddef);
int lstrlenA(LPCSTR s);
int lstrlenW(LPCWSTR s);
#define lstrlen lstrlenA
LPSTR lstrcpyA(LPSTR d, LPCSTR s);
LPWSTR lstrcpyW(LPWSTR d, LPCWSTR s);
#define lstrcpy lstrcpyA
LPSTR lstrcpynA(LPSTR d, LPCSTR s, int n);
LPWSTR lstrcpynW(LPWSTR d, LPCWSTR s, int n);
#define lstrcpyn lstrcpynA
LPSTR lstrcatA(LPSTR d, LPCSTR s);
LPWSTR lstrcatW(LPWSTR d, LPCWSTR s);
#define lstrcat lstrcatA
int lstrcmpA(LPCSTR a, LPCSTR b);
int lstrcmpW(LPCWSTR a, LPCWSTR b);
#define lstrcmp lstrcmpA
int lstrcmpiA(LPCSTR a, LPCSTR b);
int lstrcmpiW(LPCWSTR a, LPCWSTR b);
#define lstrcmpi lstrcmpiA
LPSTR CharUpperA(LPSTR s);
LPSTR CharLowerA(LPSTR s);
#define CharUpper CharUpperA
#define CharLower CharLowerA
LPSTR CharNextA(LPCSTR s);
#define CharNext CharNextA
BOOL IsCharAlphaA(CHAR c);
BOOL IsCharAlphaNumericA(CHAR c);
#define IsCharAlpha IsCharAlphaA
#define IsCharAlphaNumeric IsCharAlphaNumericA
int wsprintfA(LPSTR buf, LPCSTR fmt, ...);
#define wsprintf wsprintfA
int wvsprintfA(LPSTR buf, LPCSTR fmt, va_list args);
#define wvsprintf wvsprintfA
int GetDateFormatA(LCID l, DWORD f, const SYSTEMTIME *t, LPCSTR fmt, LPSTR buf, int n);
int GetTimeFormatA(LCID l, DWORD f, const SYSTEMTIME *t, LPCSTR fmt, LPSTR buf, int n);
int GetDateFormatW(LCID l, DWORD f, const SYSTEMTIME *t, LPCWSTR fmt, LPWSTR buf, int n);
int GetTimeFormatW(LCID l, DWORD f, const SYSTEMTIME *t, LPCWSTR fmt, LPWSTR buf, int n);
#define GetDateFormat GetDateFormatA
#define GetTimeFormat GetTimeFormatA
#define DATE_SHORTDATE 0x00000001
#define DATE_LONGDATE 0x00000002
#define TIME_NOSECONDS 0x00000002
#define LOCALE_SYSTEM_DEFAULT 0x0800
#define LOCALE_USER_DEFAULT 0x0400

/* registry (backed by a persistent key/value file) */
LONG RegOpenKeyExA(HKEY key, LPCSTR sub, DWORD opts, REGSAM sam, PHKEY out);
#define RegOpenKeyEx RegOpenKeyExA
LONG RegOpenKeyA(HKEY key, LPCSTR sub, PHKEY out);
#define RegOpenKey RegOpenKeyA
LONG RegCreateKeyExA(HKEY key, LPCSTR sub, DWORD reserved, LPSTR cls, DWORD opts, REGSAM sam, LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp);
#define RegCreateKeyEx RegCreateKeyExA
LONG RegCreateKeyA(HKEY key, LPCSTR sub, PHKEY out);
#define RegCreateKey RegCreateKeyA
LONG RegCloseKey(HKEY key);
LONG RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size);
#define RegQueryValueEx RegQueryValueExA
LONG RegSetValueExA(HKEY key, LPCSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD size);
#define RegSetValueEx RegSetValueExA
LONG RegDeleteValueA(HKEY key, LPCSTR name);
#define RegDeleteValue RegDeleteValueA
LONG RegDeleteKeyA(HKEY key, LPCSTR sub);
#define RegDeleteKey RegDeleteKeyA
LONG RegEnumValueA(HKEY key, DWORD index, LPSTR name, LPDWORD namelen, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD datalen);
#define RegEnumValue RegEnumValueA
LONG RegEnumKeyExA(HKEY key, DWORD index, LPSTR name, LPDWORD namelen, LPDWORD reserved, LPSTR cls, LPDWORD clslen, LPFILETIME ft);
#define RegEnumKeyEx RegEnumKeyExA
LONG RegEnumKeyA(HKEY key, DWORD index, LPSTR name, DWORD namelen);
#define RegEnumKey RegEnumKeyA
LONG RegQueryInfoKeyA(HKEY key, LPSTR cls, LPDWORD clslen, LPDWORD reserved, LPDWORD subkeys, LPDWORD maxsubkeylen, LPDWORD maxclasslen, LPDWORD values, LPDWORD maxvaluenamelen, LPDWORD maxvaluelen, LPDWORD secdesc, LPFILETIME ft);
#define RegQueryInfoKey RegQueryInfoKeyA
LONG RegFlushKey(HKEY key);

/* windowing: a single canvas-backed window */
typedef struct tagWNDCLASSA {
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra, cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCSTR lpszMenuName;
    LPCSTR lpszClassName;
} WNDCLASS, WNDCLASSA, *LPWNDCLASS;
typedef struct tagCREATESTRUCTA {
    LPVOID lpCreateParams;
    HINSTANCE hInstance;
    HMENU hMenu;
    HWND hwndParent;
    int cy, cx, y, x;
    LONG style;
    LPCSTR lpszName, lpszClass;
    DWORD dwExStyle;
} CREATESTRUCT, *LPCREATESTRUCT;
typedef struct tagMINMAXINFO {
    POINT ptReserved, ptMaxSize, ptMaxPosition, ptMinTrackSize, ptMaxTrackSize;
} MINMAXINFO, *LPMINMAXINFO;
typedef struct tagPAINTSTRUCT {
    HDC hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore, fIncUpdate;
    BYTE rgbReserved[32];
} PAINTSTRUCT, *LPPAINTSTRUCT;

#define CS_VREDRAW 0x0001
#define CS_HREDRAW 0x0002
#define CS_DBLCLKS 0x0008
#define CS_OWNDC 0x0020
#define CS_CLASSDC 0x0040
#define WS_OVERLAPPED 0x00000000L
#define WS_POPUP 0x80000000L
#define WS_CHILD 0x40000000L
#define WS_MINIMIZE 0x20000000L
#define WS_VISIBLE 0x10000000L
#define WS_DISABLED 0x08000000L
#define WS_CLIPSIBLINGS 0x04000000L
#define WS_CLIPCHILDREN 0x02000000L
#define WS_MAXIMIZE 0x01000000L
#define WS_CAPTION 0x00C00000L
#define WS_BORDER 0x00800000L
#define WS_DLGFRAME 0x00400000L
#define WS_SYSMENU 0x00080000L
#define WS_THICKFRAME 0x00040000L
#define WS_MINIMIZEBOX 0x00020000L
#define WS_MAXIMIZEBOX 0x00010000L
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_EX_TOPMOST 0x00000008L
#define WS_EX_APPWINDOW 0x00040000L
#define CW_USEDEFAULT ((int)0x80000000)
#define GWL_WNDPROC (-4)
#define GWL_HINSTANCE (-6)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_USERDATA (-21)
#define GWLP_USERDATA (-21)
#define HWND_TOP ((HWND)0)
#define HWND_TOPMOST ((HWND)-1)
#define HWND_NOTOPMOST ((HWND)-2)
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOACTIVATE 0x0010
#define SWP_SHOWWINDOW 0x0040
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXFULLSCREEN 16
#define SM_CYFULLSCREEN 17
#define SM_CXDOUBLECLK 36
#define SM_CYDOUBLECLK 37
#define IDC_ARROW MAKEINTRESOURCE(32512)
#define IDC_WAIT MAKEINTRESOURCE(32514)
#define IDI_APPLICATION MAKEINTRESOURCE(32512)
#define BLACK_BRUSH 4
#define NULL_BRUSH 5

ATOM RegisterClassA(const WNDCLASS *wc);
#define RegisterClass RegisterClassA
BOOL UnregisterClassA(LPCSTR name, HINSTANCE inst);
#define UnregisterClass UnregisterClassA
HWND CreateWindowExA(DWORD exstyle, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
#define CreateWindowEx CreateWindowExA
#define CreateWindow(c, n, s, x, y, w, h, p, m, i, l) CreateWindowExA(0, c, n, s, x, y, w, h, p, m, i, l)
BOOL DestroyWindow(HWND w);
BOOL ShowWindow(HWND w, int cmd);
BOOL UpdateWindow(HWND w);
HWND SetFocus(HWND w);
HWND GetFocus(void);
HWND GetActiveWindow(void);
HWND GetForegroundWindow(void);
BOOL SetForegroundWindow(HWND w);
HWND SetActiveWindow(HWND w);
BOOL IsWindow(HWND w);
BOOL IsWindowVisible(HWND w);
BOOL IsIconic(HWND w);
BOOL GetClientRect(HWND w, LPRECT r);
BOOL GetWindowRect(HWND w, LPRECT r);
BOOL AdjustWindowRect(LPRECT r, DWORD style, BOOL menu);
BOOL AdjustWindowRectEx(LPRECT r, DWORD style, BOOL menu, DWORD exstyle);
BOOL SetWindowPos(HWND w, HWND after, int x, int y, int cx, int cy, UINT flags);
BOOL MoveWindow(HWND w, int x, int y, int cx, int cy, BOOL repaint);
BOOL SetWindowTextA(HWND w, LPCSTR text);
#define SetWindowText SetWindowTextA
LONG GetWindowLongA(HWND w, int idx);
LONG SetWindowLongA(HWND w, int idx, LONG v);
#define GetWindowLong GetWindowLongA
#define SetWindowLong SetWindowLongA
#define GetWindowLongPtr GetWindowLongA
#define SetWindowLongPtr SetWindowLongA
HWND FindWindowA(LPCSTR cls, LPCSTR name);
#define FindWindow FindWindowA
BOOL ClientToScreen(HWND w, LPPOINT p);
BOOL ScreenToClient(HWND w, LPPOINT p);
int GetSystemMetrics(int idx);
LRESULT DefWindowProcA(HWND w, UINT msg, WPARAM wp, LPARAM lp);
#define DefWindowProc DefWindowProcA
LRESULT CallWindowProcA(WNDPROC proc, HWND w, UINT msg, WPARAM wp, LPARAM lp);
#define CallWindowProc CallWindowProcA
BOOL PeekMessageA(LPMSG msg, HWND w, UINT min, UINT max, UINT remove);
#define PeekMessage PeekMessageA
BOOL GetMessageA(LPMSG msg, HWND w, UINT min, UINT max);
#define GetMessage GetMessageA
BOOL TranslateMessage(const MSG *msg);
LRESULT DispatchMessageA(const MSG *msg);
#define DispatchMessage DispatchMessageA
BOOL PostMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp);
#define PostMessage PostMessageA
LRESULT SendMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp);
#define SendMessage SendMessageA
void PostQuitMessage(int code);
BOOL IsDialogMessageA(HWND dlg, LPMSG msg);
#define IsDialogMessage IsDialogMessageA
int TranslateAcceleratorA(HWND w, HACCEL acc, LPMSG msg);
#define TranslateAccelerator TranslateAcceleratorA
UINT_PTR SetTimer(HWND w, UINT_PTR id, UINT ms, void *fn);
BOOL KillTimer(HWND w, UINT_PTR id);
int ShowCursor(BOOL show);
HCURSOR SetCursor(HCURSOR c);
HCURSOR LoadCursorA(HINSTANCE inst, LPCSTR name);
#define LoadCursor LoadCursorA
HICON LoadIconA(HINSTANCE inst, LPCSTR name);
#define LoadIcon LoadIconA
BOOL GetCursorPos(LPPOINT p);
BOOL SetCursorPos(int x, int y);
BOOL ClipCursor(const RECT *r);
HWND SetCapture(HWND w);
BOOL ReleaseCapture(void);
SHORT GetAsyncKeyState(int vk);
SHORT GetKeyState(int vk);
BOOL GetKeyboardState(PBYTE state);
UINT MapVirtualKeyA(UINT code, UINT type);
#define MapVirtualKey MapVirtualKeyA
int ToAscii(UINT vk, UINT scan, const BYTE *state, LPWORD out, UINT flags);
HKL GetKeyboardLayout(DWORD thread);
UINT GetDoubleClickTime(void);
BOOL SystemParametersInfoA(UINT action, UINT param, PVOID pv, UINT winini);
#define SystemParametersInfo SystemParametersInfoA
BOOL InvalidateRect(HWND w, const RECT *r, BOOL erase);
BOOL ValidateRect(HWND w, const RECT *r);
HDC BeginPaint(HWND w, LPPAINTSTRUCT ps);
BOOL EndPaint(HWND w, const PAINTSTRUCT *ps);
HDC GetDC(HWND w);
int ReleaseDC(HWND w, HDC dc);
HGDIOBJ GetStockObject(int i);
BOOL DeleteObject(HGDIOBJ o);
HGDIOBJ SelectObject(HDC dc, HGDIOBJ o);
int GetDeviceCaps(HDC dc, int idx);
#define HORZRES 8
#define VERTRES 10
#define BITSPIXEL 12
#define VREFRESH 116
BOOL MessageBeep(UINT type);
BOOL FlashWindow(HWND w, BOOL invert);
BOOL EnableWindow(HWND w, BOOL enable);
HWND GetDlgItem(HWND dlg, int id);
BOOL EndDialog(HWND dlg, INT_PTR result);
INT_PTR DialogBoxParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM param);
#define DialogBoxParam DialogBoxParamA
#define DialogBox(i, t, p, f) DialogBoxParamA(i, t, p, f, 0)
HWND CreateDialogParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM param);
#define CreateDialogParam CreateDialogParamA
#define CreateDialog(i, t, p, f) CreateDialogParamA(i, t, p, f, 0)
BOOL SetDlgItemTextA(HWND dlg, int id, LPCSTR text);
#define SetDlgItemText SetDlgItemTextA
UINT GetDlgItemTextA(HWND dlg, int id, LPSTR text, int max);
#define GetDlgItemText GetDlgItemTextA
BOOL CheckDlgButton(HWND dlg, int id, UINT check);
UINT IsDlgButtonChecked(HWND dlg, int id);
LRESULT SendDlgItemMessageA(HWND dlg, int id, UINT msg, WPARAM wp, LPARAM lp);
#define SendDlgItemMessage SendDlgItemMessageA
BOOL SetRect(LPRECT r, int l, int t, int ri, int b);
BOOL PtInRect(const RECT *r, POINT p);
BOOL IntersectRect(LPRECT d, const RECT *a, const RECT *b);
BOOL OffsetRect(LPRECT r, int dx, int dy);
BOOL IsRectEmpty(const RECT *r);
HINSTANCE ShellExecuteA(HWND w, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR dir, INT show);
#define ShellExecute ShellExecuteA
BOOL OpenClipboard(HWND w);
BOOL CloseClipboard(void);
HANDLE GetClipboardData(UINT fmt);
BOOL IsClipboardFormatAvailable(UINT fmt);
#define CF_TEXT 1
#define CF_UNICODETEXT 13

/* IME (Asian text input) — inactive on the web build */
HIMC ImmGetContext(HWND w);
BOOL ImmReleaseContext(HWND w, HIMC imc);
HIMC ImmAssociateContext(HWND w, HIMC imc);

#ifdef __cplusplus
}
#endif

/* Minimal COM base */
#ifdef __cplusplus
#ifndef interface
#define interface struct
#endif
#define STDMETHODCALLTYPE
#define STDMETHOD(m) virtual HRESULT STDMETHODCALLTYPE m
#define STDMETHOD_(t, m) virtual t STDMETHODCALLTYPE m
#define STDMETHODIMP HRESULT STDMETHODCALLTYPE
#define STDMETHODIMP_(t) t STDMETHODCALLTYPE
#define PURE = 0
#define THIS_
#define THIS void
#define DECLARE_INTERFACE(i) struct i
#define DECLARE_INTERFACE_(i, b) struct i : public b
struct IUnknown {
    virtual HRESULT QueryInterface(REFIID riid, void **ppv) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;
    virtual ~IUnknown() {}
};
typedef IUnknown *LPUNKNOWN;
#endif

/* Late additions: resources, version info, priority */
#define REALTIME_PRIORITY_CLASS 0x00000100
#define IDLE_PRIORITY_CLASS 0x00000040
#define RT_RCDATA MAKEINTRESOURCE(10)
#define RT_STRING MAKEINTRESOURCE(6)
typedef struct tagVS_FIXEDFILEINFO {
    DWORD dwSignature, dwStrucVersion, dwFileVersionMS, dwFileVersionLS, dwProductVersionMS, dwProductVersionLS;
    DWORD dwFileFlagsMask, dwFileFlags, dwFileOS, dwFileType, dwFileSubtype, dwFileDateMS, dwFileDateLS;
} VS_FIXEDFILEINFO;
#ifdef __cplusplus
extern "C" {
#endif
HRSRC FindResourceA(HMODULE mod, LPCSTR name, LPCSTR type);
#define FindResource FindResourceA
HGLOBAL LoadResource(HMODULE mod, HRSRC res);
LPVOID LockResource(HGLOBAL res);
DWORD SizeofResource(HMODULE mod, HRSRC res);
DWORD GetFileVersionInfoSizeA(LPCSTR name, LPDWORD handle);
#define GetFileVersionInfoSize GetFileVersionInfoSizeA
BOOL GetFileVersionInfoA(LPCSTR name, DWORD handle, DWORD len, LPVOID data);
#define GetFileVersionInfo GetFileVersionInfoA
BOOL VerQueryValueA(LPCVOID block, LPCSTR sub, LPVOID *buf, PUINT len);
#define VerQueryValue VerQueryValueA
#ifdef __cplusplus
}
#endif

typedef struct _TIME_ZONE_INFORMATION {
    LONG Bias;
    WCHAR StandardName[32];
    SYSTEMTIME StandardDate;
    LONG StandardBias;
    WCHAR DaylightName[32];
    SYSTEMTIME DaylightDate;
    LONG DaylightBias;
} TIME_ZONE_INFORMATION, *LPTIME_ZONE_INFORMATION;
#define TIME_ZONE_ID_UNKNOWN 0

/* PE image headers (only used to read the exe's own link timestamp) */
typedef struct _IMAGE_DOS_HEADER {
    WORD e_magic, e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc, e_ss, e_sp, e_csum, e_ip, e_cs, e_lfarlc, e_ovno;
    WORD e_res[4];
    WORD e_oemid, e_oeminfo;
    WORD e_res2[10];
    LONG e_lfanew;
} IMAGE_DOS_HEADER, *PIMAGE_DOS_HEADER;
typedef struct _IMAGE_FILE_HEADER {
    WORD Machine, NumberOfSections;
    DWORD TimeDateStamp, PointerToSymbolTable, NumberOfSymbols;
    WORD SizeOfOptionalHeader, Characteristics;
} IMAGE_FILE_HEADER, *PIMAGE_FILE_HEADER;
#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550

#ifdef __cplusplus
extern "C" {
#endif
DWORD GetTimeZoneInformation(LPTIME_ZONE_INFORMATION tz);
BOOL FileTimeToDosDateTime(const FILETIME *ft, LPWORD date, LPWORD time);
BOOL DosDateTimeToFileTime(WORD date, WORD time, LPFILETIME ft);
LONG RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size);
LONG RegSetValueExW(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD size);
#ifdef __cplusplus
}
#endif
