/* printf.c - miniature printf-style string formatting without dependencies.

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   For obvious reasons, any string formatting within yalloc cannot invoke malloc(), and any other dependency will make yalloc inherit such.
   Thus, this miniature version is used, being reasonably standards-conformant and has no more dependencies than yalloc has.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

   mini snprintf(). All C11 features except multibyte / wide char are supported. C23 features are recognised, yet only wN supported

   A nonstandard, yet standards-compatible backtick *after* a conversion modifiies the behaviour as follows :
     - After an integer conversion spec, formats a human-readable int with K/M/G/T suffix.    "%u`",123_456_789  = '12.3M'
     - After a string conversion spec, adds a 's' plural char if the previous integer converted was not '1'. %u %s``,itemcnt,"item"  = '2 items' for item == 2

   A nonstandard leading zero in a precision for %u translates into "%u.%u",val >> 16,val & 0xffff), or 32 bits for %lu
   For %x and %b it enables grouping in 4 digits

   These two in order to have the compiler's format diagnostics work whilst having a way to use extensions.

   TODO floating point rounding is inaccurate, last digit may be wrong

   The base function is :

   unsigned int snprintf_mini(char * restrict str, unsigned int offset, unsigned int len, const char * restrict format, ...)

   and its related mini_vsnprintf(... va_list ap)

   Formatted output, including a null byte, is written into 'str' starting at 'offset' up to the maximum string length 'len'.
   The offset facilitates stepwise appending to a string without adjusting the length parameter

   In contrast with snprintf, at most (len - 1) bytes are written and a null always appended, thus the actual buffer length can be passed.
   The number of chars - excluding the null - actually written is returned.
   Example:

   char buffer[Buflen];
   unsigned int current;

   current  = snprintf_mini(buffer, 0, Buflen, "bla %a %b %c %d %e %f %g %hu \n ", 1.2, 3, '4', -5, 6e7, 0.89, 1e5, 11);
   current += snprintf_mini(buffer, current, Buflen, "more stories %u \n", 1234);
   write(1, buffer, current);

   format string is parsed into conversion spec, flags, modifiers, width and precision. The latter are fetched for '*'
   all are interpreted and normalized
   args are fetched according to modifiers
   conversion specs, width and precision are further normalized / changed based on value
   conversion is performed
   negative values are converted as positive and sign prepended
   zero values are handled specially
   padding and decimal separators are added at the end
 */

/* Floating-point support
  0 - print as integer
  1 - elementary support, can be slow
  2 - full support - do not use for yalloc
 */
#define Mini_printf_float_formats 1 // %f %g %a

#include <stddef.h> // size_t

#include <errno.h> // %m
#include <limits.h> // long_max

#if Mini_printf_float_formats
// #include <float.h> // mant-dig
#include <math.h> // fpclassify, frexp,pow for formats = 2
#endif

#include <stdarg.h>
#include <stdint.h> // intmax_t
#include <string.h> // memcpy

#include "base.h"

typedef long long sb8;

#include "printf.h"

// max single conversion length without padding, except %s
#define Maxfmt 256

enum Radix { Radixdec,Radixoct,Radixhex,Radixbin };

enum Packed8 Fmt { // note the value assignments
  Fmtinv,
  Flgws=1,
  Flgpls=2, // +
  Flgpad0=4, // %02d

  Fmtdot,
  Fmtast, // %*d

  Flgleft=8, // %-3u

  // len mods
  Modh,Modl,ModL,
  Modj, // intmax_t
  Modz, // size_t
  Modt, // ptrdiff_t
  Modw, // wN

  Flgalt=16,

  ModD, // Decimal
  ModH,

  Flghr=32, // nonstandard human-readable 2.3M

  // 0-9
  Dig0,Dig1,Dig2,Dig3,Dig4,Dig5,Dig6,Dig7,Dig8,Dig9,

  Flgsep=64, // %'
  Fmtc,Fmts,Fmtm,
  Fmtd,Fmtu,Fmtb,FmtB,Fmto,Fmtx,FmtX,Fmtp,
  Fmtf=76,FmtF,Fmtg,FmtG,Fmte,FmtE,Fmta,FmtA, // odd for uppercase
  Fmtpct, // %%
  Fmtn, // %n
  Fmteof
};

enum Packed8 Cnv {
  Cnvinv,
  Cnvu, // all int
  Cnvc, // %c
  Cnve, // all float
  Cnvs, // %s
  Cnvp, // %p
  Cnvn, // %n
  Cnvm, // %m

  // derived
  Cnvlu,Cnvllu,
  Cnvle,
  Cnvju
};

static const ub1 hextab[16] = "0123456789abcdef";
static const ub1 Hextab[16] = "0123456789ABCDEF";

// %u
static ub1 *ucnv(ub1 *end,unsigned int x)
{
    do *--end = (ub1)((x % 10) + '0'); while (x /= 10);
    return end;
}

// %x
static ub1 *hexcnv(ub1 *end,unsigned int x,ub1 Case)
{
  const unsigned char *tab = Case ? Hextab : hextab;

  do {
    *--end = tab[x & 0xf];
  } while (x >>= 4);
   return end;
}

// %o %b
static ub1 *xcnv(ub1 *end,unsigned int x,enum Radix rdx,enum Fmt flags)
{
  ub1 msk=1,shr=1;

  switch (rdx) {
  case Radixdec:
  case Radixhex: return end;

  case Radixbin: break;
  case Radixoct: msk = 7; shr = 3; break;
  }
  do *--end = (x & msk) + '0'; while (x >>= shr);
  if ( (flags & Flgalt) && *end != '0') *--end = '0';
  return end;
}

