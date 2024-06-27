/* test.c - yalloc tests

   This file is part of yalloc, yet another memory allocator with emphasis on efficiency and compactness.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <unistd.h> // write

#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h> // long_max

#include "stdlib.h"

#include "malloc.h"

#include "base.h"

#include "printf.h"

#define Logfile Ftest

#if 0
static void oswrite(int fd,const char *buf,size_t len)
{
  write(fd,buf,len);
}
#endif

#include "os.h"

#include "config.h"

#undef Yal_diag_init
#define Yal_diag_init "yal_test_diag.cfg"

struct st_heap {
  ub4 id;
  char errmsg[Diag_buf];
};
typedef struct st_heap heap;
static heap tstheap;

#undef Yal_enable_log
#define Yal_enable_log 1

#define Cold
#include "diag.h"

static size_t atox(cchar *s)
{
  size_t x = 0;
  char c;

  while ( (c = *s++)  ) {
    if (c == '.') continue;
    x <<= 4;
    if (c >= 'a') x += c - 'a' + 10;
    else x += c - '0';
  }
  return x;
}

static size_t atoul(cchar *s)
{
  size_t x = 0;
  char c;

  if (*s == '0' && s[1] == 'x') return atox(s + 2);

  while ( (c = *s++) ) {
    if (c == '.') continue;
    else x = x * 10 + c - '0';
  }
  return x;
}

static bool haschr(cchar *s,char c) { return strchr(s,c) != nil; }

// wikipedia xorshift
static ub8 xorshift64star(void)
{
  static ub8 x = 0x05a3ae52de3bbf0aULL;

  x ^= x >> 12; // a
  x ^= x << 25; // b
  x ^= x >> 27; // c
  return (x * 2685821657736338717ULL);
}

static ub8 rndstate[ 16 ];

static ub8 xorshift1024star(void)
{
  static int p;

  ub8 s0 = rndstate[p];
  ub8 s1 = rndstate[p = ( p + 1 ) & 15];
  s1 ^= s1 << 31; // a
  s1 ^= s1 >> 11; // b
  s0 ^= s0 >> 30; // c
  rndstate[p] = s0 ^ s1;

  return (ub4)(rndstate[p] * 1181783497276652981ULL);
}

static size_t rnd(size_t range)
{
  size_t r = (size_t)xorshift1024star();
  return r % range;
}

static struct yal_stats stats;

static void clrstats(void)
{
  yal_mstats(&stats,0,0,1,"test");
}

static int haserr(size_t exp,void *p,size_t x,int line)
{
  size_t errs = yal_mstats(nil,0,1,0,"test");
  if (errs != exp) { ylog(Ltest,"test.c:%d ptr %p x %zx.%zu expected %zu, found %zu",line,p,x,x,exp,errs) return line; }
  return 0;
}

static int slabs(cchar *cmd,size_t from,size_t to)
{
  size_t len;
  void *p,*r,*q = nil;
  size_t ip,iq;
  bool quit = haschr(cmd,'!');

  for (len = from; len < to; len++) {
    p = malloc(len);
    if (quit && p == nil) return __LINE__;
    q = malloc(len);
    if (quit && q == nil) return __LINE__;
    if (len && p && q == p) { ylog(Ltest,"len %zu p %p q %p",len,p,q) return __LINE__; }
    if (len == 0 && q != p) { ylog(Ltest,"len 0 p %p q %p",p,q) return __LINE__; }
    iq = (size_t)q;
    ip =(size_t)p;
    if (ip > iq && ip < iq + len) return __LINE__;
    if (ip < iq && ip > iq - len) return __LINE__;
    if (malloc_usable_size(p) < len) return __LINE__;
    else if (malloc_usable_size(p) >= len * 2) return __LINE__;
    r = malloc(len / 4);
    if (quit && r == nil) return __LINE__;
  }
  free(q);
  return 0;
}

static int fre2(size_t small,size_t large)
{
  ub4 i;
  void *s,*l,*p;
  void *ps[128];
  int rv;

  s = malloc(small);
  l = malloc(large);
  free(s);
  yal_options(Yal_log,0x3c);
  free(s);

  free(l);
  free(l);

  if (haserr(2,s,0,__LINE__)) return __LINE__;

  for (i = 0; i < 128; i++) ps[i] = malloc(small);
  free(ps[0]);
  free(ps[0]);
  free(ps[100]);
  free(ps[100]);
  yal_options(Yal_log,0);
  rv = haserr(4,s,1,__LINE__);
  return rv;
}

static int bist(size_t cnt,size_t hilen,size_t iter)
{
  size_t it,n,len,newlen;
  size_t loadr,hiadr,adrlen,ip;
  size_t nofres;
  ub4 als,fres,al,fre;
  void *p;
  ub4 fln;
  heap *hb = &tstheap;

  als = cnt >> 32;
  fres = cnt & hi32;

  ylog(Ltest,"iter %zu als %u fres %u len %zu",iter,als,fres,hilen);
  for (it = 0; it < iter; it++) {
    nofres = 0;
    for (al = 0; al < als; al++) {
      len = rnd(hilen);
      p = malloc(len);
      if (p == nil) return __LINE__;
    }
    // if (ooms) ylog(Ltest,"iter %zu oom %zu",it,ooms);

    fln = 0; // bist_check(0);
    if (fln) {
      error2(nil,Ltest,fln,"test bist failed (%u.%u)",fln >> 16,fln & hi16);
      return __LINE__;
    }
    yal_mstats(&stats,0,0,0,"test");
    loadr = stats.loadr;
    hiadr = stats.hiadr;
    if (loadr == 0 || hiadr <= loadr) return Fln;
    adrlen = hiadr - loadr;

    yal_options(Yal_log,0x3c);
    for (fre = 0; fre < fres; fre++) {
      ip = loadr + rnd(adrlen);
      p = (void *)ip;
      if (malloc_usable_size(p) == 0) { nofres++; continue; }
      if ( (fre & 0xf) == 0) {
        newlen = rnd(hilen);
        p = realloc(p,newlen);
      } else free(p);
    }
    yal_options(Yal_log,0);

    if (nofres) {
      ylog(Ltest,"iter %zu nofre %zu",it,nofres);
      if (nofres == fres) return __LINE__;
    }
    fln = 0; // bist_check(1);
    if (fln) {
      error2(nil,Ltest,fln,"test bist failed (%u.%u)",fln >> 16,fln & hi16);
      return __LINE__;
    }
  }
  return 0;
}

static int allfre(cchar *cmd,size_t cnt,size_t len,size_t iter)
{
  size_t i,n;
  void *p;
  void *ps[256];
  int rv;
  ub4 fln;
  heap *hb = &tstheap;

  p = malloc(len);
  ylog(Ltest,"allfree cnt %zu size %zu iters %zu first %p",cnt,len,iter,p)
  memset(ps,0,sizeof(ps));
  if (iter == 0) iter = 256;

 for (n = 0; n < iter; n++) {
  for (i = 0; i < cnt; i++) {
    p = malloc(len);
    if (p == nil) { ylog(Ltest,"%s: nil p at cnt %zu",cmd,i) return __LINE__; }
    if (i < 256) ps[i] = p;
  }
  fln = 0; // bist_check(2);
  if (fln) {
    error2(nil,Ltest,fln,"test %s failed (%u.%u)",cmd,fln >> 16,fln & hi16);
    return __LINE__;
  }
  for (i = 0; i < min(cnt,256); i++) {
    p = ps[i];
    free(p);
    if (haserr(0,p,i,__LINE__)) return __LINE__;
  }
  fln = 0; // bist_check(3);
  if (fln) {
    error2(nil,Ltest,fln,"test %s failed (%u.%u)",cmd,fln >> 16,fln & hi16);
    return __LINE__;
  }
 }
  return 0;
}

static int do_test(cchar *cmd,size_t arg1,size_t arg2,size_t arg3,bool dostat)
{
  int rv = __LINE__;
  void *q = nil,*p = nil,*a,*b,*c;
  char *pc;
  size_t n;
  ub4 i,j,ii;
  ub4 fln;
  // ub4 ord;
  size_t nlim = arg1;
  size_t ilim = arg2;
  static void *ps[65536];

  for (i = 0; i < 16; i++) rndstate[i] = xorshift64star();

  if (haschr(cmd,'s')) { rv = slabs(cmd,arg1,arg2); if (rv) return rv; }
//  if (haschr(cmd,'b')) { rv = blocks(arg1,arg2); if (rv) return rv; }
  if (haschr(cmd,'a')) { rv = allfre(cmd,arg1,arg2,arg3); if (rv) return rv; }
  if (haschr(cmd,'2')) { rv = fre2(arg1,arg2); if (rv) return rv; }
  if (haschr(cmd,'b')) {
    rv = bist(arg1,arg2,arg3);
    if (rv) return rv;
  }

  if (dostat) calloc(0,Yal_trigger_stats_clear);
  clrstats();

#if 0
  for (n = 16; n < nlim; n++) {
    ylog(Ltest,"len %zu",n)
    for (i = 0; i < ilim; i++) {
      q = p;
      p = malloc(n);
      if (p == nil) { ylog(Ltest,"nil p for %zu",n) return __LINE__; }
      a = malloc(n);
      if (a == nil) { ylog(Ltest,"nil a for %zu",n) return __LINE__; }
      b = malloc(n);
      if (b == nil) { ylog(Ltest,"nil b for %zu",n) return __LINE__; }
      c = malloc(n);
      if (c == nil) { ylog(Ltest,"nil c for %zu",n) return __LINE__; }
      // ylog(Ftest,"a %p i %u",a,i);

      if (haserr(0,p,__LINE__)) return __LINE__;

      free(a);
      if (haserr(0,p,__LINE__)) return __LINE__;

      free(b);
      if (haserr(0,p,__LINE__)) return __LINE__;

      free(c);
      if (haserr(0,p,__LINE__)) return __LINE__;
    }
  }
#endif

  return 0;

}

static const char usage[] = "usage: test  cmds args\n\ns - slabs\na = all\n2 = double free\nS stats\n";

int main(int argc,char *argv[])
{
  int rv;
  size_t arg1 = 0,arg2 = 0,arg3 = 0;
  cchar *cmd = "";
  bool dostat;

  if (argc < 2) {
    oswrite(1,usage,sizeof(usage));
    return 1;
  }

  diag_init();

  if (argc == 100) {
    oom(nil,0,Ltest,0,0);
    free2(nil,0,Ltest,0,nil,0,"test");
  }

  if (argc > 1 && strcmp(argv[1],"--") == 0) { argc--; argv++; }
  if (argc > 1) cmd = argv[1];
  if (argc > 2) arg1 = atoul(argv[2]);
  if (argc > 3) arg2 = atoul(argv[3]);
  if (argc > 4) arg3 = atoul(argv[4]);

  dostat = (strchr(cmd,'S') != nil);
  ylog(Ltest,"stat %u",dostat)

  ylog(Ltest,"%s %zu.%zx %zu.%zx %zu.%zx",cmd,arg1,arg1,arg2,arg2,arg3,arg3)

  rv = do_test(cmd,arg1,arg2,arg3,dostat);
  if (rv) { ylog(Ltest,"test error on line %d",rv); }
  else { ylog(Ltest,"test %s ok",cmd); }

  return rv;
}
