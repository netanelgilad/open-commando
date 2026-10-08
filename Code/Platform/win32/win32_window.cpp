/*
** Win32 windowing and input on a single browser canvas.
**
** The game creates one top-level window. Its WNDPROC receives messages that
** the web input layer (Platform/web) posts via Rebel_Post_Message(); key and
** mouse state for GetAsyncKeyState/GetCursorPos is tracked here as well.
** Dialog boxes, menus and GDI painting are inert.
*/
#include "windows.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace {

struct Window {
  WNDPROC proc = nullptr;
  std::string title;
  DWORD style = 0;
  DWORD exstyle = 0;
  RECT rect{0, 0, 800, 600};
  bool visible = false;
  LONG user_data = 0;
  HINSTANCE inst = nullptr;
};

// Built on first use: game code runs before main() (static constructors)
// and may reach this before this file's globals are initialized.
std::recursive_mutex &Lock_Instance() {
  static std::recursive_mutex *instance = new std::recursive_mutex();
  return *instance;
}
#define Lock Lock_Instance()
// Built on first use: game code runs before main() (static constructors)
// and may reach this before this file's globals are initialized.
std::map<std::string, WNDCLASS> &Classes_Instance() {
  static std::map<std::string, WNDCLASS> *instance = new std::map<std::string, WNDCLASS>();
  return *instance;
}
#define Classes Classes_Instance()
// Built on first use: game code runs before main() (static constructors)
// and may reach this before this file's globals are initialized.
Window &MainWindow_Instance() {
  static Window *instance = new Window();
  return *instance;
}
#define MainWindow MainWindow_Instance()
bool MainWindowExists = false;
HWND const MainHandle = (HWND)0x1000;
// Built on first use: game code runs before main() (static constructors)
// and may reach this before this file's globals are initialized.
std::deque<MSG> &Queue_Instance() {
  static std::deque<MSG> *instance = new std::deque<MSG>();
  return *instance;
}
#define Queue Queue_Instance()
bool QuitPosted = false;
BYTE KeyState[256];
POINT Cursor{0, 0};
int CursorCount = 0;
HWND Focus = nullptr;
int ScreenW = 1024, ScreenH = 768;

Window *Get(HWND w) { return (w == MainHandle && MainWindowExists) ? &MainWindow : nullptr; }

void Track_Input(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_KEYDOWN:
  case WM_SYSKEYDOWN: KeyState[wp & 0xFF] |= 0x80; break;
  case WM_KEYUP:
  case WM_SYSKEYUP: KeyState[wp & 0xFF] &= ~0x80; break;
  case WM_LBUTTONDOWN: KeyState[VK_LBUTTON] |= 0x80; break;
  case WM_LBUTTONUP: KeyState[VK_LBUTTON] &= ~0x80; break;
  case WM_RBUTTONDOWN: KeyState[VK_RBUTTON] |= 0x80; break;
  case WM_RBUTTONUP: KeyState[VK_RBUTTON] &= ~0x80; break;
  case WM_MBUTTONDOWN: KeyState[VK_MBUTTON] |= 0x80; break;
  case WM_MBUTTONUP: KeyState[VK_MBUTTON] &= ~0x80; break;
  default: break;
  }
  if (msg >= WM_MOUSEMOVE && msg <= WM_MBUTTONDBLCLK) {
    Cursor.x = GET_X_LPARAM(lp);
    Cursor.y = GET_Y_LPARAM(lp);
  }
  // Left/right modifier variants mirror the generic ones.
  KeyState[VK_SHIFT] = (KeyState[VK_LSHIFT] | KeyState[VK_RSHIFT] | KeyState[VK_SHIFT]) & 0x80;
}

} // namespace

/* ---------------------------------------------- hooks for the web runtime */

extern "C" void Rebel_Post_Message(UINT msg, WPARAM wp, LPARAM lp) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  Track_Input(msg, wp, lp);
  if (!MainWindowExists)
    return;
  MSG m{};
  m.hwnd = MainHandle;
  m.message = msg;
  m.wParam = wp;
  m.lParam = lp;
  m.time = GetTickCount();
  m.pt = Cursor;
  Queue.push_back(m);
}

extern "C" void Rebel_Set_Screen_Size(int w, int h) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  ScreenW = w;
  ScreenH = h;
}

