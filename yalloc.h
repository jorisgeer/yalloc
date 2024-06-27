/* yalloc.h - common definitions

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

// enable extensions conditionally
#if  Isclang
 #pragma clang diagnostic error "-Wimplicit-function-declaration"
 #pragma clang diagnostic error "-Wincompatible-pointer-types"
 #pragma clang diagnostic error "-Wconditional-uninitialized"
 #pragma clang diagnostic error "-Wuninitialized"
 #pragma clang diagnostic error "-Wmissing-field-initializers"
 #pragma clang diagnostic error "-Wmissing-prototypes"
 #pragma clang diagnostic error "-Wint-conversion"
 #pragma clang diagnostic ignored "-Wunused"
 #pragma clang diagnostic ignored "-Wpadded"
 #pragma clang diagnostic ignored "-Wc2x-compat"
 #pragma clang diagnostic ignored "-Wdeclaration-after-statement"
 #pragma clang diagnostic ignored "-Wcast-align"
#elif Isgcc
 #pragma GCC diagnostic error "-Wshadow"
 #pragma GCC diagnostic error "-Wmaybe-uninitialized"
 #pragma GCC diagnostic error "-Wimplicit-function-declaration"
 #pragma GCC diagnostic error "-Wmissing-field-initializers"
 #pragma GCC diagnostic error "-Wvla"
 #pragma GCC diagnostic error "-Warray-bounds"
#endif

#if Isgcc || Isclang
 #define Mallike __attribute__ ((malloc))
 #define Align(a) __attribute__ ((aligned (a)))
 #define likely(a) __builtin_expect( (a),1)
 #define unlikely(a) __builtin_expect( (a),1)

 #define Overflows // __attribute__((no_sanitize("unsigned-integer-overflow")) )
 #define Cold __attribute__ ((cold))
 #define Hot __attribute__ ((hot))

 #if defined (__x86_64__) || defined (__i386__)
  #define Pause __builtin_ia32_pause();
 #elif defined (__arm__)
  #define Pause __yield();
 #else
  #define Pause
 #endif

 #define ctz(x) (unsigned int)__builtin_ctz((x))
 #define clz(x) (unsigned int)__builtin_clz((x))
 #define ctzl(x) (unsigned int)__builtin_ctzl((x))
 #define clzl(x) (unsigned int)__builtin_clzl((x))
 #define sat_mul(a,b,res) __builtin_umull_overflow((a),(b),(res))
#else
// #define Inline inline
 #define Mallike

#endif

#ifdef __has_builtin
  #if __has_builtin(__builtin_clz)
    #define Useclz
  #endif
#endif

// todo others
#ifndef Useclz
  static int clz(unsigned int x)
  {
    int n = 0;
    unsigned int bit = 1;
    while (x) x &= (1u << n++);
    return 32 - n;
  }
#endif

#define doalign(ip,n) ( (ip) + (n) - 1) & ~((n) - 1)
// #define isalign(p,n) ( (p) & ~((n) - 1) == 0)

// #define hi15 0x7fff
//#define hi20 0xfffffU
//#define hi24 0xffffffU
//#define hi28 0xfffffffU
//#define hi31 0x7fffffffU
//#define hi56 0xffffffffffffffUL

//#define Bit15 0x8000U
//#define Bit30 0x40000000U
//#define Bit31 0x80000000U
#define Bit63 0x8000000000000000UL
