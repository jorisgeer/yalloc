/* lock.h - locking primitives

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

#undef Logfile
#define Logfile Flock

#ifdef Have_atomics
 #include <stdatomic.h>
 #define Cas(cmp,exp,des) atomic_compare_exchange_weak_explicit(&(cmp),&(exp),(des),memory_order_acquire,memory_order_relaxed)
 #define Atomad(a,b) atomic_fetch_add_explicit(&(a),(b),memory_order_relaxed);
 #define Atomget(a) atomic_load_explicit(&(a),memory_order_relaxed);
 #define Atomset(a,b) atomic_store_explicit(&(a),(b),memory_order_relaxed);

#else

 #warning "multithreading support requires C11 atomics"
 #define _Atomic
 #define Cas(cmp,exp,des) (cmp) = (des)
 #define Atomad(a,b) (a) += (b);
 #define Atomget(a) (a);
 #define Atomset(a,b) (a) = (b);
#endif

// --- OS locks ---

// https://outerproduct.net/futex-dictionary.html

#if Yal_locking == 0 || Yal_inter_thread_free == 0 // none
typedef unsigned int oslock_t;

// --- linux futex(2) ---
#elif Yal_locking == 1
typedef uint32_t oslock_t;

#include <errno.h>
#include <time.h>
#include <linux/futex.h>
#include <sys/syscall.h>

static enum Status oslock(enum Loc loc,ub4 id,_Atomic oslock_t *locvar,oslock_t cmp,ub4 timeout_us,char *errmsg)
{
  long rv;
  struct timespec ts = { 0, timeout_us * 1000 };
  uint32_t adr2,val3;
  int op = FUTEX_PRIVATE_FLAG | FUTEX_WAIT;

  ylog(loc,"< os lock at %p %u us",(void *)locvar,timeout_us);
  rv = syscall(SYS_futex,locvar,op,cmp,&ts,&adr2,&val3);
  ylog(loc,"> os lock at %p - %lu",(void *)locvar,rv);

  if (rv == -1) {
    switch(errno) {
      case ETIMEDOUT: error2(errmsg,loc,Fln,"unable to obtain futex lock for heap %u: %m",id) return St_tmo;
      case EAGAIN: return St_nolock;
      case EINTR: return St_intr;
      default: return St_error;
    }
  }
  return St_ok;
}

static enum Status osunlock(enum Loc loc,ub4 id,ub4 fln,_Atomic oslock_t *locvar,char *errmsg)
{
  long rv;
  struct timespec ts;
  uint32_t adr2,val3;
  int op = FUTEX_PRIVATE_FLAG | FUTEX_WAKE;

  ylog(loc,"< os wake at %p",(void *)locvar);
  rv = syscall(SYS_futex,locvar,op,1,&ts,&adr2,&val3);
  ylog(loc,"> os wake at %p - %lu",(void *)locvar,rv);

  if (rv == -1) { error2(errmsg,loc,Fln,"unable to release futex lock for region %x: %m",id) return St_error; }
  return St_ok;
}

// --- posix (unix) pthreads ---
#elif Yal_locking == 2
typedef pthread_mutex_t oslock_t;

  #include <pthread.h>

static int oslock(enum Loc loc,ub4 id,oslock_t *locvar,oslock_t cmp,ub4 timeout_us)
{
  struct timespec ts = { 0, timeout_us * 1000 };
  int rv;

  rv = pthread_mutex_timedlock(locvar,&ts);
  if (rv) { error(hb->errmsg,loc,"unable to obtain OS lock for heap %u: %s",strerror(hb->errmsg,rv)) }
  return rv;
}

static int osunlock(enum Loc loc,ub4 id,oslock_t *locvar)
{
  int rv = pthread_mutex_unlock(locvar);
  if (rv) { error(hb->errmsg,loc,"unable to release OS lock for region %x: %s",id,strerror(hb->errmsg,rv)) }
  return rv;
}

// --- c11 threads
#elif Yal_locking == 3

  #include <threads.h>

typedef mtx_t oslock_t;

static int oslock(enum Loc loc,ub4 id,oslock_t *locvar,oslock_t cmp,ub4 timeout_us)
{
  struct timespec ts = { 0, timeout_us * 1000 };
  int rv = mtx_timedlock(locvar,&ts);

  if (rv != thrd_success) { error(hb->errmsg,loc,"unable to obtain OS lock for heap %u: %s",rv == thrd_timedout ? "timed out" : "error") }

  return rv != thrd_success;
}

static int osunlock(enum Loc loc,ub4 id,oslock_t *locvar)
{
  mtx_unlock(locvar);
  return 0;
}

#elif Yal_locking == 4 // darwin
#include <errno.h>

typedef uint64_t oslock_t;

# include <pthread.h>

/* https://github.com/apple-oss-distributions/xnu/blob/94d3b452840153a99b38a3a9659680b2a006908e/bsd/sys/ulock.h  macos 10.12+
 https://opensource.apple.com/source/xnu/xnu-7195.50.7.100.1/bsd/sys/ulock.h.auto.html
 https://opensource.apple.com/source/xnu/xnu-7195.50.7.100.1/bsd/kern/sys_ulock.c.auto.html
 */
