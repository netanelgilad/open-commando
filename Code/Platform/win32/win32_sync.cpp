/*
** Win32 threads and synchronization on pthreads.
**
** Kernel objects (threads, events, mutexes, semaphores) share one handle
** type so WaitForSingleObject/CloseHandle work on any of them. File handles
** are a separate kind (see win32_file.cpp) and also go through CloseHandle.
*/
#include "windows.h"
#include "win32_handles.h"

#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <pthread.h>
#include <sched.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#endif

namespace {

struct SyncObject : RebelHandle {
  pthread_mutex_t lock;
  pthread_cond_t cond;
  // event
  bool manual_reset = false;
  bool signaled = false;
  // mutex
  pthread_t owner{};
  bool owned = false;
  int recursion = 0;
  // semaphore
  LONG count = 0;
  LONG max_count = 0;
  // thread
  pthread_t thread{};
  bool thread_valid = false;
  bool finished = false;
  DWORD exit_code = STILL_ACTIVE;
  LPTHREAD_START_ROUTINE start = nullptr;
  LPVOID arg = nullptr;
  bool suspended = false;

  explicit SyncObject(Kind k) : RebelHandle(k) {
    pthread_mutex_init(&lock, nullptr);
    pthread_cond_init(&cond, nullptr);
  }
  ~SyncObject() override {
    pthread_cond_destroy(&cond);
    pthread_mutex_destroy(&lock);
  }

  // Must hold lock. Returns true if the object is signaled for the caller
  // and consumes the signal where Win32 semantics say so.
  bool Try_Acquire() {
    switch (kind) {
    case Kind::Event:
      if (!signaled)
        return false;
      if (!manual_reset)
        signaled = false;
      return true;
    case Kind::Mutex:
      if (owned && !pthread_equal(owner, pthread_self()))
        return false;
      owned = true;
      owner = pthread_self();
      recursion++;
      return true;
    case Kind::Semaphore:
      if (count <= 0)
        return false;
      count--;
      return true;
    case Kind::Thread:
      return finished;
    default:
      return true;
    }
  }
};

SyncObject *As_Sync(HANDLE h) {
  RebelHandle *rh = Rebel_Handle(h);
  if (!rh)
    return nullptr;
  switch (rh->kind) {
  case RebelHandle::Kind::Event:
  case RebelHandle::Kind::Mutex:
  case RebelHandle::Kind::Semaphore:
  case RebelHandle::Kind::Thread:
    return static_cast<SyncObject *>(rh);
  default:
    return nullptr;
  }
}

void Deadline_After(DWORD ms, timespec &ts) {
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
}

thread_local DWORD ThisThreadId = 0;
LONG NextThreadId = 1;

void *Thread_Trampoline(void *p) {
  SyncObject *t = static_cast<SyncObject *>(p);
  pthread_mutex_lock(&t->lock);
  while (t->suspended)
    pthread_cond_wait(&t->cond, &t->lock);
  pthread_mutex_unlock(&t->lock);
  DWORD code = t->start(t->arg);
  pthread_mutex_lock(&t->lock);
  t->exit_code = code;
  t->finished = true;
  pthread_cond_broadcast(&t->cond);
  pthread_mutex_unlock(&t->lock);
  Rebel_Handle_Release(t); // the running thread's reference
  return nullptr;
}

} // namespace

/* ------------------------------------------------------------------ handles */

RebelHandle::~RebelHandle() {}

RebelHandle *Rebel_Handle(HANDLE h) {
  if (!h || h == INVALID_HANDLE_VALUE)
    return nullptr;
  RebelHandle *rh = static_cast<RebelHandle *>(h);
  return rh->magic == RebelHandle::MAGIC ? rh : nullptr;
}

void Rebel_Handle_Release(RebelHandle *h) {
  if (h && __atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL) == 0) {
    h->magic = 0;
    delete h;
  }
}

extern "C" BOOL CloseHandle(HANDLE h) {
  RebelHandle *rh = Rebel_Handle(h);
  if (!rh) {
    // pseudo handles (GetCurrentProcess/Thread) and NULL
    SetLastError(ERROR_INVALID_HANDLE);
    return h == (HANDLE)(LONG_PTR)-1 || h == (HANDLE)(LONG_PTR)-2;
  }
  rh->Close();
  Rebel_Handle_Release(rh);
  return TRUE;
}

/* ------------------------------------------------------------------ threads */

