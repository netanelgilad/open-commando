#pragma once
#include "windows.h"
#ifdef __cplusplus
extern "C" {
#endif
uintptr_t _beginthread(void (*start)(void *), unsigned stack, void *arg);
uintptr_t _beginthreadex(void *security, unsigned stack, unsigned (*start)(void *), void *arg, unsigned flags,
                         unsigned *thread_id);
void _endthread(void);
void _endthreadex(unsigned code);
#ifdef __cplusplus
}
#endif
