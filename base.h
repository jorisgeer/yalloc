/* base.h - base definitions

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifndef __STDC__
 #error "missing __STDC__ : require iso c 99+"
#endif
#ifndef __STDC_VERSION__
 #error "missing __STDC_VERSION__ : require iso c 99+"
#endif
#if __STDC_VERSION__ < 199901L
 #error "require iso c 99+"
#endif

#if __STDC_VERSION__ < 201112L
 #warning "multithreading support requires C11"
  typedef unsigned char bool;
 #define sassert(expr,msg) if  ( (expr) == 0) assert_fail(__LINE__,__FILE__,msg)
 #define _Thread_local
 #define Func ""
 #define _Noreturn
 #define quick_exit(c) _Exit(c)
 #define Dummyret(x) return(x);
  typedef unsigned char bool;

#else

  #if __STDC_VERSION__ < 202311L
    typedef _Bool bool;
  #else
    #define Have_c23
  #endif

 #define sassert(expr,msg) _Static_assert((expr),msg)
 #define Func __func__
 #define Dummyret(x)

 #ifndef __STDC_NO_ATOMICS__
  #define Have_atomics
 #endif

#endif

#ifdef Have_atomics
 #include <stdatomic.h>
#else
 #warning "multithreading support requires C11 atomics"
 #define _Atomic
 #define atomic_fetch_add_explicit(obj,op) *obj += op
#endif

// basic types
typedef unsigned char  ub1;
typedef unsigned short ub2;
typedef unsigned int ub4;

typedef unsigned long ub8;
typedef unsigned long long ubll;

//typedef signed char sb1;
//typedef short  sb2;
typedef int sb4;
typedef long sb8;
typedef long long sbll;

typedef const char cchar;

#define nil (void *)0

// handful of useful macros
#define hi15 0x7fff
#define hi16 0xffffU
#define hi20 0xfffffU
#define hi24 0xffffffU
#define hi28 0xfffffffU
#define hi31 0x7fffffffU
#define hi32 0xffffffffU
#define hi56 0xffffffffffffffUL
#define hi64 0xffffffffffffffffUL

#define Bit15 0x8000U
#define Bit30 0x40000000U
#define Bit31 0x80000000U
#define Bit63 0x8000000000000000UL

#ifndef max
 #define max(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef min
 #define min(a,b) ((a) < (b) ? (a) : (b))
#endif

#ifndef nil
 #define nil (void*)0
#endif

// enable extensions conditionally
#if defined __clang__ && defined __clang_major__ && !defined D_BetterC
 #define Isclang 1
 #define Isgcc 0

 #pragma clang diagnostic error "-Wimplicit-function-declaration"
 #pragma clang diagnostic error "-Wincompatible-pointer-types"
 #pragma clang diagnostic error "-Wconditional-uninitialized"
 #pragma clang diagnostic error "-Wuninitialized"
 #pragma clang diagnostic error "-Wmissing-field-initializers"
 #pragma clang diagnostic error "-Wmissing-prototypes"
 #pragma clang diagnostic error "-Wint-conversion"

#pragma clang diagnostic ignored "-Wpadded"
#pragma clang diagnostic ignored "-Wc2x-compat"
#pragma clang diagnostic ignored "-Wdeclaration-after-statement"

 #ifdef __has_feature
  #if __has_feature(address_sanitizer)
   #define Asan
  #endif
 #endif

#elif defined __GNUC__ && __GNUC__ > 3 && !defined D_BetterC
 #define Isclang 0
 #define Isgcc 1

 #pragma GCC diagnostic error "-Wshadow"
 #pragma GCC diagnostic error "-Wmaybe-uninitialized"
 #pragma GCC diagnostic error "-Wimplicit-function-declaration"
 #pragma GCC diagnostic error "-Wmissing-field-initializers"
 #pragma GCC diagnostic error "-Wvla"
 #pragma GCC diagnostic error "-Warray-bounds"

 #ifdef __SANITIZE_ADDRESS__
  #define Asan
 #endif

#elif defined Have_c23
  #define ctz(x) stdc_trailing_zerosull((x))

#else
 #define Isclang 0
 #define Isgcc 0
#endif

#if Isgcc || Isclang
 #define Packed8 __attribute__((packed))
 #define Fallthrough __attribute__ ((fallthrough));
 #define Mallike __attribute__ ((malloc))
 #define _Printf(fmt,ap) __attribute__ ((format (printf,fmt,ap)))
 #define Unused __attribute__ ((unused))
 #define Align(a) __attribute__ ((aligned (a)))
 #define likely(a) __builtin_expect_with_probability( (a),1,0.999)
 #define unlikely(a) __builtin_expect_with_probability( (a),1,0.001)

 #define ctz(x) (unsigned int)__builtin_ctz((x))
 #define clz(x) (unsigned int)__builtin_clz((x))
 #define ctzl(x) (unsigned int)__builtin_ctzl((x))
 #define clzl(x) (unsigned int)__builtin_clzl((x))
 #define sat_mul(a,b,res) __builtin_umull_overflow((a),(b),(res))
#else
 #define Packed8
 #define Attribute(name)
 #define Fallthrough
 #define Inline inline
 #define Mallike
 #define Unused
 #define Align(a)
 #define _Printf(fmt,ap)
#endif

#ifdef __has_builtin
  #if __has_builtin(__builtin_clz)
    #define Useclz
  #endif
#endif

#ifndef Useclz
  static int  __builtin_clz(unsigned int x)
  {
    int n = 0;
    unsigned int bit = 1;
    while (x) { x &= (1u << n++);
    return 32 - n;
  }
#endif

#ifdef UINT128_MAX
  typedef uint128_t ub16;
  typedef int128_t sb16;
#elif Isgcc || Isclang
  typedef __uint128_t ub16;
  typedef __int128_t sb16;
#endif

#define doalign(p,n) ( (p) + (n) - 1) & ~((n) - 1)

#if defined _FORTIFY_SOURCE && _FORTIFY_SOURCE > 0
  // #warning "_FORTIFY_SOURCE is defined"
#endif