extern int __ulock_wait(uint32_t operation, void *addr, uint64_t value,uint32_t timeout_us);
extern int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);

#define UL_COMPARE_AND_WAIT 1
#define UL_UNFAIR_LOCK 2

#define ULF_WAKE_ALL 0x100
#define ULF_WAKE_THREAD 0x200
#define ULF_WAKE_ALLOW_NON_OWNER 0x00000400

#define ULF_NO_ERRNO 0x1000000

static enum Status oslock(enum Loc loc,ub4 id,_Atomic oslock_t *locvar,oslock_t cmp,ub4 timeout_us,char *errmsg)
{
  int rv;
  size_t tid = 0; // (size_t)(void *)pthread_self();
  uint32_t op = UL_COMPARE_AND_WAIT | ULF_NO_ERRNO;

  ylog(loc,"< ulock_wait for region %x.%zx at %p %u us",id,tid,(void *)locvar,timeout_us);
  rv = __ulock_wait(op,locvar,cmp,timeout_us);

  if (rv >= 0) {
    ylog(loc,"> ulock_wait ok for %x.%zx at %p - %d",id,tid,(void *)locvar,rv);
    return St_ok;
  }

  rv = -rv;
  ylog(loc,"> ulock_wait for %x.%zx at %p - %d %s",id,tid,(void *)locvar,rv,strerror(rv))

  switch(rv) {
    case ETIMEDOUT: error(errmsg,loc,"unable to obtain ulock for heap.region %x in %u us",id,timeout_us) return St_tmo;
    case EINVAL: error(errmsg,loc,"unable to obtain ulock for heap.region %x: %s",id,strerror(rv)) return St_error;
    case EAGAIN: return St_nolock;
    case EINTR: return St_intr;
    default: error(errmsg,loc,"unable to obtain ulock for heap.region %x: %s",id,strerror(rv)) return St_error;
  }
}

static enum Status osunlock(enum Loc loc,ub4 id,ub4 fln,_Atomic oslock_t *locvar,char *errmsg)
{
  int rv;
  enum Status ret;
  uint32_t op = ULF_NO_ERRNO;

  op |= UL_UNFAIR_LOCK;
//  uint32_t op = UL_COMPARE_AND_WAIT;

  // op |= ULF_WAKE_ALL;
  // op |= ULF_WAKE_ALLOW_NON_OWNER;

  ylog(loc,"< os wake for region %x at %zu",id,(size_t)atomic_load(locvar))
  rv = __ulock_wake(op,locvar,0);

  if (rv >= 0) {
    ylog(loc,"> os wake on region %x at %zu - %d",id,(size_t)atomic_load(locvar),rv)
    return St_ok;
  }

  rv = -rv;
  switch (rv) {
  case ENOENT:  ret = St_ok; break;
  case EOWNERDEAD:
  default: ret = St_error;
  }
  if (ret == St_ok) { ylog2(loc,fln,"lock.h:%u os wake on region %x at %zu: %s",__LINE__,id,(size_t)atomic_load(locvar),strerror(rv)) }
  else { *errmsg = 0; error2(errmsg,loc,fln,"lock.h:%u os wake on region %x at %zu: %s",__LINE__,id,(size_t)atomic_load(locvar),strerror(rv)) }
  return ret;
}

// --- windows ---
// https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitonaddress
#elif Yal_locking == 5

typedef unsigned int oslock_t;

 #include <synchapi.h>

static int oslock(enum Loc loc,ub4 id,oslock_t *locvar,oslock_t cmp,ub4 timeout_ms)
{
  bool rv = WaitOnAddress(locvar,cmp,sizeof(oslock_t),timeout_ms);
  return rv;
}

static enum Status osunlock(enum Loc loc,ub4 id,ub4 fln, oslock_t *locvar)
{
  WakeByAddressSingle(locvar);
  return 0;
}

#else
 #error "unsupported locking mode"
#endif // Yal_locking

// --- spin locks and atomic ops ---

typedef oslock_t ylock_t;

// static ylock_t two = 2;

/*
static void unlock(ylock_t *locvar)
{
  atomic_store_explicit(locvar,&zero,memory_order_release);
}
*/
// returns true on success
static inline bool trylock(_Atomic ylock_t *locvar)
{
  ylock_t zero = 0;
  return atomic_compare_exchange_weak_explicit(locvar,&zero,1,memory_order_acquire,memory_order_relaxed);
}

#if  Yal_inter_thread_free || Yal_thread_model == 2
// typically after trylock above. returns true on failure
static Hot bool trylock2(_Atomic ylock_t *locvar)
{
  int iter;
  ylock_t loc;
  ylock_t zero = 0;

  iter = Lockspin;
  do {
    Pause
    loc = atomic_load_explicit(locvar,memory_order_relaxed);
    if (loc == 2) return 1; // contended
    if (loc == 0 && atomic_compare_exchange_weak_explicit(locvar,&zero,1,memory_order_acquire,memory_order_relaxed) == 1) return 0;
  } while (--iter);

  return 1; // give up
}
#endif
