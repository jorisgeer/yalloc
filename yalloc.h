/* yalloc.h - common definitions

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

static const char yal_version[] = "0.8.1-alpha.0";

#if Isgcc || Isclang // enable extensions conditionally
 #define Mallike __attribute__ ((malloc))
 #define Align(a) __attribute__ ((aligned (a)))
 #define likely(a) __builtin_expect( (a),1)
 #define unlikely(a) __builtin_expect( (a),0)

 #define Hot __attribute__ ((hot))

 #if defined (__x86_64__) || defined (__i386__)
  #define Pause __builtin_ia32_pause();
 #elif defined (__arm__)
  #define Pause __yield();
 #else
  #define Pause
 #endif

 #define sat_mul(a,b,res) __builtin_umull_overflow((a),(b),(res))

 #ifdef __has_builtin
  #if __has_builtin(__builtin_clz)
    #define clz(x) (unsigned int)__builtin_clz((x))
    #define clzl(x) (unsigned int)__builtin_clzl((x))
  #endif
  #if __has_builtin(__builtin_ctz)
    #define ctz(x) (unsigned int)__builtin_ctz((x))
    #define ctzl(x) (unsigned int)__builtin_ctzl((x))
  #endif
 #endif

#else
 #define Mallike
 #define Align(a)
 #define likely(a) (a)
 #define unlikely(a) (a)
 #define Cold
 #define Hot
 #define Pause

 static bool sat_mul(unsigned long a,unsigned long b,unsigned long *res)
 {
   unsigned long c = a * b;
   *res = c;
   return c < a || c < b; // heuristic
 }

#endif // gcc or clang

// rough approx
static void sat_inc(unsigned int *a)
{
  *a = (*a & 0x7fffffff) + 1;
}

#ifndef clz
  static Const int clz(unsigned int x)
  {
    int n = 31;
    while ( (x & (1u << n)) == 0) n--;
    return 32 - n;
  }
  static Const int clzl(unsigned long x)
  {
    int n = sizeof(long) * 8 - 1;
    while ( (x & (1ul << n)) == 0) n--;
    return sizeof(long) * 8 - n;
  }
#endif

#ifndef ctz
  static Const int ctz(unsigned int x)
  {
    int n = 0;
    while ( (x & (1u << n)) == 0) n++;
    return n;
  }

  static Const int ctzl(unsigned long x)
  {
    int n = 0;
    while ( (x & (1u << n)) == 0) n++;
    return n;
  }
#endif

#define Hi20 0xfffffu
#define Hi24 0xffffffU
#define Hi30 0x3fffffffu
#define Hi31 0x7fffffffu
#define Hi64 0xfffffffffffffffful

#define Bit31 0x80000000ul