extern "C" HWND Rebel_Main_Window(void) { return MainWindowExists ? MainHandle : nullptr; }

/* ------------------------------------------------------------ class/window */

static void Activate_Main_Window(HWND w);

extern "C" ATOM RegisterClassA(const WNDCLASS *wc) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  Classes[wc->lpszClassName ? wc->lpszClassName : ""] = *wc;
  return (ATOM)Classes.size();
}

extern "C" BOOL UnregisterClassA(LPCSTR name, HINSTANCE) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  return Classes.erase(name ? name : "") > 0;
}

extern "C" HWND CreateWindowExA(DWORD exstyle, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
                                HWND, HMENU, HINSTANCE inst, LPVOID param) {
  WNDPROC proc = nullptr;
  {
    std::lock_guard<std::recursive_mutex> g(Lock);
    auto it = Classes.find(cls ? cls : "");
    if (it == Classes.end()) {
      fprintf(stderr, "[win32] CreateWindowEx: class '%s' not registered\n", cls ? cls : "");
      return nullptr;
    }
    if (MainWindowExists) {
      fprintf(stderr, "[win32] CreateWindowEx: only one window is supported\n");
      return nullptr;
    }
    proc = it->second.lpfnWndProc;
    MainWindow = Window();
    MainWindow.proc = proc;
    MainWindow.title = name ? name : "";
    MainWindow.style = style;
    MainWindow.exstyle = exstyle;
    MainWindow.inst = inst;
    if (x == CW_USEDEFAULT)
      x = 0;
    if (y == CW_USEDEFAULT)
      y = 0;
    if (w == CW_USEDEFAULT)
      w = ScreenW;
    if (h == CW_USEDEFAULT)
      h = ScreenH;
    MainWindow.rect = RECT{x, y, x + w, y + h};
    MainWindow.visible = (style & WS_VISIBLE) != 0;
    MainWindowExists = true;
    Focus = MainHandle;
  }
  CREATESTRUCT cs{};
  cs.lpCreateParams = param;
  cs.hInstance = inst;
  cs.cx = w;
  cs.cy = h;
  cs.x = x;
  cs.y = y;
  cs.style = (LONG)style;
  cs.lpszName = name;
  cs.lpszClass = cls;
  cs.dwExStyle = exstyle;
  if (proc && proc(MainHandle, WM_CREATE, 0, (LPARAM)&cs) == -1) {
    MainWindowExists = false;
    return nullptr;
  }
  if (style & WS_VISIBLE)
    Activate_Main_Window(MainHandle);
  return MainHandle;
}

extern "C" BOOL DestroyWindow(HWND w) {
  Window *win = Get(w);
  if (!win)
    return FALSE;
  if (win->proc)
    win->proc(w, WM_DESTROY, 0, 0);
  std::lock_guard<std::recursive_mutex> g(Lock);
  MainWindowExists = false;
  return TRUE;
}

// Windows activates a top-level window when it is first shown and sends the
// activation messages synchronously; the game only renders once it has seen
// WM_ACTIVATEAPP (GameInFocus). The page later reports real focus changes.
static bool Activated = false;
static void Activate_Main_Window(HWND w) {
  Window *win = Get(w);
  if (!win || !win->proc || Activated)
    return;
  Activated = true;
  win->proc(w, WM_ACTIVATEAPP, TRUE, 0);
  win->proc(w, WM_ACTIVATE, WA_ACTIVE, 0);
  win->proc(w, WM_SETFOCUS, 0, 0);
}

extern "C" BOOL ShowWindow(HWND w, int cmd) {
  Window *win = Get(w);
  if (!win)
    return FALSE;
  BOOL was = win->visible;
  win->visible = cmd != SW_HIDE && cmd != SW_MINIMIZE;
  if (win->visible)
    Activate_Main_Window(w);
  return was;
}