extern "C" HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg,
                               DWORD flags, LPDWORD id) {
  SyncObject *t = new SyncObject(RebelHandle::Kind::Thread);
  t->start = fn;
  t->arg = arg;
  t->suspended = (flags & CREATE_SUSPENDED) != 0;
  t->refs = 2; // caller's handle + running thread
  DWORD tid = (DWORD)__atomic_add_fetch(&NextThreadId, 1, __ATOMIC_RELAXED);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  if (stack)
    pthread_attr_setstacksize(&attr, stack < 65536 ? 65536 : stack);
  int rc = pthread_create(&t->thread, &attr, Thread_Trampoline, t);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    fprintf(stderr, "[win32] CreateThread failed (%d): build without pthreads?\n", rc);
    t->refs = 1;
    Rebel_Handle_Release(t);
    SetLastError(8); /* ERROR_NOT_ENOUGH_MEMORY */
    return nullptr;
  }
  pthread_detach(t->thread);
  t->thread_valid = true;
  if (id)
    *id = tid;
  return t;
}

extern "C" DWORD ResumeThread(HANDLE h) {
  SyncObject *t = As_Sync(h);
  if (!t)
    return (DWORD)-1;
  pthread_mutex_lock(&t->lock);
  DWORD prev = t->suspended ? 1 : 0;
  t->suspended = false;
  pthread_cond_broadcast(&t->cond);
  pthread_mutex_unlock(&t->lock);
  return prev;
}

// Suspending a running pthread is not possible; only threads created
// suspended honour it. The engine only suspends threads it is about to stop.
extern "C" DWORD SuspendThread(HANDLE) { return 0; }

extern "C" BOOL TerminateThread(HANDLE h, DWORD code) {
  SyncObject *t = As_Sync(h);
  if (!t)
    return FALSE;
  fprintf(stderr, "[win32] TerminateThread is not supported; thread keeps running\n");
  (void)code;
  return FALSE;
}

extern "C" BOOL GetExitCodeThread(HANDLE h, LPDWORD code) {
  SyncObject *t = As_Sync(h);
  if (!t)
    return FALSE;
  pthread_mutex_lock(&t->lock);
  *code = t->finished ? t->exit_code : STILL_ACTIVE;
  pthread_mutex_unlock(&t->lock);
  return TRUE;
}

extern "C" BOOL SetThreadPriority(HANDLE, int) { return TRUE; }
extern "C" int GetThreadPriority(HANDLE) { return THREAD_PRIORITY_NORMAL; }
extern "C" void ExitThread(DWORD code) {
#ifdef __EMSCRIPTEN__
  // Ending the browser main thread would end the program (the game asks for
  // this on asserts via cDevOptions::ExitThreadOnAssert): ignore it there.
  if (emscripten_is_main_browser_thread()) {
    fprintf(stderr, "[win32] ExitThread(%lu) on the main thread ignored\n", (unsigned long)code);
    return;
  }
#endif
  pthread_exit((void *)(ULONG_PTR)code);
}

extern "C" DWORD GetCurrentThreadId(void) {
#ifdef __EMSCRIPTEN__
  return (DWORD)(ULONG_PTR)pthread_self();
#else
  if (!ThisThreadId)
    ThisThreadId = (DWORD)__atomic_add_fetch(&NextThreadId, 1, __ATOMIC_RELAXED);
  return ThisThreadId;
#endif
}

/* ------------------------------------------------------- events / mutexes */

extern "C" HANDLE CreateEventA(LPSECURITY_ATTRIBUTES, BOOL manual, BOOL initial, LPCSTR) {
  SyncObject *e = new SyncObject(RebelHandle::Kind::Event);
  e->manual_reset = manual != 0;
  e->signaled = initial != 0;
  return e;
}

extern "C" HANDLE OpenEventA(DWORD, BOOL, LPCSTR) {
  SetLastError(ERROR_FILE_NOT_FOUND); // no cross-process named objects
  return nullptr;
}

extern "C" BOOL SetEvent(HANDLE h) {
  SyncObject *e = As_Sync(h);
  if (!e)
    return FALSE;
  pthread_mutex_lock(&e->lock);
  e->signaled = true;
  pthread_cond_broadcast(&e->cond);
  pthread_mutex_unlock(&e->lock);
  return TRUE;
}

extern "C" BOOL ResetEvent(HANDLE h) {
  SyncObject *e = As_Sync(h);
  if (!e)
    return FALSE;
  pthread_mutex_lock(&e->lock);
  e->signaled = false;
  pthread_mutex_unlock(&e->lock);
  return TRUE;
}

