/* printf.c - miniature printf-style string formatting

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

   mini snprintf(). All C11 features except multibyte / wide char are supported. C23 features are recognised, yet only wN supported

   A nonstandard backtick <after> an integer conversion spec formats a human-readable int with K/M/G/T suffix
   A nonstandard %n pointing to an intever with value Printf_dot will make the next %d/x/b use digit grouping
   These two in order to have the compler's format diagnostics work whilst having a way to use extensions.

   TODO floating point rounding is inaccurate, last digit may be off

   The base function is :

   unsigned int snprintf_mini(char * restrict str, unsigned int offset, unsigned int len, const char * restrict format, ...)

   Formatted output, including a null byte, is written into 'str' starting at 'offset' up to the maximum string length 'len'.
   The offset facilitates stepwise appending to a string without adjusting the length parameter

   In contrast with snprintf, at most (len - 1) bytes are written and a null always appended, thus the actual buffer length can be passed.
   The number of chars - excluding the null - actually written is returned. Example:

   char buffer[Buf_len]
   unsigned int count;

   count  = snprintf_mini(buffer, 0, Buf_len, "bla %a %b %c %d %e %f %g %hu \n ", 1.2, 3, '4', -5, 6e7, 0.89, 1e5, 11);
   count += snprintf_mini(buffer, count, Buf_len, "more stories %z \n", 1234);

   write(1, buffer, count);

   format string is parsed into conversion spec, flags, modifiers, width and precision. The latter are fetched for '*'
   all are interpreted and normalized
   args are fetched according to modifiers
   conversion specs, width and precision are further normalized / changed based on value
   conversion is performed
   negative values are converted as positive and sign prepended
   zero values are handled specially
   padding and decimal separators are added at the end
 */

#define Mini_printf_float_formats 0 // %f %g %a

#include <errno.h> // %m
#include <limits.h> // long_max

#if Mini_printf_float_formats > 0
 #include <float.h> // mant-dig
#endif
 #include <math.h> // frexp, pow

#include <stdarg.h>
#include <stdint.h> // intmax_t
#include <string.h> // memcpy

#include "base.h"

#include "printf.h"

#include <unistd.h>

#define Maxfmt 256

enum Radix { Radixdec,Radixoct,Radixhex,Radixbin };

enum Packed8 Fmt {
  Fmtinv,
  Flgws=1,
  Flgpls=2,
  Flgpad0=4,

  Fmtdot,
  Fmtast,

  Flgleft=8,

  // len mods
  Modh,Modl,ModL,
  Modj, // intmax_t
  Modz, // size_t
  Modt, // ptrdiff_t
  Modw, // wN

  Flgalt=16,

  ModD, // Decimal
  ModH,

  Flghr=32,

  // 0-9
  Dig0,Dig1,Dig2,Dig3,Dig4,Dig5,Dig6,Dig7,Dig8,Dig9,

  Flgsep=64,

  Fmtc,Fmts,Fmtm,

  Fmtd,Fmtu,Fmtb,FmtB,Fmto,Fmtx,FmtX,Fmtp,

  Fmtf=76,FmtF,Fmtg,FmtG,Fmte,FmtE,Fmta,FmtA,

  Fmtpct,

  Fmtn,

  Fmteof
};

enum Packed8 Cnv {
  Cnvinv,
  Cnvu,Cnvc,
  Cnve,
  Cnvs,Cnvp,Cnvn,Cnvm,

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
  if (x) *--end = (ub1)((x % 10) + '0');
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
  else if (x >= 1024u) { x1 = x >> 10; x2 = x; scale = 'k'; }
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

  if (x >= (1ul << 60)) { shift = 60; scale = 'E'; }
  else if (x >= (1ul << 50)) { shift = 50; scale = 'P'; }
  else if (x >= (1ul << 40)) { shift = 40; scale = 'T'; }
  else if (x >= (1ul << 30)) { shift = 30; scale = 'G'; }
  else {
    return Ucnv(end,(ub4)x);
  }
  x1 = (ub4)(x >> shift);
  x2 = (ub4)(x >> (shift - 10));
  return hrcnv(end,x1,x2,scale);
}

