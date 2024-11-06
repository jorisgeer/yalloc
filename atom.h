/* atom.h - wrapper for atomics

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifdef Have_atomics // c11
 #include <stdatomic.h>

#define Cas(cmp,exp,des) atomic_compare_exchange_strong_explicit(&(cmp),&(exp),(des),memory_order_acq_rel,memory_order_acquire)
#define Casa(cmp,exp,des) atomic_compare_exchange_strong_explicit(cmp,exp,des,memory_order_acq_rel,memory_order_acquire)

#define cas(cmp,exp,des) atomic_compare_exchange_weak_explicit(&(cmp),&(exp),(des),memory_order_acq_rel,memory_order_acquire)
#define casa(cmp,exp,des) atomic_compare_exchange_weak_explicit(cmp,exp,des,memory_order_acq_rel,memory_order_acquire)

#define Atomad(a,b,o) atomic_fetch_add_explicit(&(a),(b),o)
#define Atomsub(a,b,o) atomic_fetch_sub_explicit(&(a),(b),o)

#define Atomget(a,o) atomic_load_explicit(&(a),o)
#define Atomgeta(a,o) atomic_load_explicit((a),o)

#define Atomset(a,b,o) atomic_store_explicit(&(a),(b),o)
#define Atomseta(a,b,o) atomic_store_explicit((a),(b),o)

#define Atomfence(o) atomic_thread_fence(o)

#define Monone memory_order_relaxed
#define Morel memory_order_release
#define Moacq memory_order_acquire
#define Moacqrel memory_order_acq_rel

#elif defined __has_builtin // gcc, clang
 #if __has_builtin(__atomic_load)
  #define Have_atomics

  #define Cas(cmp,exp,des) __atomic_compare_exchange_n(&(cmp),&(exp),(des),0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)
  #define cas(cmp,exp,des) __atomic_compare_exchange_n(&(cmp),&(exp),(des),1,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)
  #define Casa(cmp,exp,des) __atomic_compare_exchange_n((cmp),(exp),(des),0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)
  #define casa(cmp,exp,des) __atomic_compare_exchange_n((cmp),(exp),(des),1,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)

  #define Atomad(a,b,o) __atomic_fetch_add(&(a),(b),o)
  #define Atomsub(a,b,o) __atomic_fetch_sub(&(a),(b),o)

  #define Atomget(a,o) __atomic_load_n(&(a),o)
  #define Atomgeta(a,o) __atomic_load_n((a),o)

  #define Atomset(a,b,o) __atomic_store_n(&(a),(b),o)
  #define Atomseta(a,b,o) __atomic_store_n((a),(b),o)

  #define Atomfence(o) __atomic_thread_fence(o)

  #define Monone __ATOMIC_RELAXED
  #define Morel __ATOMIC_RELEASE
  #define Moacq __ATOMIC_ACQUIRE
  #define Moacqrel __ATOMIC_ACQ_REL

 #endif
#endif

#ifndef Have_atomics
  // #warning "multithreading support requires atomics"
 #define _Atomic // -V1059 PVS override-reserved

 #define Cas(cmp,exp,des) (cmp) == (exp) ? ((cmp) = (des)) && 1 : 0
 #define cas(cmp,exp,des) (cmp) == (exp) ? ((cmp) = (des)) && 1 : 0
 #define Casa(cmp,exp,des) *(cmp) == (exp) ? (*(cmp) = (des)) && 1 : 0
 #define casa(cmp,exp,des) *(cmp) == (exp) ? (*(cmp) = (des)) && 1 : 0

 #define Atomad(a,b,o) (a) += (b)
 #define Atomsub(a,b,o) (a) -= (b)

 #define Atomget(a,o) (a)
 #define Atomgeta(a,o) *(a)

 #define Atomset(a,b,o) (a) = (b)
 #define Atomseta(a,b,o) *(a) = (b)

 #define Atomfence(o) __atomic_thread_fence(o)

 #define Monone
 #define Morel
 #define Moacq
 #define Moacqrel

#endif