static const unsigned char cnvtab[200] =
  "00010203040506070809"
  "10111213141516171819"
  "20212223242526272829"
  "30313233343536373839"
  "40414243444546474849"
  "50515253545556575859"
  "60616263646566676869"
  "70717273747576777879"
  "80818283848586878889"
  "90919293949596979899"
;

// %lu
static ub1 *ulcnv(ub1 *end,unsigned long x)
{
  const unsigned char *p;

  if (x == 0) { *--end = '0'; return end; }

  while (x >= 10) {
    p = cnvtab + 2 * (x % 100);
    *--end = p[1];
    *--end = p[0];
    x /= 100;
  }
  if (x) *--end = (ub1)(x + '0');
  return end;
}

// %lx
static ub1 *hexlcnv(ub1 *end,unsigned long x,ub1 Case)
{
  const unsigned char *tab = Case ? Hextab : hextab;

  do {
    *--end = tab[x & 0xf];
  } while (x >>= 4);
   return end;
}

// %lo %lb
static ub1 *xlcnv(ub1 *end,unsigned long x,enum Radix rdx,enum Fmt flags)
{
  ub1 msk=1,shr=1;

  switch (rdx) {
  case Radixhex:
  case Radixdec: return end;

  case Radixbin: break;
  case Radixoct: msk = 7; shr = 3; break;
  }
  do *--end = (x & msk) + '0'; while (x >>= shr);
  if ( (flags & Flgalt) && *end != '0') *--end = '0';
  return end;
}

#if ULLONG_MAX > ULONG_MAX
// ll mod
static ub1 *ullcnv(ub1 *end,unsigned long long x)
{
  const unsigned char *p;
  ub4 n = 0;

  if (x == 0) { *--end = '0'; return end; }

  while (x >= 10) {
    p = cnvtab + 2 * (x % 100);
    *--end = p[1];
    *--end = p[0];
    x /= 100;
  }
  if (x) *--end = (ub1)((x % 10) + '0');
  return end;
}

static ub1 *hexllcnv(ub1 *end,unsigned long long x,ub1 Case)
{
  const unsigned char *tab = Case ? Hextab : hextab;

  do {
    *--end = tab[x & 0xf];
  } while (x >>= 4);
   return end;
}

static ub1 *xllcnv(ub1 *end,unsigned long long x,enum Radix rdx,enum Fmt flags,ub1 Case)
{
  ub1 msk=1,shr=1;

  switch (rdx) {
  case Radixhex:
  case Radixdec: return end;

  case Radixbin: break;
  case Radixoct: msk = 7; shr = 3; break;
  }
  do *--end = (x & msk) + '0'; while (x >>= shr);
  if ( (flags & Flgalt) && *end != '0') *--end = '0';
  return end;
}
#elif Mini_printf_float_formats > 0
  #define ullcnv(end,x) ulcnv(end,x)
  #define hexllcnv(end,x,case) hexlcnv(end,x,case)
#endif

// human-readable %u, 2.3G
static ub1 *hrcnv(ub1 *end,ub4 x1,ub4 x2,ub1 scale)
{
  *--end = scale;
  *--end = ' ';

  x2 &= 0x3ff;
  if (x2 > 999) { x1++; x2 = 0; }
  else x2 /= 100;

  if (x2) {
    *--end = (ub1)x2 + '0';
    *--end = '.';
  }
  return ucnv(end,x1);
}

static ub1 *Ucnv(ub1 *end,ub4 x)
{
  ub4 x1,x2;
  ub1 scale;

  if (x >= 1024u * 1024u * 1024u) { x1 = x >> 30; x2 = x >> 20; scale = 'G'; }
  else if (x >= 1024u * 1024u) { x1 = x >> 20; x2 = x >> 10; scale = 'M'; }
  else if (x > 9999) { x1 = x >> 10; x2 = x; scale = 'k'; }
  else {
    return ucnv(end,x);
  }

  return hrcnv(end,x1,x2,scale);
}

// idem, long
static ub1 *Ulcnv(ub1 *end,unsigned long x)
{
  ub4 x1,x2;
  ub1 scale;
  ub2 shift;

#if ULONG_MAX > 0xffffffff
  if (x >= (1ul << 60)) { shift = 60; scale = 'E'; }
  else if (x >= (1ul << 50)) { shift = 50; scale = 'P'; }
  else if (x >= (1ul << 40)) { shift = 40; scale = 'T'; }
  else
#endif
   if (x >= (1ul << 30)) { shift = 30; scale = 'G'; }
  else {
    return Ucnv(end,(ub4)x);
  }
  x1 = (ub4)(x >> shift);
  x2 = (ub4)(x >> (shift - 10));
  return hrcnv(end,x1,x2,scale);
}

// for all floating point formats, zero, inf and nan are handled separately and negatives converted
// for %f
static ub1 *ullcnv_dot(ub1 *end,unsigned long long x,ub4 dotpos)
{
  const unsigned char *p;
  ub4 n = 0;

  while (x >= 10) {
    p = cnvtab + 2 * (x % 100);
    *--end = p[1]; if (++n == dotpos) *--end = '.';
    *--end = p[0]; if (++n == dotpos) *--end = '.';
    x /= 100;
  }
  if (x) *--end = (ub1)(x + '0');
  return end;
}

#if Mini_printf_float_formats

static double tentab[19] = { 10.0,100.0,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,1e11,1e12,1e13,1e14,1e15,1e16,1e17,1e18 };
static double itentab[19] = { 0.1,0.01,1e-3,1e-4,1e-5,1e-6,1e-7,1e-8,1e-9,1e-10,1e-11,1e-12,1e-13,1e-14,1e-15,1e-16,1e-17,1e-18 };