extern "C" BOOL UpdateWindow(HWND w) { return Get(w) != nullptr; }
extern "C" HWND SetFocus(HWND w) {
  HWND prev = Focus;
  Focus = w;
  return prev;
}
extern "C" HWND GetFocus(void) { return Focus; }
extern "C" HWND GetActiveWindow(void) { return Rebel_Main_Window(); }
extern "C" HWND GetForegroundWindow(void) { return Rebel_Main_Window(); }
extern "C" BOOL SetForegroundWindow(HWND w) {
  if (!Get(w))
    return FALSE;
  Activate_Main_Window(w);
  return TRUE;
}
extern "C" HWND SetActiveWindow(HWND w) { return w; }
extern "C" BOOL IsWindow(HWND w) { return Get(w) != nullptr; }
extern "C" BOOL IsWindowVisible(HWND w) {
  Window *win = Get(w);
  return win && win->visible;
}
extern "C" BOOL IsIconic(HWND) { return FALSE; }

extern "C" BOOL GetClientRect(HWND w, LPRECT r) {
  Window *win = Get(w);
  if (!win)
    return FALSE;
  *r = RECT{0, 0, win->rect.right - win->rect.left, win->rect.bottom - win->rect.top};
  return TRUE;
}

extern "C" BOOL GetWindowRect(HWND w, LPRECT r) {
  Window *win = Get(w);
  if (!win)
    return FALSE;
  *r = win->rect;
  return TRUE;
}

// The canvas has no frame: client area == window area.
extern "C" BOOL AdjustWindowRect(LPRECT, DWORD, BOOL) { return TRUE; }
extern "C" BOOL AdjustWindowRectEx(LPRECT, DWORD, BOOL, DWORD) { return TRUE; }

extern "C" BOOL SetWindowPos(HWND w, HWND, int x, int y, int cx, int cy, UINT flags) {
  Window *win = Get(w);
  if (!win)
    return FALSE;
  if (!(flags & SWP_NOMOVE)) {
    int ww = win->rect.right - win->rect.left, hh = win->rect.bottom - win->rect.top;
    win->rect = RECT{x, y, x + ww, y + hh};
  }
  if (!(flags & SWP_NOSIZE)) {
    win->rect.right = win->rect.left + cx;
    win->rect.bottom = win->rect.top + cy;
  }
  if (flags & SWP_SHOWWINDOW)
    win->visible = true;
  return TRUE;
}

extern "C" BOOL MoveWindow(HWND w, int x, int y, int cx, int cy, BOOL) {
  return SetWindowPos(w, nullptr, x, y, cx, cy, SWP_NOZORDER);
}

extern "C" BOOL SetWindowTextA(HWND w, LPCSTR text) {
  Window *win = Get(w);
  if (!win)
    return FALSE;
  win->title = text ? text : "";
  return TRUE;
}

extern "C" LONG GetWindowLongA(HWND w, int idx) {
  Window *win = Get(w);
  if (!win)
    return 0;
  switch (idx) {
  case GWL_STYLE: return (LONG)win->style;
  case GWL_EXSTYLE: return (LONG)win->exstyle;
  case GWL_USERDATA: return win->user_data;
  case GWL_WNDPROC: return (LONG)(LONG_PTR)win->proc;
  case GWL_HINSTANCE: return (LONG)(LONG_PTR)win->inst;
  default: return 0;
  }
}

extern "C" LONG SetWindowLongA(HWND w, int idx, LONG v) {
  Window *win = Get(w);
  if (!win)
    return 0;
  LONG prev = GetWindowLongA(w, idx);
  switch (idx) {
  case GWL_STYLE: win->style = (DWORD)v; break;
  case GWL_EXSTYLE: win->exstyle = (DWORD)v; break;
  case GWL_USERDATA: win->user_data = v; break;
  case GWL_WNDPROC: win->proc = (WNDPROC)(LONG_PTR)v; break;
  default: break;
  }
  return prev;
}

extern "C" HWND FindWindowA(LPCSTR, LPCSTR) { return nullptr; } // no other instance
extern "C" BOOL ClientToScreen(HWND, LPPOINT) { return TRUE; }
extern "C" BOOL ScreenToClient(HWND, LPPOINT) { return TRUE; }

extern "C" int GetSystemMetrics(int idx) {
  switch (idx) {
  case SM_CXSCREEN:
  case SM_CXFULLSCREEN: return ScreenW;
  case SM_CYSCREEN:
  case SM_CYFULLSCREEN: return ScreenH;
  case SM_CXDOUBLECLK:
  case SM_CYDOUBLECLK: return 4;
  default: return 0;
  }
}

/* ---------------------------------------------------------------- messages */

