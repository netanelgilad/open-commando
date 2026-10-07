#pragma once
// Common base for every kernel object handed out as a Win32 HANDLE.
#include "windows.h"

struct RebelHandle {
  enum class Kind { File, Find, Event, Mutex, Semaphore, Thread, RegistryKey };
  static constexpr unsigned MAGIC = 0x52424C48; // "RBLH"

  unsigned magic = MAGIC;
  Kind kind;
  int refs = 1;

  explicit RebelHandle(Kind k) : kind(k) {}
  virtual ~RebelHandle();
  virtual void Close() {}
};

// Validates and returns the object behind a HANDLE, or nullptr.
RebelHandle *Rebel_Handle(HANDLE h);
void Rebel_Handle_Release(RebelHandle *h);

DWORD Rebel_Errno_To_Win32(int e);
void Rebel_Unix_To_FileTime(long long unix_seconds, long nanos, LPFILETIME ft);