static double sixteentab[17] = { 1.0,16.0,256.0,4096.0,65536.0,1.04857600e+06,1.677721600e+07,2.6843545600e+08,4.29496729600e+09,6.871947673600e+10,1.09951162777600e+12,1.759218604441600e+13,2.8147497671065600e+14,4.50359962737049600e+15,7.2057594037927936e+16,1.1529215046068472e+18,1.8446744073709556e+19 };

// %a x > 0 < inf
static ub1 *acnv(ub1 *end,double x,enum Fmt flags,ub4 prec,ub1 Case)
{
  int exp;
  ub4 p;
  ub4 uexp = 0,nexp;
  double mant;
  unsigned long long imant;

  if (prec == Hi32) p = 16;
  else if (prec > 16) {
    uexp = prec - 16;
    p = 16;
  } else p = prec;

  mant = frexp(x,&exp);  // x = frac * 2^iexp
  if (exp < 0) nexp = (ub4)-exp;
  else nexp = (ub4)exp;
  end = hexcnv(end,nexp,Case);
  *--end = Case ? 'P' :  'p';
  *--end = exp < 0 ? '-' : '+';

  mant *= sixteentab[p];
  imant = (unsigned long long)mant;

  if (uexp > 1) {
    do *--end = '0'; while (--uexp);
    end -= uexp;
  }
  end = hexllcnv(end,imant,Case);
  if (prec == 0 && (flags & Flgalt) ) return end;
  end--;
  *end = end[1];
  end[1] = '.';
  *--end = Case ? 'X' : 'x';
  *--end = '0';
  return end;
}
#endif // Mini_printf_float_formats

// Primitive version
#if Mini_printf_float_formats == 1

// %e x > 0 < inf
static ub1 *ecnv(ub1 *end,double x,enum Fmt flags,ub1 casemsk,ub4 prec)
{
  ub4 pexp,exp = 0;
  ub8 ix;
  double xx,ten;
  bool eneg = 0;

  prec = min(prec,17);

  if (x < 1.0) {
    eneg = 1;
    if (x > 1e-17) {
      while (x < itentab[exp]) exp++;
      xx = x * tentab[exp];
      exp++;
    } else {
      xx = x;
      while (xx < 1.0) { exp++; xx *= 10.0; }
    }
    while (prec) { xx *= 10.0; prec--; }
  } else if (x > 1e16) {
    ten = x;
    while (ten >= 10.0) { exp++; ten *= 0.1; }
    xx = x; pexp = 0;
    while (xx >= 10.0 && pexp + prec < exp) { pexp++; xx *= 0.1; }
  } else {
    if (x < tentab[17 - prec]) {
      xx = x;
      while (x >= tentab[exp]) exp++;
      // exp++;
      pexp = exp;
      while (pexp < prec) { pexp++;  xx *= 10.0; }
    } else {
      xx = 10.0;
      while (x > xx) { exp++; xx *= 10.0; }
      xx = x;
      pexp = prec;
      while (xx < tentab[pexp]) { pexp++; xx *= 10.0; }
    }
  }
  end = ucnv(end,exp);
  if (exp < 10) *--end = '0';
  *--end = eneg ? '-' : '+';
  *--end = 'E' | casemsk;
  ix = (ub8)xx;
  if (xx - (double)ix >= 0.5) ix++;

  end = ullcnv(end,ix);

  if (prec == 0 && x >= 1.0&&  (flags & Flgalt) ) return end;
  end--;
  *end = end[1]; // insert dot
  end[1] = '.';
  return end;
}

// %f - only between 1e-18 and 1e+18
static ub1 *fcnv(ub1 *end,double x,enum Fmt flags,ub1 casemsk,ub4 prec)
{
  ub8 ix;
  ub4 exp = 0,dig = 0;

  if (x < 1.0) {
    if (x < 1e-18) return ecnv(end,x,flags,casemsk,prec);
    if (prec == 0) {
      *--end = (x >= 0.5) ? '1' : '0';
      return end;
    }
    while (dig < prec - 1 && x < itentab[dig]) dig++;
    while (exp < prec - dig && x < itentab[exp]) exp++;
    if (exp + prec > 18) { *--end = '0'; return end; } // return ecnv(end,x,flags,prec);
    x *= tentab[exp + prec];
    ix = (ub8)x;
    if (x - (double)ix >= 0.5) ix++;
    end = ullcnv(end,ix);
    while (exp) { *--end = '0'; exp--; } // 0.000 ...
    *--end = '.';
    *--end = '0';
    return end;
  }
  if (x > 1e18) return ecnv(end,x,flags,casemsk,prec);
  if (prec) {
    while (x > tentab[exp]) exp++;
    if (exp + prec > 18) return ecnv(end,x,flags,casemsk,prec);
    prec = min(prec,16);
    x *= tentab[prec - 1];
  }
  ix = (ub8)(x);
  if (x - (double)ix >= 0.5) ix++;
  return ullcnv_dot(end,ix,prec);
}

#elif Mini_printf_float_formats == 2

// normalize x to mant in [0.5..1] and 10^exp10
static double fpnorm(double x,int *piexp10)
{
  int iexp2,iexp10;
  double frac2 = frexp(x,&iexp2);  // x = frac * 2^iexp
  static double tenlog2 = 0.30102999566398119521373889472449;

  double exp10 = (long double)iexp2 * tenlog2;
  double exp10f = floorl(exp10);

  double dexp = exp10 - exp10f;

  frac2 *= pow(10.0,dexp);
  iexp10 = (int)exp10;

  // if (frac2 >=10.0) { frac2 *= 0.1; iexp10++; }
  // else if (frac2 <1.0) { frac2 *= 10.0; iexp10--; }

  *piexp10 = iexp10;

  return frac2;
}