extern "C" HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL owner, LPCSTR name) {
  // Named mutexes are used as "is another copy running" checks; in a browser
  // tab there never is one, so report a fresh object.
  (void)name;
  SyncObject *m = new SyncObject(RebelHandle::Kind::Mutex);
  if (owner) {
    m->owned = true;
    m->owner = pthread_self();
    m->recursion = 1;
  }
  SetLastError(ERROR_SUCCESS);
  return m;
}

extern "C" BOOL ReleaseMutex(HANDLE h) {
  SyncObject *m = As_Sync(h);
  if (!m)
    return FALSE;
  pthread_mutex_lock(&m->lock);
  BOOL ok = FALSE;
  if (m->owned && pthread_equal(m->owner, pthread_self())) {
    if (--m->recursion == 0) {
      m->owned = false;
      pthread_cond_broadcast(&m->cond);
    }
    ok = TRUE;
  }
  pthread_mutex_unlock(&m->lock);
  return ok;
}

extern "C" HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES, LONG initial, LONG max, LPCSTR) {
  SyncObject *s = new SyncObject(RebelHandle::Kind::Semaphore);
  s->count = initial;
  s->max_count = max;
  return s;
}

extern "C" BOOL ReleaseSemaphore(HANDLE h, LONG n, LPLONG prev) {
  SyncObject *s = As_Sync(h);
  if (!s)
    return FALSE;
  pthread_mutex_lock(&s->lock);
  if (prev)
    *prev = s->count;
  BOOL ok = s->count + n <= s->max_count;
  if (ok) {
    s->count += n;
    pthread_cond_broadcast(&s->cond);
  }
  pthread_mutex_unlock(&s->lock);
  return ok;
}

extern "C" DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
  SyncObject *o = As_Sync(h);
  if (!o) {
    SetLastError(ERROR_INVALID_HANDLE);
    return WAIT_FAILED;
  }
#ifdef __EMSCRIPTEN__
  // The browser main thread may not block. Poll once and report timeout so
  // callers fall back to their non-blocking paths.
  if (emscripten_is_main_browser_thread() && ms != 0) {
    pthread_mutex_lock(&o->lock);
    bool got = o->Try_Acquire();
    pthread_mutex_unlock(&o->lock);
    return got ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
  }
#endif
  pthread_mutex_lock(&o->lock);
  DWORD result = WAIT_OBJECT_0;
  if (ms == INFINITE) {
    while (!o->Try_Acquire())
      pthread_cond_wait(&o->cond, &o->lock);
  } else {
    timespec deadline;
    Deadline_After(ms, deadline);
    while (!o->Try_Acquire()) {
      if (pthread_cond_timedwait(&o->cond, &o->lock, &deadline) == ETIMEDOUT) {
        result = o->Try_Acquire() ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
        break;
      }
    }
  }
  pthread_mutex_unlock(&o->lock);
  return result;
}

extern "C" DWORD WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms) {
  // Polling implementation; only used with small handle sets.
  DWORD start = GetTickCount();
  for (;;) {
    if (all) {
      bool every = true;
      for (DWORD i = 0; i < n; i++)
        if (WaitForSingleObject(h[i], 0) != WAIT_OBJECT_0)
          every = false;
      if (every)
        return WAIT_OBJECT_0;
    } else {
      for (DWORD i = 0; i < n; i++)
        if (WaitForSingleObject(h[i], 0) == WAIT_OBJECT_0)
          return WAIT_OBJECT_0 + i;
    }
    if (ms != INFINITE && GetTickCount() - start >= ms)
      return WAIT_TIMEOUT;
#ifdef __EMSCRIPTEN__
    if (emscripten_is_main_browser_thread())
      return WAIT_TIMEOUT;
#endif
    sched_yield();
  }
}

/* -------------------------------------------------------- critical sections */

extern "C" void InitializeCriticalSection(LPCRITICAL_SECTION cs) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  // malloc, not new: the engine's global operator new (WWMemoryLog) takes a
  // critical section itself, so allocating one with new would recurse.
  pthread_mutex_t *m = static_cast<pthread_mutex_t *>(malloc(sizeof(pthread_mutex_t)));
  pthread_mutex_init(m, &attr);
  pthread_mutexattr_destroy(&attr);
  cs->impl = m;
  cs->LockCount = -1;
  cs->RecursionCount = 0;
}

extern "C" void DeleteCriticalSection(LPCRITICAL_SECTION cs) {
  pthread_mutex_t *m = static_cast<pthread_mutex_t *>(cs->impl);
  if (m) {
    pthread_mutex_destroy(m);
    free(m);
  }
  cs->impl = nullptr;
}

