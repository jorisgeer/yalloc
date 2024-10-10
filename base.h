/* base.h - base definitions

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

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

#if __STDC_VERSION__ < 201112L // c99
  typedef unsigned char bool;
 #define static_assert(expr,msg) if  ( (expr) == 0 ) assert_fail(Fln,msg)

  #if defined __GNUC__ && __GNUC__ > 7
  #else
   #define _Thread_local
  #endif

#else // c11+

 #ifndef __STDC_NO_ATOMICS__
  #define Have_atomics
 #endif

  #define Noret _Noreturn

 #if __STDC_VERSION__ <= 201710L // c11/17
  typedef _Bool bool;
  #define static_assert(expr,msg) _Static_assert((expr),msg)
 #endif

#endif // stdc

// basic types
typedef unsigned char  ub1;
typedef unsigned short ub2;
typedef unsigned int ub4;
typedef int sb4;
typedef unsigned long long ub8;

typedef const char cchar;

#define nil (void *)0

#define Hi16 0xffffu
#define Hi32 0xfffffffful

#ifndef max
 #define max(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef min
 #define min(a,b) ((a) < (b) ? (a) : (b))
#endif

// enable extensions conditionally
#if defined __clang__ && defined __clang_major__ && !defined D_BetterC
 #define Isclang 1
 #define Isgcc 0
 #pragma clang diagnostic error "-Wimplicit-function-declaration"
 #pragma clang diagnostic error "-Wincompatible-pointer-types"
 #pragma clang diagnostic error "-Wconditional-uninitialized"
 #pragma clang diagnostic error "-Wuninitialized"
 #pragma clang diagnostic error "-Wmissing-prototypes"
 #pragma clang diagnostic error "-Wint-conversion"
 #pragma clang diagnostic error "-Wshadow"
 #pragma clang diagnostic error "-Wzero-length-array"
 #pragma clang diagnostic error "-Wshift-count-overflow"
 #pragma clang diagnostic error "-Wshorten-64-to-32"
 #pragma clang diagnostic error "-Watomic-implicit-seq-cst"
 #pragma clang diagnostic error "-Wunused-value"
 #pragma clang diagnostic error "-Wformat"
 #pragma clang diagnostic warning "-Wimplicit-int-conversion"
 #pragma clang diagnostic ignored "-Wpadded"
 #pragma clang diagnostic ignored "-Wunused-function"
 #pragma clang diagnostic ignored "-Wdeclaration-after-statement"
 #pragma clang diagnostic ignored "-Wreserved-identifier"
 #pragma clang diagnostic ignored "-Wc2x-compat"
// #pragma clang diagnostic ignored "-Wunused-parameter"
// #pragma clang diagnostic ignored "-Wunused-variable"
// #pragma clang diagnostic ignored "-Wunused-but-set-variable"

 #pragma clang diagnostic ignored "-Wcovered-switch-default"
 #if __has_warning("-Wunsafe-buffer-usage")
  #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
 #endif

#elif defined __GNUC__ && __GNUC__ > 7 && !defined D_BetterC
 #define Isclang 0
 #define Isgcc 1
 #pragma GCC diagnostic error "-Wshadow"
 #pragma GCC diagnostic error "-Wmaybe-uninitialized"
 #pragma GCC diagnostic error "-Wimplicit-function-declaration"
 #pragma GCC diagnostic error "-Wmissing-field-initializers"
 #pragma GCC diagnostic error "-Wvla"
 #pragma GCC diagnostic error "-Warray-bounds"
 #pragma GCC diagnostic error "-Wformat"
 #pragma GCC diagnostic error "-Wunused-value"
 #pragma GCC diagnostic ignored "-Wunused-variable"
 #pragma GCC diagnostic ignored "-Wunused-parameter"
 #pragma GCC diagnostic ignored "-Wpadded"
 #pragma GCC diagnostic ignored "-Wattributes"
#else
 #define Isclang 0
 #define Isgcc 0
#endif

#if Isgcc || Isclang
 #define Packed8 __attribute__((packed))
 #define Fallthrough __attribute__ ((fallthrough));
 #define Printf(fmt,ap) __attribute__ ((format (printf,fmt,ap)))
 #define Cold __attribute__ ((cold))
 #define Unused __attribute__ ((unused))
 #define Const __attribute__ ((const))

 #if __STDC_VERSION__ < 201112L // c99
   #define Noret __attribute__ ((noreturn))
   #define _Thread_local__thread
 #endif

#else
 #define Packed8
 #define Fallthrough
 #define Printf(fmt,ap)
 #define Cold
 #define Unused
 #define Const

 #if __STDC_VERSION__ < 201112L // c99
   #define Noret
   #define _Thread_local
 #endif
#endif

#if defined _FORTIFY_SOURCE && _FORTIFY_SOURCE > 0
//  #warning "_FORTIFY_SOURCE is defined"
#endif