// %f x > 0 < inf
static ub1 *fcnv(ub1 *end,double x,enum Fmt flags,ub1 casemsk,ub4 prec)
{
  int iexp,ixexp;
  ub4 uexp,exp,xexp,dotpos,digits;
  double mant,xmant;
  unsigned long long imant;

  mant = fpnorm(x,&iexp);
  end = ullcnv(end,iexp);

  if (iexp >= 0) {
    exp = (ub4)iexp;
    if (exp > 16) {
      xexp = exp - 16;
      exp -= xexp;
      memset(end - xexp,'0',xexp); end -= xexp;
    }
    dotpos = (ub4)(16 - exp);
    digits = min(prec,dotpos);
    mant *= tentab[dotpos - digits];
    xmant = mant - floor(mant);
    imant = (unsigned long long)mant;
    if (xmant >= 0.5) imant++;
    end = ullcnv_dot(end,imant,digits);
    return end;
  } else { // exp < 0
    if (-iexp <= prec) {
      if (iexp < -16) {
        ixexp = iexp + 16;
        iexp += ixexp;
      }
      uexp = (ub4)-iexp;

      mant *= tentab[16 - prec];
      xmant = mant - floor(mant);
      imant = (unsigned long)xmant;
      if (xmant >= 0.5) imant++;
      end = ullcnv(end,imant);

    } else uexp = prec;
    if (uexp > 1) {
      memset(end - uexp - 1,'0',uexp);
      end -= uexp - 1; // 0.000 ...
    }
    *--end = '.';
    *--end = '0';
  }
  return end;
}

// %e x > 0 < inf
static ub1 *ecnv(ub1 *end,double x,enum Fmt flags,ub1 casemsk,ub4 prec)
{
  int exp;
  ub4 uexp,nexp;
  double mant,xmant;
  unsigned long long imant;

  if (prec > 16) {
    uexp = prec - 16;
  } else uexp = 0;

  mant = fpnorm(x,&exp);
  mant *= tentab[16 - prec];
  xmant = mant - floor(mant);
  imant = (unsigned long long)mant;
  if (xmant >= 0.5) imant++;

  if (exp < 0) nexp = (ub4)-exp;
  else nexp = (ub4)exp;
  end = ucnv(end,nexp);
  *--end = 'E' | casemsk;
  *--end = exp < 0 ? '-' : '+';

  if (uexp > 1) {
    memset(end - uexp,'0',uexp);
    end -= uexp;
  }
  end = ullcnv(end,imant);
  if (prec == 0 && (flags & Flgalt) ) return end;
  end--;
  *end = end[1]; // insert dot
  end[1] = '.';
  return end;
}
#endif // Mini_printf_float_formats == 2

static const enum Fmt fmtmap[128] = { // format template for parsing
  [0] = Fmteof,

  // flags
  ['-'] = Flgleft,
  ['+'] = Flgpls,
  [' '] = Flgws,
  ['#'] = Flgalt,
  ['0'] = Dig0,
  ['I'] = Flghr,
  ['\''] = Flgsep,

  ['.'] = Fmtdot,
  ['*'] = Fmtast,

  ['1'] = Dig1,  ['2'] = Dig2,  ['3'] = Dig3,  ['4'] = Dig4,  ['5'] = Dig5,  ['6'] = Dig6,  ['7'] = Dig7,  ['8'] = Dig8,  ['9'] = Dig9,

  // length mods
  ['l'] = Modl,  ['h'] = Modh,  ['L'] = ModL,

  ['z'] = Modz,
  ['t'] = Modt,
  ['j'] = Modj,
  ['w'] = Modw,

  ['D'] = ModD,
  ['H'] = ModH,

  // integer
  ['b'] = Fmtb, ['B'] = FmtB,  ['d'] = Fmtd,  ['i'] = Fmtd,  ['u'] = Fmtu,  ['o'] = Fmto,  ['x'] = Fmtx,  ['X'] = FmtX,

  // float
  ['f'] = Fmtf, ['F'] = FmtF,
  ['g'] = Fmtg, ['G'] = FmtG,
  ['e'] = Fmte, ['E'] = FmtE,
  ['a'] = Fmta, ['A'] = FmtA,

  // char / string / ptr
  ['c'] = Fmtc,  ['s'] = Fmts,  ['p'] = Fmtp,

  ['n'] = Fmtn,

  ['m'] = Fmtm,

  ['%'] = Fmtpct
};

#define Pluralbuf 256

