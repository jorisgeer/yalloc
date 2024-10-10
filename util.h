/* util.h - utility functions

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

// David Stafford's murmur3 variant mixer - http://zimbry.blogspot.com/2011/09/better-bit-mixing-improving-on.html

static
#if Isgcc || Isclang
__attribute__((no_sanitize("unsigned-integer-overflow")))
#endif
  Const unsigned long long murmurmix(unsigned long long x) // mix 13 = 30	0xbf58476d1ce4e5b9	27	0x94d049bb133111eb	31
{
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9UL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebUL;
  x ^= x >> 31;

  return x;
}

static inline ub4 Const doalign4(ub4 n,ub4 a) { return (n + a - 1) & ~(a - 1); }

static inline size_t Const doalign8(size_t n,size_t a) { return (n + a - 1) & ~(a - 1); }

static size_t atox(cchar *s)
{
  size_t x = 0;
  char c;

  while ( (c = *s++)  ) {
    if (c == '.') continue;
    x <<= 4;
    if (c >= '0' && c <= '9') x += (size_t)c - '0';
    else {
      c |= 0x20;
      if (c >= 'a' && c <= 'f') x += (size_t)c - 'a' + 10;
      else return x;
    }
  }
  return x;
}

static size_t atoul(cchar *p)
{
  size_t x = 0;
  char c;

  if (*p == '0' && (p[1] | 0x20) == 'x') return atox(p + 2);

  while ( (c = *p++) ) {
    if (c == '.') continue;
    if (c >= '0' && c <= '9') x = x * 10 + (size_t)(c - '0');
    else break;
  }
  return x;
}

static ub4 atou(cchar *p) { return (ub4)atoul(p); }

static inline Const bool sometimes(const size_t a,const ub4 b)
{
  return ( (a & b) == b);
}
