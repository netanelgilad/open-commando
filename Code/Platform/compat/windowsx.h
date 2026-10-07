#pragma once
#include "windows.h"
/* windowsx.h memory helpers */
#define GlobalAllocPtr(flags, cb) GlobalLock(GlobalAlloc((flags), (cb)))
#define GlobalFreePtr(lp) (GlobalUnlock(lp), (BOOL)(ULONG_PTR)GlobalFree(lp))
