/*
** DirectInput 8 on browser events.
**
** The page forwards keyboard (KeyboardEvent.code -> scan code), mouse button
** and Pointer Lock movement events through the Rebel_Input_* entry points
** below. Each acquired device keeps immediate state plus a FIFO of
** DIDEVICEOBJECTDATA records for GetDeviceData, matching how
** Combat/directinput.cpp polls once per frame. No joysticks are enumerated
** yet (gamepad support is tracked in docs/WEB_PORT_GAPS.md).
*/
#include "windows.h"
#include "dinput.h"

#include <cstring>
#include <deque>
#include <mutex>

extern "C" {
const DIDATAFORMAT c_dfDIKeyboard = {sizeof(DIDATAFORMAT), 0, 0, 256, 0, nullptr};
const DIDATAFORMAT c_dfDIMouse = {sizeof(DIDATAFORMAT), 0, 0, sizeof(DIMOUSESTATE), 0, nullptr};
const DIDATAFORMAT c_dfDIMouse2 = {sizeof(DIDATAFORMAT), 0, 0, sizeof(DIMOUSESTATE2), 0, nullptr};
const DIDATAFORMAT c_dfDIJoystick = {sizeof(DIDATAFORMAT), 0, 0, sizeof(DIJOYSTATE), 0, nullptr};
const GUID GUID_SysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID GUID_SysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID IID_IDirectInput8 = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
const GUID IID_IDirectInputDevice2 = {0x5944E682, 0xC92E, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID IID_IDirectInputDevice8 = {0x54D41080, 0xDC15, 0x4833, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};
}

namespace {

std::recursive_mutex Lock;
DWORD Sequence = 0;

enum class DeviceKind { Keyboard, Mouse };

struct Device;
Device *Keyboard = nullptr;
Device *Mouse = nullptr;

struct Device : public IDirectInputDevice8 {
  DeviceKind kind;
  ULONG refs = 1;
  bool acquired = false;
  DWORD buffer_size = 0;
  std::deque<DIDEVICEOBJECTDATA> queue;
  BYTE keys[256] = {};
  DIMOUSESTATE2 mouse = {};

  explicit Device(DeviceKind k) : kind(k) {}

  HRESULT QueryInterface(REFIID, void **out) override {
    *out = this;
    AddRef();
    return S_OK;
  }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override {
    ULONG r = --refs;
    if (r == 0) {
      std::lock_guard<std::recursive_mutex> g(Lock);
      if (Keyboard == this)
        Keyboard = nullptr;
      if (Mouse == this)
        Mouse = nullptr;
      delete this;
    }
    return r;
  }

  HRESULT SetDataFormat(LPCDIDATAFORMAT) override { return DI_OK; }
  HRESULT SetCooperativeLevel(HWND, DWORD) override { return DI_OK; }
  HRESULT SetProperty(REFGUID prop, LPCDIPROPHEADER header) override {
    if (&prop == &DIPROP_BUFFERSIZE && header)
      buffer_size = reinterpret_cast<const DIPROPDWORD *>(header)->dwData;
    return DI_OK;
  }
  HRESULT Acquire() override {
    std::lock_guard<std::recursive_mutex> g(Lock);
    acquired = true;
    return DI_OK;
  }
  HRESULT Unacquire() override {
    std::lock_guard<std::recursive_mutex> g(Lock);
    acquired = false;
    queue.clear();
    return DI_OK;
  }
  HRESULT GetDeviceState(DWORD size, LPVOID data) override {
    std::lock_guard<std::recursive_mutex> g(Lock);
    if (!acquired)
      return DIERR_NOTACQUIRED;
    if (kind == DeviceKind::Keyboard) {
      memcpy(data, keys, size < 256 ? size : 256);
    } else {
      memcpy(data, &mouse, size < sizeof(mouse) ? size : sizeof(mouse));
      // Immediate mouse axes are relative since the last read.
      mouse.lX = mouse.lY = mouse.lZ = 0;
    }
    return DI_OK;
  }
  HRESULT GetDeviceData(DWORD obj_size, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) override {
    std::lock_guard<std::recursive_mutex> g(Lock);
    if (!acquired) {
      *count = 0;
      return DIERR_NOTACQUIRED;
    }
    const bool peek = (flags & 1) != 0; // DIGDD_PEEK
    DWORD n = 0;
    auto it = queue.begin();
    while (n < *count && it != queue.end()) {
      if (data)
        memcpy((char *)data + n * obj_size, &*it, obj_size < sizeof(*it) ? obj_size : sizeof(*it));
      ++it;
      ++n;
    }
    if (!peek)
      queue.erase(queue.begin(), queue.begin() + n);
    *count = n;
    return DI_OK;
  }
  HRESULT Poll() override { return DI_OK; }

  void Push(DWORD ofs, DWORD value) {
    if (!acquired)
      return;
    DIDEVICEOBJECTDATA d = {};
    d.dwOfs = ofs;
    d.dwData = value;
    d.dwTimeStamp = GetTickCount();
    d.dwSequence = ++Sequence;
    queue.push_back(d);
    DWORD cap = buffer_size ? buffer_size : 64;
    while (queue.size() > cap)
      queue.pop_front();
  }
};

struct DirectInputObject : public IDirectInput8 {
  ULONG refs = 1;
  HRESULT QueryInterface(REFIID, void **out) override {
    *out = this;
    AddRef();
    return S_OK;
  }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override {
    ULONG r = --refs;
    if (r == 0)
      delete this;
    return r;
  }
  HRESULT CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8 *device, LPUNKNOWN) override {
    std::lock_guard<std::recursive_mutex> g(Lock);
    if (guid == GUID_SysKeyboard) {
      Keyboard = new Device(DeviceKind::Keyboard);
      *device = Keyboard;
      return DI_OK;
    }
    if (guid == GUID_SysMouse) {
      Mouse = new Device(DeviceKind::Mouse);
      *device = Mouse;
      return DI_OK;
    }
    *device = nullptr;
    return DIERR_INVALIDPARAM;
  }
  HRESULT EnumDevices(DWORD, LPDIENUMDEVICESCALLBACK, LPVOID, DWORD) override { return DI_OK; }
};

} // namespace

