/* util.h - utility functions

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

static inline ub4 Const doalign4(ub4 n,ub4 a) { return (n + a - 1) & ~(a - 1); }

static inline size_t Const doalign8(size_t n,size_t a) { return (n + a - 1) & ~(a - 1); }

static bool chkalign(void *p,size_t len) {
  static const ub2 as[] = { 1,1,2,4,4,8,8,8 };
  ub4 a;
  size_t ap,ip = (size_t)p;

  if (len < 8) a = as[len];
  else a = Basealign;
  ap = doalign8(ip,a);
  return ap != ip;
}

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

static inline Const bool sometimes(const ub4 a,const ub4 b)
{
  return ( (a & b) == b);
}