extern "C" LRESULT DefWindowProcA(HWND w, UINT msg, WPARAM, LPARAM) {
  if (msg == WM_CLOSE)
    DestroyWindow(w);
  return 0;
}

extern "C" LRESULT CallWindowProcA(WNDPROC proc, HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  return proc ? proc(w, msg, wp, lp) : 0;
}

extern "C" void Rebel_Yield(DWORD ms);
extern "C" void Rebel_Audio_Update(void);

// The game pumps messages once per frame; when the queue is empty, hand the
// browser a turn (present the frame, deliver input) at most every ~4 ms.
static void Maybe_Yield_To_Browser() {
  static DWORD last = 0;
  DWORD now = GetTickCount();
  if (now - last >= 4) {
    Rebel_Audio_Update(); // refill streamed music before yielding
    Rebel_Yield(0);
    last = GetTickCount();
  }
}

extern "C" BOOL PeekMessageA(LPMSG msg, HWND w, UINT min, UINT max, UINT remove) {
  bool empty;
  {
    std::lock_guard<std::recursive_mutex> g(Lock);
    empty = Queue.empty() && !QuitPosted;
  }
  if (empty)
    Maybe_Yield_To_Browser(); // events arriving during the yield land in Queue
  std::lock_guard<std::recursive_mutex> g(Lock);
  if (QuitPosted && Queue.empty()) {
    memset(msg, 0, sizeof(*msg));
    msg->message = WM_QUIT;
    if (remove & PM_REMOVE)
      QuitPosted = false;
    return TRUE;
  }
  for (auto it = Queue.begin(); it != Queue.end(); ++it) {
    if (min || max) {
      if (it->message < min || it->message > max)
        continue;
    }
    *msg = *it;
    if (remove & PM_REMOVE)
      Queue.erase(it);
    return TRUE;
  }
  return FALSE;
}

extern "C" BOOL GetMessageA(LPMSG msg, HWND w, UINT min, UINT max) {
  (void)w;
  // Non-blocking on the web: the caller's loop runs once per frame.
  if (!PeekMessageA(msg, w, min, max, PM_REMOVE)) {
    memset(msg, 0, sizeof(*msg));
    msg->message = WM_NULL;
    return TRUE;
  }
  return msg->message != WM_QUIT;
}

extern "C" BOOL TranslateMessage(const MSG *msg) {
  // WM_CHAR is posted by the web layer directly from KeyboardEvent.key.
  (void)msg;
  return FALSE;
}

extern "C" LRESULT DispatchMessageA(const MSG *msg) {
  Window *win = Get(msg->hwnd);
  if (!win || !win->proc)
    return 0;
  return win->proc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
}

extern "C" BOOL PostMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  MSG m{};
  m.hwnd = w ? w : MainHandle;
  m.message = msg;
  m.wParam = wp;
  m.lParam = lp;
  m.time = GetTickCount();
  Queue.push_back(m);
  return TRUE;
}

extern "C" LRESULT SendMessageA(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  Window *win = Get(w);
  return win && win->proc ? win->proc(w, msg, wp, lp) : 0;
}

extern "C" void PostQuitMessage(int) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  QuitPosted = true;
}

extern "C" BOOL IsDialogMessageA(HWND, LPMSG) { return FALSE; }
extern "C" int TranslateAcceleratorA(HWND, HACCEL, LPMSG) { return 0; }
extern "C" UINT_PTR SetTimer(HWND, UINT_PTR id, UINT, void *) { return id ? id : 1; }
extern "C" BOOL KillTimer(HWND, UINT_PTR) { return TRUE; }

/* ------------------------------------------------------------ cursor/input */

extern "C" int ShowCursor(BOOL show) { return show ? ++CursorCount : --CursorCount; }
extern "C" HCURSOR SetCursor(HCURSOR c) { return c; }
extern "C" HCURSOR LoadCursorA(HINSTANCE, LPCSTR) { return (HCURSOR)1; }
extern "C" HICON LoadIconA(HINSTANCE, LPCSTR) { return (HICON)1; }
extern "C" BOOL GetCursorPos(LPPOINT p) {
  *p = Cursor;
  return TRUE;
}
extern "C" BOOL SetCursorPos(int x, int y) {
  Cursor = POINT{x, y}; // pointer lock makes the real cursor irrelevant
  return TRUE;
}
extern "C" BOOL ClipCursor(const RECT *) { return TRUE; }
extern "C" HWND SetCapture(HWND w) { return w; }
extern "C" BOOL ReleaseCapture(void) { return TRUE; }