extern "C" HRESULT DirectInput8Create(HINSTANCE, DWORD, REFIID, LPVOID *out, LPUNKNOWN) {
  *out = new DirectInputObject();
  return DI_OK;
}

/* ------------------------------------------------- web runtime entry points */

extern "C" void Rebel_Input_Key(int scan_code, int down) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  if (!Keyboard || scan_code <= 0 || scan_code > 255)
    return;
  BYTE v = down ? 0x80 : 0x00;
  if (Keyboard->keys[scan_code] == v)
    return; // ignore key repeat: DirectInput reports transitions only
  Keyboard->keys[scan_code] = v;
  Keyboard->Push((DWORD)scan_code, v);
}

extern "C" void Rebel_Input_Mouse_Move(int dx, int dy) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  if (!Mouse)
    return;
  Mouse->mouse.lX += dx;
  Mouse->mouse.lY += dy;
  if (dx)
    Mouse->Push(DIMOFS_X, (DWORD)dx);
  if (dy)
    Mouse->Push(DIMOFS_Y, (DWORD)dy);
}

extern "C" void Rebel_Input_Mouse_Wheel(int dz) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  if (!Mouse || !dz)
    return;
  Mouse->mouse.lZ += dz;
  Mouse->Push(DIMOFS_Z, (DWORD)dz);
}

extern "C" void Rebel_Input_Mouse_Button(int button, int down) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  if (!Mouse || button < 0 || button > 7)
    return;
  BYTE v = down ? 0x80 : 0x00;
  Mouse->mouse.rgbButtons[button] = v;
  Mouse->Push((DWORD)(DIMOFS_BUTTON0 + button), v);
}

// Called when the tab loses focus or pointer lock is released.
extern "C" void Rebel_Input_Release_All(void) {
  std::lock_guard<std::recursive_mutex> g(Lock);
  if (Keyboard)
    for (int i = 1; i < 256; i++)
      if (Keyboard->keys[i])
        Rebel_Input_Key(i, 0);
  if (Mouse)
    for (int b = 0; b < 8; b++)
      if (Mouse->mouse.rgbButtons[b])
        Rebel_Input_Mouse_Button(b, 0);
}