extern "C" void EnterCriticalSection(LPCRITICAL_SECTION cs) {
  if (!cs->impl)
    InitializeCriticalSection(cs);
  pthread_mutex_lock(static_cast<pthread_mutex_t *>(cs->impl));
  cs->RecursionCount++;
}

extern "C" BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs) {
  if (!cs->impl)
    InitializeCriticalSection(cs);
  if (pthread_mutex_trylock(static_cast<pthread_mutex_t *>(cs->impl)) != 0)
    return FALSE;
  cs->RecursionCount++;
  return TRUE;
}

extern "C" void LeaveCriticalSection(LPCRITICAL_SECTION cs) {
  cs->RecursionCount--;
  pthread_mutex_unlock(static_cast<pthread_mutex_t *>(cs->impl));
}

/* -------------------------------------------------------------- interlocked */

extern "C" LONG InterlockedIncrement(LONG volatile *v) { return __atomic_add_fetch(v, 1, __ATOMIC_SEQ_CST); }
extern "C" LONG InterlockedDecrement(LONG volatile *v) { return __atomic_sub_fetch(v, 1, __ATOMIC_SEQ_CST); }
extern "C" LONG InterlockedExchange(LONG volatile *v, LONG x) { return __atomic_exchange_n(v, x, __ATOMIC_SEQ_CST); }
extern "C" LONG InterlockedExchangeAdd(LONG volatile *v, LONG x) {
  return __atomic_fetch_add(v, x, __ATOMIC_SEQ_CST);
}
extern "C" LONG InterlockedCompareExchange(LONG volatile *v, LONG x, LONG cmp) {
  __atomic_compare_exchange_n(v, &cmp, x, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return cmp;
}

/* --------------------------------------------------------------------- TLS */

extern "C" DWORD TlsAlloc(void) {
  pthread_key_t key;
  if (pthread_key_create(&key, nullptr) != 0)
    return TLS_OUT_OF_INDEXES;
  return (DWORD)key;
}
extern "C" BOOL TlsFree(DWORD i) { return pthread_key_delete((pthread_key_t)i) == 0; }
extern "C" LPVOID TlsGetValue(DWORD i) { return pthread_getspecific((pthread_key_t)i); }
extern "C" BOOL TlsSetValue(DWORD i, LPVOID v) { return pthread_setspecific((pthread_key_t)i, v) == 0; }

/* -------------------------------------------------- file mappings (unused) */

extern "C" HANDLE CreateFileMappingA(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR) { return nullptr; }
extern "C" LPVOID MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, SIZE_T) { return nullptr; }
extern "C" BOOL UnmapViewOfFile(LPCVOID) { return TRUE; }

/* ------------------------------------------------- CRT thread entry points */

#include "process.h"

namespace {
struct CrtThreadStart {
  void (*start)(void *) = nullptr;
  unsigned (*start_ex)(void *) = nullptr;
  void *arg = nullptr;
};
DWORD Crt_Thread_Entry(LPVOID p) {
  CrtThreadStart s = *static_cast<CrtThreadStart *>(p);
  delete static_cast<CrtThreadStart *>(p);
  if (s.start_ex)
    return s.start_ex(s.arg);
  s.start(s.arg);
  return 0;
}
} // namespace

extern "C" uintptr_t _beginthread(void (*start)(void *), unsigned stack, void *arg) {
  CrtThreadStart *s = new CrtThreadStart;
  s->start = start;
  s->arg = arg;
  HANDLE h = CreateThread(nullptr, stack, Crt_Thread_Entry, s, 0, nullptr);
  if (!h) {
    delete s;
    return (uintptr_t)-1;
  }
  return (uintptr_t)h;
}

extern "C" uintptr_t _beginthreadex(void *, unsigned stack, unsigned (*start)(void *), void *arg, unsigned flags,
                                    unsigned *thread_id) {
  CrtThreadStart *s = new CrtThreadStart;
  s->start_ex = start;
  s->arg = arg;
  DWORD id = 0;
  HANDLE h = CreateThread(nullptr, stack, Crt_Thread_Entry, s, flags, &id);
  if (!h) {
    delete s;
    return 0;
  }
  if (thread_id)
    *thread_id = id;
  return (uintptr_t)h;
}

extern "C" void _endthread(void) { ExitThread(0); }
extern "C" void _endthreadex(unsigned code) { ExitThread(code); }