// writes into dst + pos, upto dst + dlen from fmt and args. Always zero-terminates. Returns #chars written
ub4 Cold mini_vsnprintf(char *dst,ub4 pos,ub4 dlen,const char *fmt,va_list ap)
{
  // width and precision
  ub4 wid,prec,mindig,P,pad0,slen,sndx;

  // modifiers
  // enum Lenmod mods;
  ub4 modw = 0;
  ub1 modl,modh;
  ub1 modD;

  // cnv specs
  enum Cnv cnv;
  enum Fmt fx,mod,fmtf = Fmtinv;
  enum Radix rdx;

  // flags
  enum Fmt flags;
  ub1 sign;
  bool isneg,iszero=0,isone=0,prvone=0;
  bool flgdon;
  ub1 dotseen,lzprec;
  ub1 Case,casemsk;

  // args
  sb4 s4=0;
  long s8=0;
  ub4 u4=0;
  unsigned long u8=0;
#if LLONG_MAX > LONG_MAX
  unsigned long long ull = 0;
#endif

  double f8=0;
  long double lf8;
  int fpclas = 0;

  void *vp=nil;
  unsigned int *uip;
  const char *p8;

  // cnv buf
  ub1 *org,*org2;
  ub1 cnvbuf[Maxfmt];
  ub1 pluralbuf[Pluralbuf];
  ub1 cnvbuf2[Maxfmt];
  ub1 *end2,*end = cnvbuf + Maxfmt - 1;

  // misc
  int local_err = 0;
  ub4 pi=0;
  ub2 dig,grp;

  unsigned char c;
  char d;

  ub4 len,xlen;
  ub4 sorg,padorg;
  ub4 n = 0;
  cchar *p = fmt;

  dst += pos;
  *dst = 0;

 while (pos + n + 2 < dlen) {

  c = (ub1)p[pi++];

// all conversions
 if (c == '%') {
  cnv = Cnvinv;
  prec = Hi32;
  wid = Hi32;
  mindig = Hi32;
  modl = modh = modD = 0;
  mod = 0;
  flags = 0;
  rdx = Radixdec;
  sign = 0;
  flgdon = 0;
  dotseen = 0;
  Case = 0;
  isone = 0;
  lzprec = 0;

 do { // while cnv
  c = (ub1)p[pi++];

  if (c < 128) fx = fmtmap[c];
  else fx = Fmtinv;

  switch (fx) {
   // flags:
   case Flgleft:
   case Flgpls: case Flgws: case Flgalt: case Flgsep: case Flghr:
     flags |= fx; // check flag assignment
     break;

   // width and precision
   case Dig0:
     if (flgdon == 0) { flags |= Flgpad0; flgdon = 1; }
     else if (dotseen == 1) { lzprec++; break; }
     Fallthrough

   case Dig1: case Dig2: case Dig3: case Dig4: case Dig5: case Dig6: case Dig7: case Dig8: case Dig9:
     u4 = fx - Dig0;
     if (dotseen) {
       dotseen++;
       if (prec < 65534 / 10) prec = prec * 10 + u4;
     } else {
       if (wid == Hi32) wid = u4;
       else if (wid < 65534 / 10) wid = wid * 10 + u4;
     }
     flgdon = 1; // no more flags
     break;

   case Fmtdot: // between width and precision
     dotseen = flgdon = 1;
     prec = 0;
     break;

   case Fmtast: // %*.*d
     flgdon = 1;
     s4 = va_arg(ap,int);
     if (dotseen) {
       if (s4 >= 0) prec = (ub4)s4;
     } else {
       dotseen = 1;
       if (s4 < 0) { wid = (ub4)-s4; flags |= Flgleft; }
       else wid = (ub4)s4;
     }
     break;

   // l, h and D modifiers
   case Modh: modh = (modh + 1) & 7; break;
   case Modl:  modl = (modl + 1) & 7; break;
   case ModD:  modD = (modD + 1) & 3; break;

  // c23 wN
   case Modw:
     if (p[pi] == 'f') pi++; // ignore
     modw = 0;
     while ( (c = (ub1)p[pi]) >= '1' && c <= '9') { modw = (modw & Hi16) * 10 + c - '0'; pi++; }
     Fallthrough

   case Modj:
   case Modz:
   case Modt:
   case ModH:
   case ModL: mod = fx; break;

   // integer conversions
   case Fmtc: cnv = Cnvc; break;
   case Fmtd: sign = 1; Fallthrough
   case Fmtu: cnv = Cnvu; break;
   case FmtB: Case = 1; Fallthrough
   case Fmtb: rdx = Radixbin; cnv = Cnvu; break;
   case Fmto: rdx = Radixoct; cnv = Cnvu; break;
   case FmtX: Case = 1; Fallthrough
   case Fmtx: rdx = Radixhex; cnv = Cnvu; break;

   // string
   case Fmts: prec = min(prec,dlen - pos - n); cnv = Cnvs; break;
   case Fmtm: prec = min(prec,dlen - pos - n); cnv = Cnvm; break;

   // float
   case FmtA: case Fmta: flags |= Flgalt; cnv = Cnve; break;
   case FmtF: case FmtG: case FmtE:
   case Fmtf: case Fmtg: case Fmte: cnv = Cnve; break;

   // misc
   case Fmtp: cnv = Cnvu; rdx = Radixhex; mod = Modz; flags |= Flgalt; break;

   case Fmtn: cnv = Cnvn; break;

   case Fmtpct: dst[n++] = (char)c; break;

   case Fmtinv: dst[n++] = '%'; dst[n++] = (char)c; dst[n++] = '!'; break;
   case Fmteof: case Flgpad0: break;
  } // switch fx

  if (cnv == Cnve) {
    Case = fx & 1;
    fmtf = fx & ~1;
    if (prec == Hi32 && fmtf != Fmta) prec = 6;
    else if (prec > Maxfmt - 32) prec = Maxfmt - 32;
  }

  } while (fx != 0 && fx < Fmtc); // end at cnvspec

  if (p[pi] == '`') { // nonstandard human-readable ints or plurals
    if (cnv == Cnvu || cnv == Cnvs) { pi++; flags |= Flghr; }
  }

  // apply modifiers
  if  (cnv == Cnvu) {
    if (mod == Modw) {
      switch (modw) {
        case 8: modh = 2; break;
        case 16:  modh = 1; break;
        case 32: if (sizeof(long) == 32) modl = 1; break;
        case 64:if (sizeof(long) == 32) modl = 2; else modl = 1; break;
        case 128: modl = 2; break;
      }
    } // modw
#if LLONG_MAX > LONG_MAX
    if (modl == 1) cnv = Cnvlu; // ld
    else if (modl) cnv = Cnvllu; // lld
#else
     if (modl) cnv = Cnvlu;
#endif

#if SIZE_MAX > UINT_MAX
     else if (mod == Modz) cnv = Cnvlu; // zd
#endif

#if PTRDIFF_MAX > INT_MAX
     else if (mod == Modt) cnv = Cnvlu; // td
#endif

#if LLONG_MAX > LONG_MAX
     else if (mod == Modj ) cnv = Cnvllu; // jd
#else
     else if (mod == Modj ) cnv = Cnvlu;
#endif

     if (prec == Hi32) prec = 1;
     else {
       flags = flags & ~Flgpad0;
       if (prec > Maxfmt - 32) prec = Maxfmt - 32;
     }
     mindig = prec;

  } else if (cnv == Cnve) { // Cnvu

     sign = 1;
#if Mini_printf_float_formats == 2
 #if LDBL_MANT_DIG > DBL_MANT_DIG
     if (mod == ModL) cnv = Cnvle;
 #endif
#endif

  } else {
    flags = flags & ~(Flgpad0 | Flgsep); // %s
  } // each base cnvspec

  isneg = 0; iszero = 0;
  casemsk = Case ? 0 : 0x20;

  org = end;

  switch (cnv) { // get args

   case Cnvc: // %c
     s4 = va_arg(ap,int);
   break;

   case Cnvu:    // %u %x %o %b
     u4 = va_arg(ap,unsigned int);
     iszero = (u4 == 0);
     isone = (u4 == 1);
     if (sign) {
       s4 = (sb4)u4;
       if (s4 < 0) { u4 = (ub4)-s4; isneg = 1; }
     }
     if (modh == 1) u4 = (unsigned short)u4;
     else if (modh) u4 = (unsigned char)u4;
   break;

   case Cnvlu: // %lu
     u8 = va_arg(ap,unsigned long);
     iszero = (u8 == 0);
     isone = (u8 == 1);
     if (sign) {
       s8 = (sb8)u8;
       if (s8 < 0) { u8 = (ub8)-s8; isneg = 1; }
     }
     if (u8 <= UINT_MAX) { u4 = (ub4)u8; cnv = Cnvu; }
     break;

   case Cnvllu: // %llu
#if LLONG_MAX > LONG_MAX
     ull = va_arg(ap,unsigned long long);
     iszero = (ull == 0);
     isone = (ull == 1);
     if (sign && (long long)ull < 0) { ull = -(long long)ull; isneg = 1; }
     if (ull <= UINT_MAX) { u4 = (ub4)ull; cnv = Cnvu; }
     else if (ull <= ULONG_MAX) { u8 = (ub8)ull; cnv = Cnvlu; }
#endif
     break;

   case Cnve:    // %f,e,a
   case Cnvle:
     if (cnv == Cnve) {
       f8 = va_arg(ap,double);
       fpclas = fpclassify(f8);
       switch (fpclas) {
       case FP_NORMAL: case FP_SUBNORMAL: if (f8 < 0) { isneg = 1; f8 = -f8; } break;
       case FP_INFINITE: if (f8 < 0) isneg = 1; break;
       case FP_NAN: case FP_ZERO: break;
       }
     } else {
       lf8 = va_arg(ap,long double);
       fpclas = fpclassify(lf8);
       switch (fpclas) {
       case FP_NORMAL: case FP_SUBNORMAL: if (lf8 < 0) { isneg = 1; lf8 = -lf8; } break;
       case FP_INFINITE: if (f8 < 0) isneg = 1; break;
       case FP_NAN: case FP_ZERO: break;
       }
     }
     switch (fpclas) {
     case FP_INFINITE: vp = Case ? "INF" : "inf"; cnv = Cnvs; prec = Hi32; break;
     case FP_NAN: vp = Case ? "NAN" : "nan"; cnv = Cnvs; prec = Hi32; break;
     case FP_NORMAL: case FP_SUBNORMAL: break;
     case FP_ZERO: break;
     }
     break;

   // %n,s,p
   case Cnvn:
   case Cnvs:
   case Cnvp: vp = va_arg(ap,void *);
   break;

   case Cnvm: // %m -> 'no such file: 2'
     local_err = errno;
     if (local_err) {
       p8 = strerror(local_err);
       u4 = (ub4)strlen(p8);
       if (u4 + 8 > end - cnvbuf) u4 = (ub4)(end - cnvbuf - 8);
       org = end - u4;
       memcpy(org,p8,u4);
       *--org = ' ';
       if (local_err < 0) u4 = (ub4)-local_err;
       else u4 = (ub4)local_err;
       org = ucnv(org,u4);
       if (local_err < 0) *--org = '-';
       *--org = ' '; *--org = ':';
     }
     cnv = Cnvinv;
     break;

   case Cnvju: break;
   case Cnvinv: break;
  } // get args

  // print
  if (iszero) { // Cnvu except %p
    flags = flags & ~Flgalt;
    if (prec == 0) {
      if (wid == Hi32 || wid == 0) cnv = Cnvinv;
      else { cnv = Cnvs; vp = ""; }
    }
  }

  switch (cnv) {

   case Cnvu:
     if (rdx == Radixhex && u4 <= 9) flags = flags & ~Flgalt;
     if (iszero && !lzprec) *--org = '0';
     else {
       if (flags & Flghr) org = Ucnv(end,u4);
       else if (rdx == Radixdec) {
         if (lzprec == 1) {
           org = ucnv(end,u4 & Hi16);
           *--org = '.';
           org = ucnv(org,u4 >> 16);
         } else org = ucnv(end,u4);
       } else if (rdx == Radixhex) {
         org = hexcnv(end,u4,Case);
       } else {
         org = xcnv(end,u4,rdx,flags);
       }
     }
   break;

   case Cnvp: u8 = (ub8)vp; Fallthrough
   case Cnvlu:
     if (flags & Flghr) org = Ulcnv(end,u8);
     else if (rdx == Radixdec) {
         if (lzprec == 1) {
           org = ulcnv(end,u8 & Hi32);
           *--org = '.';
           org = ulcnv(org,u8 >> 32);
       } else org = ulcnv(end,u8);
     } else if (rdx == Radixhex) org = hexlcnv(end,u8,Case);
     else org = xlcnv(end,u8,rdx,flags);
   break;

   case Cnvllu:
   case Cnvju:
#if ULLONG_MAX > ULONG_MAX
     if (rdx == Radixdec) org = ullcnv(end,ull);
     else if (rdx == Radixhex) org = hexllcnv(end,ull,Case);
     else org = xllcnv(end,ull,rdx,flags,Case);
#endif
   break;

   case Cnve:
   case Cnvle:
     prec = min(prec,16);
     if (fpclas == FP_ZERO) {
       if (fmtf == Fmte) { *--org = '0'; *--org = '0'; *--org = '+'; *--org = 'E' | casemsk; }
       else if (fmtf == Fmta) { *--org = '0'; *--org = '+'; *--org = 'P' | casemsk; }
       if (prec) { while (prec--) *--org = '0'; *--org = '.'; }
       else if (flags & Flgalt) *--org = '.';
       *--org = '0';
       break;
     }
      if (fmtf == Fmtg) {
        P = prec ? prec : 1;
        if (f8 >= 1e-4 && f8 < P) {
          fmtf = Fmtf; prec = P;
        } else {
          fmtf = Fmte; prec = P - 1;
        }
      }
      if  (fmtf == Fmtf) org = fcnv(end,f8,flags,casemsk,prec);
      else if (fmtf == Fmte) org = ecnv(end,f8,flags,casemsk,prec);
      else org = acnv(end,f8,flags,prec,Case);
   break;

   case Cnvn:
     if (vp == nil) break;
     if (modl == 1) *(unsigned long *)vp = n;
     else if (modl) *(unsigned long long *)vp = n;
     else if (modh == 1) *(unsigned short *)vp = (ub2)n;
     else if (modh) *(unsigned char *)vp = (ub1)n;
     else {
       uip = (unsigned int *)vp;
       *uip = n;
     }
   break;

   case Cnvc:
     if (wid != Hi32 && wid > 1) {
       slen = min(wid - 1,dlen - (n + pos));
       if ((flags & Flgleft)) {
         dst[n] = (char)s4;
         memset(dst + n + 1,' ',slen);
       } else {
         memset(dst + n,' ',slen);
         dst[n + slen] = (char)s4;
       }
       n += slen + 1;
     } else dst[n++] = (char)s4;
   break;

   case Cnvs:
     if (vp == nil) vp = "(nil)";
     p8 = (const char *)vp;
     if ( (flags & Flghr) && prvone != 1) { // plural
       sndx = 0;
       while (sndx < Pluralbuf - 3 && sndx < prec + 1 && p8[sndx]) { // -V557 PVS-array-overrun
         pluralbuf[sndx] = (ub1)p8[sndx]; sndx++; // -V557
       }
       pluralbuf[sndx++] = 's';
       pluralbuf[sndx] = 0;
       p8 = (const char *)pluralbuf;
     }

     slen = 0;
     if (wid != Hi32 && wid) {
       sndx = 0;
       while (slen < prec && slen < wid && p8[sndx++]) slen++; // get length
       if (wid <= slen) wid = Hi32;
     }

     if (wid != Hi32 && wid > slen) { // pad
       if ((flags & Flgleft)) { sorg = n; padorg = n + slen; }
       else { sorg = n + wid - slen; padorg = n; }
       memcpy(dst + sorg,p8,slen);
       memset(dst + padorg,' ',wid - slen);
       n += wid;
     } else {
       while (prec && (d = *p8++ ) ) { dst[n++] = d; prec--; }
     }
     break;

     case Cnvinv: case Cnvm: break;
  } // print each cnv
  prvone = isone;

  len = min( (ub4)(end - org),dlen - n - pos);
  if (len) {
    if (mindig != Hi32 && mindig > len) pad0 = mindig;
    else if ( (flags & Flgpad0) && wid != Hi32) { pad0 = wid; wid = Hi32; }
    else pad0 = 1;
    xlen = len;
    if (isneg || (flags & (Flgws | Flgpls) )) xlen++;
    if ( (flags & Flgalt) && rdx >= Radixhex) xlen += 2;

    while (org > cnvbuf + 6 && pad0-- > xlen) *--org = '0'; // minimum number of digits for int or pad with zero for int/float

    // digit grouping: 3 for decimal, 4 for hex and bin
    if (lzprec == 1 && rdx != Radixdec) flags |= Flgsep;
    if  (flags & Flgsep) {
      org2 = org; end2 = end;
      end = org = cnvbuf2 + Maxfmt - 1;
      dig = 0;
      if (rdx == Radixdec) { grp = 3; d = '.'; }
      else { grp = 4; d = '_'; }
      do {
        if (dig == grp) { *--org = (ub1)d; dig = 1; }
        else dig++;
        *--org = *--end2;
      } while (org > cnvbuf2 + 6 && end2 > org2);
    }
    if ( (cnv == Cnvu || cnv == Cnve) && (flags & Flgalt) ) {
      if (rdx == Radixhex) {
        *--org = 'X' | casemsk;
        *--org = '0';
      } else if (rdx == Radixbin) {
        *--org = 'B' | casemsk;
        *--org = '0';
      }
    }
    if (isneg) *--org = '-';
    else if (sign) {
      if  ((flags & Flgpls)) *--org = '+';
      else if  ((flags & Flgws)) *--org = ' ';
    }
    len = min( (ub4)(end - org),dlen - n - pos);
    if (wid != Hi32 && wid > len && !(flags & Flgleft)) { // pad
      wid -= len;
      memset(dst + n,' ',wid); n += wid;
      len = min( (ub4)(end - org),dlen - n - pos);
    }
    memcpy(dst + n,org,len); n += len;
    if (wid != Hi32 && wid > len && (flags & Flgleft)) {
      wid -= len;
      memset(dst + n,' ',wid); n += wid;
    }
  } // have cnv

  if (fx == Fmteof) break;
 } else { // lit
   dst[n] = (char)c;
   if (c == 0) return n;
   n++;
 }

 } // while (pos + n < dlen);

  if (dlen - (pos + n) < 3 && dlen - (pos + n) > 1) dst[n++] = '!';

 dst[n] = 0;
 return n;
} // minprint