#if Mini_printf_float_formats > 0

// for %f
static ub1 *ullcnv_dot(ub1 *end,unsigned long long x,ub4 dotpos)
{
  const unsigned char *p;
  ub4 n = 0;

  if (x == 0) { *--end = '0'; return end; }

  while (x >= 10) {
    p = cnvtab + 2 * (x % 100);
    *--end = p[1]; if (++n == dotpos) *--end = '.';
    *--end = p[0]; if (++n == dotpos) *--end = '.';
    x /= 100;
  }
  if (x) *--end = (ub1)((x % 10) + '0');
  return end;
}

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

  if (frac2 >=10.0) { frac2 *= 0.1; iexp10++; }
  else if (frac2 <1.0) { frac2 *= 10.0; iexp10--; }

  *piexp10 = iexp10;

  return frac2;
}

static double tentab[17] = {1e16,1e15,1e14,1e13,1e12,1e11,1e10,1e9,1e8,1e7,1e6,1e5,1e4,1e3,100.0,10.0,1.0 };

// %f
static ub1 *fcnv(ub1 *end,double x,enum Fmt flags,ub2 prec)
{
  int exp,xexp,uexp,dotpos,digits;
  double mant,xmant;
  unsigned long long imant;

  mant = fpnorm(x,&exp);

  if (exp >= 0) {
    if (exp > 16) {
      xexp = exp - 16;
      exp -= xexp;
      memset(end - xexp,'0',xexp); end -= xexp;
    }
    dotpos = 16 - exp;
    digits = min(prec,dotpos);
    mant *= tentab[dotpos - digits];
    xmant = mant - floor(mant);
    imant = (unsigned long long)mant;
    if (xmant >= 0.5) imant++;
    end = ullcnv_dot(end,imant,digits);
    return end;
  } else { // exp < 0
    if (-exp <= prec) {
      if (exp < -16) {
        xexp = exp + 16;
        exp += xexp;
      }
      uexp = -exp;

      mant *= tentab[16 - prec];
      xmant = mant - floor(mant);
      imant = (unsigned long)mant;
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

// %e
static ub1 *ecnv(ub1 *end,double x,enum Fmt flags,ub2 prec)
{
  int exp,uexp;
  double mant,xmant;
  unsigned long long imant;
  bool neg;

  if (prec > 16) {
    uexp = prec - 16;
  } else uexp = 0;

  mant = fpnorm(x,&exp);
  mant *= tentab[16 - prec];
  xmant = mant - floor(mant);
  imant = (unsigned long long)mant;
  if (xmant >= 0.5) imant++;

  if (exp < 0) {
    neg = 1; exp = -exp;
  } else neg = 0;
  end = ucnv(end,exp);
  *--end = 'e';
  *--end = neg ? '-' : '+';

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

// %a
static ub1 *acnv(ub1 *end,double x,enum Fmt flags,ub2 prec,ub1 Case)
{
  int exp,uexp;
  double mant,mant2;
  unsigned long long imant;
  bool neg;

  if (prec > 16) {
    uexp = prec - 16;
  } else uexp = 0;

  mant = frexp(x,&exp);  // x = frac * 2^iexp
  if (exp < 0) { neg = 1; exp = -exp;
  } else neg = 0;
  end = hexcnv(end,exp,Case);
  *--end = 'p';
  *--end = neg ? '-' : '+';

  mant2= ldexp(mant,exp);
  imant = (unsigned long long)mant2;

  if (uexp > 1) {
    memset(end - uexp,'0',uexp);
    end -= uexp;
  }
  end = hexllcnv(end,imant,Case);
  if (prec == 0 && (flags & Flgalt) ) return end;
  end--;
  *end = end[1];
  end[1] = '.';
  return end;
}

#endif // Mini_printf_float_formats

static const enum Fmt fmtmap[128] = { // 'parse' format template
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

// writes into dst + pos, upto dst + dlen from fmt and args. Always zero-terminates. Returns #chars written
ub4 mini_vsnprintf(char *dst,ub4 pos,ub4 dlen,const char *fmt,va_list ap)
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
  enum Fmt flags,nxflags=0;
  ub1 sign;
  bool isneg,iszero;
  bool flgdon,dotseen;
  ub1 Case,casemsk;

  // args
  sb4 s4=0;
  long s8=0;
  ub4 u4=0;
  unsigned long u8=0;
  uintmax_t uj=0;
#if LLONG_MAX > LONG_MAX
  unsigned long long ull;
#endif

  double f8=0;
  long double lf8;
  int fpclas = 0;

  void *vp=nil;
  unsigned int *uip;
  const char *p8;
  char cc[2];

  // cnv buf
  ub1 *org,*org2;
  ub1 cnvbuf[Maxfmt];
  ub1 cnvbuf2[Maxfmt];
  ub1 *end2,*end = cnvbuf + Maxfmt;

  // misc
  int local_err = 0;
  ub4 pi=0;
  ub2 dig,grp;
  bool simple;

  unsigned char c;
  char d;

  ub4 len,xlen;
  ub4 sorg,padorg;
  ub4 n = 0;
  cchar *p = fmt;

  p = fmt;

  dst += pos;
  *dst = 0;

 while (pos + n + 2 < dlen) {

  c = (ub1)p[pi++];

// all conversions
 if (c == '%') {
  cnv = Cnvinv;
  prec = hi32;
  wid = hi32;
  mindig = hi32;
  modl = modh = modD = 0;
  mod = 0;
  flags = nxflags;
  nxflags = 0;
  rdx = Radixdec;
  sign = 0;
  flgdon = 0;
  dotseen = 0;
  Case = 0;
  simple = 1;

 do { // while cnv
  c = (ub1)p[pi++];

  if (c < 128) fx = fmtmap[c];
  else fx = Fmtinv;

  switch (fx) {
   // flags:
   case Flgleft: Fallthrough
   case Flgpls: case Flgws: case Flgalt: case Flgsep: case Flghr:
     simple = 0;
     flags |= fx; // check flag assignment
     break;

   // width and precision
   case Dig0: if (flgdon == 0) flags |= Flgpad0; flgdon = 1; Fallthrough
   case Dig1: case Dig2: case Dig3: case Dig4: case Dig5: case Dig6: case Dig7: case Dig8: case Dig9:
     flgdon = 1;

     u4 = fx - Dig0;
     if (dotseen) prec = prec * 10 + u4;
     else {
       if (wid == hi32) wid = u4;
       else wid = wid * 10 + u4;
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
       if (s4 >= 0) {
         prec = (ub4)s4;
         simple = 0;
       }
     } else {
       dotseen = 1;
       if (s4 < 0) { wid = (ub4)-s4; flags |= Flgleft; }
       else wid = (ub4)s4;
       if (wid > 1) simple = 0;
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
     while ( (c = p[pi]) >= '1' && c <= '9') { modw = (modw & hi16) * 10 + c - '0'; pi++; }
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
   case FmtA: case Fmta:; flags |= Flgalt; cnv = Cnve; break;
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
    if (prec == hi32) prec = 6;
    else if (prec > Maxfmt - 32) prec = Maxfmt - 32;
  }

  } while (fx && fx < Fmtc); // end at cnvspec

  if (p[pi] == '`') { // nonstandard human-readable ints
    if (cnv == Cnvu) { pi++; flags |= Flghr; }
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
    }
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

     else if (mod == Modj ) cnv = Cnvju; // jd

     if (prec == hi32) prec = 1;
     else {
       flags = flags & ~Flgpad0;
       if (prec > Maxfmt - 32) prec = Maxfmt - 32;
     }
     mindig = prec;

  } else if (cnv == Cnve) { // Cnvu

     sign = 1;
#if Mini_printf_float_formats > 0
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
     if (simple) {
       dst[n++] = (char)s4;
       cnv = Cnvinv;
     } else {
       *cc = (char)s4; cc[1] = 0;
       vp = cc; cnv = Cnvs; prec = hi32;
     }
   break;

   case Cnvu:    // %u %x %o %b
     u4 = va_arg(ap,unsigned int);
     iszero = (u4 == 0);
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
     if (sign && (long long)ull < 0) { ull = -(long long)ull; isneg = 1; }
     if (ull <= UINT_MAX) { u4 = (ub4)ull; cnv = Cnvu; }
     else if (ull <= ULONG_MAX) { u8 = (ub8)ull; cnv = Cnvlu; }
#endif
     break;

   case Cnvju: // %ju
     uj = va_arg(ap,uintmax_t);
     iszero = (uj == 0);
     if (sign && (long long)uj < 0) { uj = (unsigned long long)-(intmax_t)uj; isneg = 1; }
     if (uj <= hi32) { u4 = (ub4)uj; cnv = Cnvu; }
     else cnv = sizeof(long) == sizeof(long long) ? Cnvlu : Cnvllu;
     break;

   case Cnve:    // %f,e
   case Cnvle:
     if (cnv == Cnve) {
       f8 = va_arg(ap,double);
       fpclas = fpclassify(f8);
     } else {
       lf8 = va_arg(ap,long double);
       fpclas = fpclassify(lf8);
     }
     switch (fpclas) {
     case FP_INFINITE: vp = Case ? "INF" : "inf"; cnv = Cnvs; prec = hi32; break;
     case FP_NAN: vp = Case ? "NAN" : "nan"; cnv = Cnvs; prec = hi32; break;
     case FP_NORMAL:
     case FP_SUBNORMAL:
#if Mini_printf_float_formats == 0
     cnv = Cnvlu;
     u8 = (ub8)f8;
#endif
     break;
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

   case Cnvinv: break;
  } // get args

  // print
  if (iszero) { // Cnvu except %p
    flags = flags & ~Flgalt;
    if (prec == 0) {
      if (wid == hi32 || wid == 0) cnv = Cnvinv;
      else { cnv = Cnvs; vp = ""; }
    } else if (simple) { dst[n++] = '0'; cnv = Cnvinv; }
  }

  switch (cnv) {

   case Cnvu:
     if (rdx == Radixhex && u4 <= 9) flags = flags & ~Flgalt;
     if (iszero) *--org = '0';
     else {
       if (flags & Flghr) org = Ucnv(end,u4);
       else if (rdx == Radixdec) org = ucnv(end,u4);
       else if (rdx == Radixhex) org = hexcnv(end,u4,Case);
       else org = xcnv(end,u4,rdx,flags);
       if (simple && org + 1 == end) {
         if (isneg) dst[n++] = '-';
          dst[n++] = (char)*org; org = end;
       }
     }
   break;

   case Cnvp: u8 = (ub8)vp; Fallthrough
   case Cnvlu:
     if (flags & Flghr) org = Ulcnv(end,u8);
     else if (rdx == Radixdec) org = ulcnv(end,u8);
     else if (rdx == Radixhex) org = hexlcnv(end,u8,Case);
     else org = xlcnv(end,u8,rdx,flags);
   break;

   case Cnvllu:
   case Cnvju:
#if ULLONG_MAX > ULONG_MAX
     if (rdx == Radixdec) org = ullcnv(end,ull);
     else if (rdx == Radixhex) org = hexllcnv(end,ull,Case);
     else org = xllcnv(end,ull,rdx,flags);
#endif
   break;

   case Cnve:
   case Cnvle:
     if (fpclas == FP_ZERO) {
       if (fmtf == Fmte) { *--org = '0'; *--org = '0'; *--org = '+'; *--org = 'E' | casemsk; }
       else if (fmtf == Fmta) { *--org = '0'; *--org = '+'; *--org = 'P' | casemsk; }
       if (prec) { memset(end - prec,'0',prec); org -= prec; *--org = '.'; }
       else if (flags & Flgalt) *--org = '.';
       *--org = '0';
       break;
     }
      if (f8 < 0) { f8 = -f8; isneg = 1; }
      if (fmtf == Fmtg) {
        P = prec ? prec : 1;
        if (f8 >= 1e-4 && f8 < P) {
          fmtf = Fmtf; prec = P;
        } else {
          fmtf = Fmte; prec = P - 1;
        }
      }
#if Mini_printf_float_formats == 0
#else
      if  (fmtf == Fmtf) org = fcnv(end,f8,flags,(ub2)prec);
      else if (fmtf == Fmte) org = ecnv(end,f8,flags,(ub2)prec);
      else org = acnv(end,f8,flags,(ub2)prec,Case);
#endif
      break;

   case Cnvn:

     if (modl == 1) *(unsigned long *)vp = n;
     else if (modl) *(unsigned long long *)vp = n;
     else if (modh == 1) *(unsigned short *)vp = (ub2)n;
     else if (modh) *(unsigned char *)vp = (ub1)n;
     else {
       uip = (unsigned int *)vp;
       if (*uip == Printf_dot) nxflags = Flgsep; // comnpatibility hack to get %'x and %'b
       else *uip = n;
     }
     break;

   case Cnvs:
     if (vp == nil) vp = "(nil)";
     p8 = (const char *)vp;
     slen = 0;
     if (wid != hi32 && wid) {
       sndx = 0;
       while (slen < prec && slen < wid && p8[sndx++]) slen++; // get length
       if (wid <= slen) wid = hi32;
     }

     if (wid != hi32 && wid > slen) { // pad
       if ((flags & Flgleft)) { sorg = n; padorg = n + slen; }
       else { sorg = n + wid - slen; padorg = n; }
       memcpy(dst + sorg,p8,slen);
       memset(dst + padorg,' ',wid - slen);
       n += wid;
     } else {
       while (prec && (d = *p8++ ) ) { dst[n++] = d; prec--; }
     }
     break;

     case Cnvinv: case Cnvc: case Cnvm: break;
  } // print each cnv

  len = min( (ub4)(end - org),dlen - n - pos);
  if (len) {
    if (mindig != hi32 && mindig > len) pad0 = mindig;
    else if ( (flags & Flgpad0) && wid != hi32) { pad0 = wid; wid = hi32; }
    else pad0 = 1;
    xlen = len;
    if (isneg || (flags & (Flgws | Flgpls) )) xlen++;
    if ( (flags & Flgalt) && rdx >= Radixhex) xlen += 2;

    while (org > cnvbuf + 6 && pad0-- > xlen) *--org = '0'; // minimum number of digits for int or pad with zero for int/float

    // digit grouping: 3 for decimal, 4 for hex and bin
    if  (flags & Flgsep) {
      org2 = org; end2 = end;
      end = org = cnvbuf2 + Maxfmt;
      dig = 0;
      if (rdx == Radixdec) { grp = 3; d = '.'; }
      else { grp = 4; d = '_'; }
      do {
        if (dig == grp) { *--org = d; dig = 1; }
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
    if (wid != hi32 && wid > len && !(flags & Flgleft)) { // pad
      wid -= len;
      memset(dst + n,' ',wid); n += wid;
      len = min( (ub4)(end - org),dlen - n - pos);
    }
    memcpy(dst + n,org,len); n += len;
    if (wid != hi32 && wid > len && (flags & Flgleft)) {
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

#include <unistd.h>

#define Testbuf 1024

static void wrch(char c)
{
  write(1,&c,1);
}

static bool Printf(3,4) check(int line,cchar *exp,cchar *fmt,...)
{
  va_list ap;
  ub1 buf[Testbuf];
  ub1 tmp[16];
  ub1 *end;
  ub4 pos;
  size_t explen = strlen(exp);
  char sq = '\'';

  va_start(ap,fmt);
  memset(buf,' ',Testbuf);
  pos = mini_vsnprintf((char *)buf,0,Testbuf,fmt,ap);
  va_end(ap);

  if (memcmp(exp,buf,explen+1) == 0 && pos == explen) return 0;

  // do not use printf to format - we know it is broken if we come here
  end = ucnv(tmp + 16,line);
  write(1,end,tmp + 16 - end);
  wrch(' ');

  end = ucnv(tmp + 16,pos);
  write(1,end,tmp + 16 - end);
  wrch(' ');

  end = ucnv(tmp + 16,explen);
  write(1,end,tmp + 16 - end);
  wrch(' ');

  write(1,"exp: '",6); write(1,exp,explen);  wrch(sq); wrch('\n');
  write(1,"     ",5);
  write(1,"act: '",6); write(1,buf,pos);  wrch(sq); wrch('\n');
  wrch('\n');
  return 1;
}

static int test(void)
{
  bool rv = 0;
  char buf[32];
  ub4 pos;

  pos = snprintf_mini(buf,0,16,"123 %s 456","12345678901234567890");
  if (pos != 16 || memcmp(buf,"123 123456789012",16)) {
    wrch('"'); write(1,buf,pos); wrch('"'); wrch('\n'); rv = __LINE__;
  }

  rv |= check(__LINE__,"abc 'ghi' def","abc '%s' def","ghi");

  rv |= check(__LINE__,"abc '1234567890' def","abc '%.10s' def","123456789012345678901234567890123456789");

  rv |= check(__LINE__,"abc 'gh' def","abc '%.2s' def","ghi");
  rv |= check(__LINE__,"abc 'ghi             ' def","abc '%-16s' def","ghi");

  // rv |= check(__LINE__,"abc '34' def","abc '%w8x' def",0x1234);

  rv |= check(__LINE__,"101","%b",5);
  rv |= check(__LINE__,"0101","%llb",0x123456789abcdef0ull);

  return  rv;

  rv |= check(__LINE__,"abc '1.2000' ghi","abc '%f' ghi",1.2);
  rv |= check(__LINE__,"abc '1.2000' ghi","abc '%.18f' ghi",1.2);
  rv |= check(__LINE__,"abc '12.300' ghi","abc '%f' ghi",12.3);
  rv |= check(__LINE__,"abc '123.45600' ghi","abc '%f' ghi",123.456);

  rv |= check(__LINE__,"abc '123.45600' ghi","abc '%f' ghi",2.34567890123456789e+14);
  rv |= check(__LINE__,"abc '123.45600' ghi","abc '%f' ghi",2.34567890123456789e+32);
  rv |= check(__LINE__,"abc '123.45600' ghi","abc '%f' ghi",2.34567890123456789e+64);

  rv |= check(__LINE__,"abc '123.45600' ghi","abc '%f' ghi",2.34567890123456789e-8);
  rv |= check(__LINE__,"abc '123.45600' ghi","abc '%.16f' ghi",2.34567890123456789e-8);
  rv |= check(__LINE__,"abc '123.45600' ghi","abc '%f' ghi",2.34567890123456789e-14);
  rv |= check(__LINE__,"abc '123.45600' ghi","abc '%f' ghi",2.34567890123456789e-30);

  rv |= check(__LINE__,"abc '123.45600' ghi","abc '%e' ghi",2.34567890123456789e+64);

  rv |= check(__LINE__, "bla 1.2 3 4 -5 6e7 0.89 1e5 11" ,"bla %a %b %c %d %e %f %g %hu \n ", 1.2, 3, '4', -5, 6e7, 0.89, 1e5, 11);

  return rv;
}

int main(int argc,char *argv[])
{
  return test();
}
#endif
