// Portable replacements for wwlib/Except.cpp (Windows SEH crash handler).
#include "Except.h"
#include <cstring>

static unsigned long MainThreadID = 0;
static bool ExitOnException = false;
static bool TryingToExit = false;
static void (*AppExceptionCallback)() = nullptr;
static const char *(*AppVersionCallback)() = nullptr;

int Stack_Walk(unsigned long *, int, void *) { return 0; }
bool Lookup_Symbol(void *, char *symbol, int &displacement) {
  if (symbol) symbol[0] = 0;
  displacement = 0;
  return false;
}
void Load_Image_Helper() {}
void Register_Thread_ID(unsigned long thread_id, const char *, bool main) {
  if (main) MainThreadID = thread_id;
}
void Unregister_Thread_ID(unsigned long, const char *) {}
void Register_Application_Exception_Callback(void (*cb)()) { AppExceptionCallback = cb; }
void Register_Application_Version_Callback(const char *(*cb)()) { AppVersionCallback = cb; }
void Set_Exit_On_Exception(bool set) { ExitOnException = set; }
bool Is_Trying_To_Exit() { return TryingToExit; }
unsigned long Get_Main_Thread_ID() { return MainThreadID; }