ub4 Printf(4,5) snprintf_mini(char *dst,ub4 pos,ub4 len,cchar *fmt,...)
{
  va_list ap;
  ub4 n;

  if (pos + 2 >= len) return 0;

  va_start(ap,fmt);
  n = mini_vsnprintf(dst,pos,len,fmt,ap);
  va_end(ap);

  dst[pos + n] = 0;

  return n;
}

#ifdef Test

#include <unistd.h> // write
#include <stdlib.h> // strtod

#define Testbuf 4096
static char testbuf[Testbuf];
static char expbuf[Testbuf * 2];

static void wrch(char c)
{
  write(1,&c,1);
}

static char *append(char *buf,char *s)
{
  ub4 len = (ub4)strlen(s);

  memcpy(buf,s,len);
  buf[len] = ' ';
  return buf + len + 1;
}

static char *cnv4(char *buf,ub4 x)
{
  ub4 i;

  for (i = 0; i < 4; i++) { buf[3-i] = (x % 10) + '0'; x /= 10; }
  buf[4] = ' ';
  return buf + 5;
}

static bool Printf(3,4) check(ub4 line,char *exp,cchar *fmt,...)
{
  va_list ap;
  ub4 pos;
  ub4 explen = (ub4)strlen(exp);
  char sq = '\'';
  char *rep = expbuf;

  va_start(ap,fmt);
  memset(testbuf,' ',Testbuf);
  pos = mini_vsnprintf((char *)testbuf,0,Testbuf - 2,fmt,ap);
  va_end(ap);

  if (pos == explen && memcmp(exp,testbuf,explen+1) == 0) return 0;

  // do not use printf to format - we know it is broken if we come here
  rep = cnv4(rep,line);
  rep = append(rep,"expected:");

  rep = cnv4(rep,explen);

  rep = append(rep,exp);
  rep = append(rep,"\n     actual:  ");

  rep = cnv4(rep,pos);

  rep = append(rep,testbuf);

  *rep++ = '\n';

  write(1,expbuf,(size_t)(rep - expbuf));

  return 1;
}