extern "C" SHORT GetAsyncKeyState(int vk) { return (KeyState[vk & 0xFF] & 0x80) ? (SHORT)0x8000 : 0; }
extern "C" SHORT GetKeyState(int vk) { return (KeyState[vk & 0xFF] & 0x80) ? (SHORT)0xFF80 : 0; }
extern "C" BOOL GetKeyboardState(PBYTE state) {
  memcpy(state, KeyState, 256);
  return TRUE;
}

// US layout scan codes for the keys the game binds by scan code.
extern "C" UINT MapVirtualKeyA(UINT code, UINT type) {
  static const struct { BYTE vk, sc; } map[] = {
      {VK_ESCAPE, 0x01}, {'1', 0x02}, {'2', 0x03}, {'3', 0x04}, {'4', 0x05}, {'5', 0x06}, {'6', 0x07},
      {'7', 0x08}, {'8', 0x09}, {'9', 0x0A}, {'0', 0x0B}, {VK_BACK, 0x0E}, {VK_TAB, 0x0F}, {'Q', 0x10},
      {'W', 0x11}, {'E', 0x12}, {'R', 0x13}, {'T', 0x14}, {'Y', 0x15}, {'U', 0x16}, {'I', 0x17},
      {'O', 0x18}, {'P', 0x19}, {VK_RETURN, 0x1C}, {VK_CONTROL, 0x1D}, {'A', 0x1E}, {'S', 0x1F},
      {'D', 0x20}, {'F', 0x21}, {'G', 0x22}, {'H', 0x23}, {'J', 0x24}, {'K', 0x25}, {'L', 0x26},
      {VK_SHIFT, 0x2A}, {'Z', 0x2C}, {'X', 0x2D}, {'C', 0x2E}, {'V', 0x2F}, {'B', 0x30}, {'N', 0x31},
      {'M', 0x32}, {VK_MENU, 0x38}, {VK_SPACE, 0x39}, {VK_CAPITAL, 0x3A}, {VK_F1, 0x3B}, {VK_F2, 0x3C},
      {VK_F3, 0x3D}, {VK_F4, 0x3E}, {VK_F5, 0x3F}, {VK_F6, 0x40}, {VK_F7, 0x41}, {VK_F8, 0x42},
      {VK_F9, 0x43}, {VK_F10, 0x44}, {VK_F11, 0x57}, {VK_F12, 0x58}, {VK_HOME, 0x47}, {VK_UP, 0x48},
      {VK_PRIOR, 0x49}, {VK_LEFT, 0x4B}, {VK_RIGHT, 0x4D}, {VK_END, 0x4F}, {VK_DOWN, 0x50},
      {VK_NEXT, 0x51}, {VK_INSERT, 0x52}, {VK_DELETE, 0x53},
  };
  for (auto &e : map) {
    if (type == 0 && e.vk == code)
      return e.sc; // MAPVK_VK_TO_VSC
    if (type == 1 && e.sc == code)
      return e.vk; // MAPVK_VSC_TO_VK
  }
  if (type == 2) // MAPVK_VK_TO_CHAR
    return (code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9') || code == ' ' ? code : 0;
  return 0;
}

extern "C" int ToAscii(UINT vk, UINT, const BYTE *state, LPWORD out, UINT) {
  bool shift = state && (state[VK_SHIFT] & 0x80);
  if (vk >= 'A' && vk <= 'Z') {
    *out = (WORD)(shift ? vk : vk + 32);
    return 1;
  }
  if ((vk >= '0' && vk <= '9') || vk == ' ') {
    *out = (WORD)vk;
    return 1;
  }
  return 0;
}

extern "C" HKL GetKeyboardLayout(DWORD) { return (HKL)0x04090409; }
extern "C" UINT GetDoubleClickTime(void) { return 500; }
extern "C" BOOL SystemParametersInfoA(UINT, UINT, PVOID, UINT) { return FALSE; }

/* -------------------------------------------------------------- GDI (inert) */

extern "C" BOOL InvalidateRect(HWND, const RECT *, BOOL) { return TRUE; }
extern "C" BOOL ValidateRect(HWND, const RECT *) { return TRUE; }
extern "C" HDC BeginPaint(HWND, LPPAINTSTRUCT ps) {
  memset(ps, 0, sizeof(*ps));
  return (HDC)1;
}
extern "C" BOOL EndPaint(HWND, const PAINTSTRUCT *) { return TRUE; }
extern "C" HGDIOBJ GetStockObject(int) { return (HGDIOBJ)1; }
extern "C" int Rebel_Screen_Width(void) { return ScreenW; }
extern "C" int Rebel_Screen_Height(void) { return ScreenH; }
// GetDC/SelectObject/DeleteObject/GetDeviceCaps live in gfx/gdi_text.cpp.
extern "C" BOOL MessageBeep(UINT) { return TRUE; }
extern "C" BOOL FlashWindow(HWND, BOOL) { return TRUE; }

/* ---------------------------------------------------------- dialogs (none) */

extern "C" BOOL EnableWindow(HWND, BOOL) { return TRUE; }
extern "C" HWND GetDlgItem(HWND, int) { return nullptr; }
extern "C" BOOL EndDialog(HWND, INT_PTR) { return TRUE; }
extern "C" INT_PTR DialogBoxParamA(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM) {
  fprintf(stderr, "[win32] DialogBoxParam: Win32 dialogs are not available\n");
  return IDCANCEL;
}
extern "C" HWND CreateDialogParamA(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM) { return nullptr; }
extern "C" BOOL SetDlgItemTextA(HWND, int, LPCSTR) { return FALSE; }
extern "C" UINT GetDlgItemTextA(HWND, int, LPSTR text, int max) {
  if (text && max > 0)
    text[0] = 0;
  return 0;
}
extern "C" BOOL CheckDlgButton(HWND, int, UINT) { return FALSE; }
extern "C" UINT IsDlgButtonChecked(HWND, int) { return 0; }
extern "C" LRESULT SendDlgItemMessageA(HWND, int, UINT, WPARAM, LPARAM) { return 0; }

/* -------------------------------------------------------------------- rects */

extern "C" BOOL SetRect(LPRECT r, int l, int t, int ri, int b) {
  *r = RECT{l, t, ri, b};
  return TRUE;
}
extern "C" BOOL PtInRect(const RECT *r, POINT p) { return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom; }
extern "C" BOOL IntersectRect(LPRECT d, const RECT *a, const RECT *b) {
  d->left = a->left > b->left ? a->left : b->left;
  d->top = a->top > b->top ? a->top : b->top;
  d->right = a->right < b->right ? a->right : b->right;
  d->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
  if (d->left >= d->right || d->top >= d->bottom) {
    *d = RECT{0, 0, 0, 0};
    return FALSE;
  }
  return TRUE;
}
extern "C" BOOL OffsetRect(LPRECT r, int dx, int dy) {
  r->left += dx;
  r->right += dx;
  r->top += dy;
  r->bottom += dy;
  return TRUE;
}
extern "C" BOOL IsRectEmpty(const RECT *r) { return r->left >= r->right || r->top >= r->bottom; }

/* ------------------------------------------------------------- shell/misc */

extern "C" HINSTANCE ShellExecuteA(HWND, LPCSTR, LPCSTR file, LPCSTR, LPCSTR, INT) {
  fprintf(stderr, "[win32] ShellExecute(%s) ignored\n", file ? file : "");
  return (HINSTANCE)2; // <= 32 means failure
}
extern "C" BOOL OpenClipboard(HWND) { return FALSE; }
extern "C" BOOL CloseClipboard(void) { return TRUE; }
extern "C" HANDLE GetClipboardData(UINT) { return nullptr; }
extern "C" BOOL IsClipboardFormatAvailable(UINT) { return FALSE; }

extern "C" HIMC ImmGetContext(HWND) { return nullptr; }
extern "C" BOOL ImmReleaseContext(HWND, HIMC) { return TRUE; }
extern "C" HIMC ImmAssociateContext(HWND, HIMC) { return nullptr; }

extern "C" HMENU GetMenu(HWND) { return nullptr; } // the game window has no menu bar
extern "C" BOOL SetMenu(HWND, HMENU) { return TRUE; }