#if Isgcc
#elif Isclang
 #pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif

static int test(unsigned long u8,double f8,const char *s,char *fmt)
{
  bool rv = 0;
  char buf[1024];
  ub4 u4,len = 1022;
  ub4 pos = 0;

  if (*fmt == '`') {
    f8 = 16.0;
    for (u4 = 0; u4 < 16; u4++) {
      pos += snprintf_mini(buf,pos,len,"%.16e,",f8);
      f8 *= 16;
    }
    buf[pos++] = '\n';
  }

  pos += snprintf_mini(buf,pos,len,fmt,s,u8,f8);
  buf[pos++] = '\n';
  write(1,buf,pos);

  pos = snprintf_mini(buf,0,16,"123 %s 456","12345678901234567890");
  if (pos != 16 || memcmp(buf,"123 123456789012",16)) {
    wrch('"'); write(1,buf,pos); wrch('"'); wrch('\n'); rv = __LINE__;
  }

  rv |= check(__LINE__," '0.123' "," '%.01u' ",123);

  rv |= check(__LINE__,"abc 'ghi' def","abc '%s' def","ghi");

  rv |= check(__LINE__,"abc '1234567890' def","abc '%.10s' def","123456789012345678901234567890123456789");

  rv |= check(__LINE__,"abc 'gh' def","abc '%.2s' def","ghi");
  rv |= check(__LINE__,"abc 'ghi             ' def","abc '%-16s' def","ghi");

  rv |= check(__LINE__,"abc '1.234' def","abc '%.3f' def",1.234);
  rv |= check(__LINE__,"abc '1.235e+03' def","abc '%.3e' def",1234.5);

  // rv |= check(__LINE__,"abc '34' def","abc '%w8x' def",0x1234); // c23

  rv |= check(__LINE__,"101","%b",5);
  rv |= check(__LINE__,"1001000110100010101100111100010011010101111001101111011110000","%llb",0x123456789abcdef0ull);

  rv |= check(__LINE__,"abc '1.200000' ghi","abc '%f' ghi",1.2);
  rv |= check(__LINE__,"abc '1.2000000000000000' ghi","abc '%.18f' ghi",1.2);
  rv |= check(__LINE__,"abc '123.456000' ghi","abc '%f' ghi",123.456);

  return rv;
}

int main(int argc,char *argv[])
{
  unsigned long u8 = 0;
  double f8 = 0.0;
  char *fmt = "";
  const char *str = "test";

  if (argc > 1) fmt = argv[1];
  if (argc > 2) str = argv[2];
  if (argc > 3) u8 = strtoul(argv[3],nil,0);
  if (argc > 4) f8 = strtod(argv[4],nil);

  return test(u8,f8,str,fmt);
}
#endif // Test
