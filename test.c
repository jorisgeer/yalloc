/* test.c - yalloc tests

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

   Semi-portable, unix-like systems only with pthreads
*/


#define _POSIX_C_SOURCE 199309L

#include <unistd.h> // write, nanosleep

#include <errno.h>

#include <pthread.h>

#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h> // long_max

#include "stdlib.h"
#include "malloc.h"

#include "base.h"

#include "printf.h"
#include "os.h"

#define Yal_enable_tag 1

#define L __LINE__

static const char usagemsg[] = "\nusage: test  [opts] cmds args\n\n\
-s   show total stats at end\n\
-S   as above plus per thread end\n\n\
s - slabs lolen hilen count iter\n\
a = all\n\
2 = double free\n\
x - xfree #threads list of 'a' tid1 tid2 len count or 'f' tid1 tid2 len count\n\
m - manual list of  'a' len count or 'f' from to\n\
\n";

static bool dostat,dotstat;

static const int Error_fd = 1;
static const int Log_fd = 1;

static unsigned long mypid;

#if Yal_enable_tag
 #define malloc(len) yal_alloc( (len), L) // include tag
 #define free(p) yal_free( (p), L)
 #define realloc(p,len) yal_realloc( (p), (len), L)
#endif

#include "atom.h"

#include "util.h"

static bool haschr(cchar *s,char c) { return strchr(s,c) != nil; }

// wikipedia xorshift or https://prng.di.unimi.it

static ub8 g_state[17]; // 0-15 = state  16 = p

static void inixor(ub8 *state)
{
  state[16] = 0; // p
}

static ub8 xorshift64star(void)
{
  ub8 x = 0x05a3ae52de3bbf0aULL;

  x ^= x >> 12; // a
  x ^= x << 25; // b
  x ^= x >> 27; // c
  //coverity[overflow_const]
  return (x * 2685821657736338717ULL);
}

static ub8 xorshift1024star(ub8 *state)
{
  ub4 p = (ub4)state[16];

  ub8 s0 = state[p];
  ub8 s1 = state[p = ( p + 1 ) & 15];
  s1 ^= s1 << 31; // a
  s1 ^= s1 >> 11; // b
  s0 ^= s0 >> 30; // c
  state[p] = s0 ^ s1;

  //coverity[overflow_const]
  return (ub4)(state[p] * 1181783497276652981ULL);
}

static size_t rnd(size_t range,ub8 *state)
{
  size_t r = (size_t)xorshift1024star(state);
  return r % range;
}

static ub4 pick(ub4 mask,ub8 *state)
{
  size_t r = (size_t)xorshift1024star(state);

  return (r & mask) == mask;
}

enum Loglvl { Fatal,Assert,Error,Warn,Info,Trace,Verb,Debug,Nolvl };
static cchar * const lvlnames[Nolvl + 1] = { "Fatal","Assert","Error","Warn","Info","Trace","Verb","Debug"," " };

static void tlog(int line,enum Loglvl lvl,cchar *fmt,va_list ap)
{
  char buf[1024];
  ub4 pos = 0,len = 1022;

  pos = snprintf_mini(buf,0,len,"   yal/test.c:%3d ",line);
  pos += snprintf_mini(buf,pos,len,"%-5lu %-4u %-3u %c test      ",mypid,0,0,*lvlnames[lvl]);

  pos += mini_vsnprintf(buf,pos,len,fmt,ap);
  buf[pos++] = '\n';
  oswrite(lvl > Error ? Log_fd : Error_fd,buf,pos,__LINE__);
}

extern Noret void exit(int status);

static Printf(2,3) Noret void fatal(int line,cchar *fmt,...)
{
  va_list ap;

  va_start(ap,fmt);
  tlog(line,Fatal,fmt,ap);
  va_end(ap);
  exit(1);
}

static Printf(2,3) int error(int line,cchar *fmt,...)
{
  va_list ap;

  va_start(ap,fmt);
  tlog(line,Error,fmt,ap);
  va_end(ap);
  return line;
}

static Printf(2,3) ub4 warning(int line,cchar *fmt,...)
{
  va_list ap;

  va_start(ap,fmt);
  tlog(line,Warn,fmt,ap);
  va_end(ap);
  return 0;
}

static Printf(2,3) ub4 info(int line,cchar *fmt,...)
{
  va_list ap;

  va_start(ap,fmt);
  tlog(line,Info,fmt,ap);
  va_end(ap);
  return 0;
}

static void showstats(int line,bool all,cchar *desc)
{
  struct yal_stats stats;
  ub4 opts = Yal_stats_detail | Yal_stats_state | Yal_stats_print; // | Yal_stats_cfg;

  if (dostat == 0) return;
  if (all) opts |= Yal_stats_totals;
  yal_mstats(&stats,opts,(ub4)line,desc);
}

static int haserr(size_t exp,void *p,size_t x,int line)
{
  size_t errs = yal_mstats(nil,0,(ub4)line,"test");
  if (errs != exp) return error(L,"test.c:%d ptr %p x %zx.%zu expected %zu, found %zu",line,p,x,x,exp,errs);
  return 0;
}

static void diadis(ub4 x)
{
  yal_options(Yal_diag_enable,x,0);
}

static void diaena(ub4 x)
{
  yal_options(Yal_diag_enable,x,1);
}

//coverity[-alloc]
static int slabs(cchar *cmd,size_t from,size_t to,size_t cnt,size_t iter)
{
  size_t len,alen,i,c;
  void *p,*r,*q = nil;
  size_t ip,iq;
  bool quit = haschr(cmd,'!');

  if (to == 0) to = from + 1;
  if (iter == 0) iter = 1;
  if (cnt == 0) cnt = 1;

  alen = 0;
  for (len = from; len < to; len++) alen += 2 * len + len / 4;

  info(L,"malloc(%zu` .. %zu`) * %zu iter %zu sum %zu`",from,to,cnt,iter,alen * cnt * iter);
  for (i = 0; i < iter; i++) {
    for (len = from; len < to; len++) {
     for (c = 0; c < cnt; c++) {
      p = malloc(len);
      if (quit && p == nil) return L;
      q = malloc(len);
      if (quit && q == nil) return L;
      if (len && p && q == p) return error(L,"len %zu p %p q %p",len,p,q);
      if (len == 0 && q != p) return error(L,"len 0 p %p q %p",p,q);
      iq = (size_t)q;
      ip =(size_t)p;
      if (ip > iq && ip < iq + len) return L;
      if (ip < iq && ip > iq - len) return L;
      alen = malloc_usable_size(p);
      if (alen < len) return L;
      else if (len >= 16 && alen >= len * 4) return error(L,"iter %zu block %zx len %zu has allocated %zu",iter,ip,len,alen);
      r = malloc(len / 4);
      if (quit && r == nil) return L;
      if (pick(15,g_state)) free(q);
    } // cnt
   } // len
  } // iter
  return 0;
}

#define Pointers 65536
#define Tids 64

struct xinfo {
  _Atomic ub4 lock;
  ub4 tid;
  _Atomic ub4 cmd;
  _Atomic size_t len;
  _Atomic size_t cnt;
//  ub4 cmd;
//  size_t len;
//  size_t cnt;
  ub4 mode;
  size_t pos;
  size_t iter;
  void * _Atomic ps[Pointers];
};

static void waitus(ub8 usec)
{
  struct timespec ts;
  int rv,ec;

  memset(&ts,0,sizeof(ts));
  ts.tv_nsec = (long)(usec * 1000ul);
  rv = nanosleep(&ts,nil);
  if (rv == -1) {
    ec = errno;
    warning(L,"nanosleep returned %d %m",ec);
  }
}

static int getlock(int ln,struct xinfo *ap,ub4 from,ub4 to)
{
  ub4 iter = Hi16;
  ub4 f = from;

  info(ln,"tid %u %u -> %u = %u",ap->tid,from,to,Atomget(ap->lock,Moacq));
  while (--iter && Cas(ap->lock,from,to) == 0) { waitus(1000ul ); from = f; }
  info(ln,"tid %u %u -> %u = %u in %u it",ap->tid,from,to,Atomget(ap->lock,Moacq),Hi16 - iter);
  return iter ? 0 : L;
}

//coverity[-alloc]
static void *xfree_thread(void *arg)
{
  struct xinfo *ap = (struct xinfo *)arg;
  void *p;
  size_t len,ofs,cnt,iter = 256;
  ub4 c,cmd;
  ub4 tid = ap->tid;
  ssize_t rv;
  void * _Atomic * ps;

  info(L,"tid %u start",tid);

  do {
    rv = getlock(L,ap,2,3);
    if (rv) break;

    cmd = Atomget(ap->cmd,Moacq);
    info(L,"tid %u cmd %u",tid,cmd);
    ps = ap->ps;

    // nop
    if (cmd == 0) {
      waitus(1000ul * (tid + 1));

    // alloc
    } else if (cmd == 1) {
      cnt = Atomget(ap->cnt,Moacq);
      len = Atomget(ap->len,Monone);
      info(L,"alloc %zu * %zu",cnt,len);
      for (c = 0; c < cnt; c++) {
        p = malloc(len);
        // info(L,"alloc %zu = %p",len,p);
        if (c >= Pointers) continue;
        Atomseta(ps + c,p,Morel);
      }

    // free
    } else if (cmd == 2) {
      ofs = Atomget(ap->len,Moacq);
      cnt = Atomget(ap->cnt,Moacq);
      for (c = 0; c < cnt; c++) {
        if (ofs + c >= Pointers) break;
        p = Atomgeta(ps + ofs + c,Moacq);
        // info(L,"tid %u free %zx at %u",tid,(size_t)p,c);
        free(p);
      }
    } else if (cmd == 3) { // realloc todo

    } else if (cmd == 9) { // exit
      rv = 0;
      if (dotstat) showstats(L,0,"xfree");
      rv = getlock(L,ap,3,4);
      if (rv) break;
      pthread_exit( (void *)rv);
    }

    rv = getlock(L,ap,3,4);
    if (rv) break;
    info(L,"tid %u 4",tid);

  } while (--iter);

  error(L,"tid %u timeout",tid);
  rv = L;
  return (void *)rv;
}

/* manual remote free tester
 list of commands:
 - a .tid1. .tid2. .len. .count. -> allocate .count. blocks of .len. in .tid1., export to '.id2.
 - f .tid1. .tid2. .pos. .count.  -> free .count. blocks from .pos. as allocated
 */
//coverity[-alloc]
static int xfree(cchar *cmd,size_t tidcnt,int argc,char *argv[])
{
  char *arg;
  size_t cnt=0,len=0,c,pos;
  int rv;
  ub4 tid,xid=0;
  ub4 loc;
  pthread_t tids[Tids];
  struct xinfo *a1,*a2;
  static struct xinfo infos[Tids];
  void *retval;
  void *p;
  void * _Atomic * ps1;
  void * _Atomic * ps2;

  if (tidcnt ==  0) return L;

  tidcnt = min(tidcnt,Tids);

  info(L,"xfree tidcnt %zu cmd %s",tidcnt,cmd);
  memset(infos,0,sizeof(infos));

  for (tid = 1; tid <= tidcnt; tid++) {
    a1 = infos + tid;
    a1->tid = tid;
    Atomset(a1->lock,0,Morel);
    rv = pthread_create(tids + tid,nil,xfree_thread,(void *)(infos + tid));
    if (rv) return L;
  }

  do {
    if  (argc > 4) {
      arg = *argv++;
      tid = atou(*argv++);
      xid = atou(*argv++);
      len = atoul(*argv++);
      cnt = atoul(*argv++);
      argc -= 5;
    } else {
      arg = "z";
      tid = 1;
      argc = 0;
    }
    a1 = infos + tid;
    rv = getlock(L,a1,0,1);
    if (rv) return rv;

    Atomset(a1->len,len,Morel);
    Atomset(a1->cnt,cnt,Morel);

    if (*arg == 'a') { // alloc
      a2 = infos + xid;
      ps2 = a2->ps;
      pos = a2->pos;
      if (tid == 0) { // on main heap
        info(L,"alloc %zu blocks of len %zu on main heap",cnt,len);
        for (c = 0; c < cnt; c++) {
          p = malloc(len);
          if (c + pos >= Pointers) continue;
          Atomseta(ps2 + c + pos,p,Morel);
        }
        continue;
      }
      info(L,"alloc %zu blocks of len %zu on heap %u for %u",cnt,len,tid,xid);
      Atomset(a1->cmd,1,Morel);

    } else if (*arg == 'f') { // free
      ps1 = a1->ps;
      if (tid == 0) { // on main heap
        info(L,"free %zu bloks in main",cnt);
        for (c = 0; c < cnt; c++) {
           if (c + len >= Pointers) break;
           p = Atomgeta(ps1 + len + c,Moacq);
           free(p);
        }
        continue;
      }
      info(L,"free %zu blocks in heap %un",cnt,tid);
      Atomset(a1->cmd,2,Morel);

    } else if (*arg == 'z') {
      Atomset(a1->cmd,9,Morel);

    } else return L;

    rv = getlock(L,a1,1,2);
    if (rv) return rv;

    rv = getlock(L,a1,4,0);
    if (rv) return rv;

    if (*arg == 'a') { // alloc, copy ptrs
      cnt = min(cnt,Pointers);
      a2 = infos + xid;
      pos = a2->pos;
      ps1 = a1->ps;
      ps2 = a2->ps;
      for (c = 0; c < cnt; c++) {
        if (c + pos>= Pointers) break;
        p = Atomgeta(ps1 + c,Moacq);
        Atomseta(ps2 + pos + c,p,Morel);
      }
      a2->pos = pos + cnt;
    }

  } while (argc);

  // wait for threads to end
  for (tid = 1; tid <= tidcnt; tid++) {
    info(L,"wait for tid %u",tid);
    a1 = infos + tid;
    if (Atomget(a1->cmd,Moacq) == 9) continue;
    loc = Atomget(a1->lock,Moacq);
    if (loc !=0 && loc != 4) {
      rv = getlock(L,a1,4,0);
      if (rv) return rv;
      loc = 0;
    }
    rv = getlock(L,a1,loc,1);
    if (rv) return rv;
    Atomset(a1->cmd,9,Morel);
    rv = getlock(L,a1,1,2);
    if (rv) return rv;
    rv = getlock(L,a1,4,0);
    if (rv) return rv;
  }

  for (tid = 1; tid <= tidcnt; tid++) {
    waitus(1000ul);
    rv = pthread_join(tids[tid],&retval);
    if (rv) {
      warning(L,"thread %u exit sattus %d",tid,rv);
      return L;
    }
    rv = (int)(size_t)retval;
    if (rv) {
      warning(L,"thread %u returned %d",tid,rv);
      return rv;
    }
  }
  waitus(1000);

  // yal_mstats(nil,0xff,(ub4)L,"xfree");

  return 0;
}

//coverity[-alloc]
static void *mt_alfre_thread(void *arg)
{
  struct xinfo *ap = (struct xinfo *)arg;
  ub4 tid = ap->tid;
  size_t cnt,len,iter,l,c,cc,pos,pp,pe;
  ub4 mode,alfre;
  void *p;
  void * _Atomic * ps;
  ssize_t rv = 0;
  ub8 state[18];

  inixor(state);
  for (l = 0; l < 16; l++) state[l] = xorshift64star();

  iter = ap->iter;
  cnt = Atomget(ap->cnt,Moacq);
  cnt = max(cnt,1);
  len = Atomget(ap->len,Monone);
  mode = ap->mode;
  ps = ap->ps;

  info(L,"+thread %u cnt %zu len %zu iter %zu mode %u",tid,cnt,len,iter,mode);

  pos = 0;
  while (iter) {
    iter--;
    alfre = pick(3,state);
    info(L,"tid %u alfre %u",tid,alfre);
    if (alfre) {
      cc = rnd(cnt,state);
      l = rnd(len,state);
      for (c = 0; c < cc; c++) {
        p = malloc(l);
        if (pos >= Pointers) continue;
        Atomseta(ps + pos,p,Morel);
        pos++;
      }
    } else {
      pp = rnd(max(pos,1),state);
      pe = min(pos,rnd(cnt,state));
      while (pp < min(pe,Pointers)) {
        p = Atomgeta(ps + pp,Moacq);
        free(p);
        // ap->ps[pp] = nil;
        pp++;
      }
    }
    if (pick(0x3f,state)) {
      for (pp = 0; pp < pos; pp++) {
        p = Atomgeta(ps + pp,Moacq);
        free(p);
      }
      // memset(ap->ps,0,pos * sizeof(void *));
      pos = 0;
    }
  } // iter
  info(L,"-thread %u cnt %zu len %zu",ap->tid,cnt,len);
  waitus(1000ul * tid);
  pthread_exit( (void *)rv);
}

// tid, len, cnt, iter, mode
static int mt_alfre(cchar *cmd,size_t tidcnt,size_t iter2,int argc,char *argv[])
{
  size_t cnt,len,iter,i;
  ub4 mode;
  int rv;
  ub4 tid;
  pthread_t tids[64];
  struct xinfo *a1;
  static struct xinfo infos[64];
  void *retval;

  if (tidcnt ==  0) return L;

  tidcnt = min(tidcnt,63);

  info(L,"mt_alfre tidcnt %zu iter %zu cmd %s",tidcnt,iter2,cmd);
  memset(infos,0,sizeof(infos));

  while (argc > 3) {
    tid = atou(*argv++);
    len = atoul(*argv++);
    cnt = atoul(*argv++);
    iter = atoul(*argv++);
    mode = atou(*argv++);
    argc -= 5;
    cnt = max(cnt,1);
    info(L,"+thread %u cnt %zu len %zu iter %zu mode %u",tid,cnt,len,iter,mode);
    a1 = infos + tid;
    Atomset(a1->cnt,cnt,Morel);
    Atomset(a1->len,len,Morel);
    a1->iter = iter;
    a1->mode = mode;
  }

 for (i = 0; i < iter2; i++) {

  for (tid = 1; tid <= tidcnt; tid++) {
    a1 = infos + tid;
    a1->tid = tid;
    rv = pthread_create(tids + tid,nil,mt_alfre_thread,(void *)a1);
    if (rv) return L;
  }

  for (tid = 1; tid <= tidcnt; tid++) {
    waitus(1000ul);
    rv = pthread_join(tids[tid],&retval);
    if (rv) {
      warning(L,"thread %u exit sattus %d",tid,rv);
      return L;
    }
    rv = (int)(size_t)retval;
    if (rv) {
      warning(L,"thread %u returned %d",tid,rv);
      return rv;
    }
  }
  waitus(1000ul);

 } // iter

  // yal_mstats(nil,0xff,(ub4)L,"xfree");

  return 0;
}

// double free
static int fre2(size_t small,size_t large,size_t noerr)
{
  ub4 i;
  void *s,*l;
  void *ps[128];
  int rv;

  s = malloc(small);
  l = malloc(large);
  free(s);
  if (noerr) diadis(Yal_diag_dblfree);
  //coverity[double_free]
  free(s);

  free(l);
  //coverity[double_free]
  free(l);

  if (haserr(2,s,0,L)) return L;

  for (i = 0; i < 128; i++) ps[i] = malloc(small);
  free(ps[0]);
  free(ps[0]);
  free(ps[100]);
  free(ps[100]);
  diaena(Yal_diag_dblfree);
  rv = haserr(4,s,1,L);
  return rv;
}

//coverity[-alloc]
static int tstrand(size_t als,size_t fres,size_t hilen,size_t iter)
{
  size_t it,len,newlen;
  size_t loadr,hiadr,adrlen,ip;
  size_t nofres,err;
  ub4 al,fre;
  void *p;
  struct yal_stats stats;

  memset(&stats,0,sizeof(stats));

  info(L,"iter %zu als %zu fres %zu len %zu",iter,als,fres,hilen);
  for (it = 0; it < iter; it++) {
    nofres = 0;
    for (al = 0; al < als; al++) {
      len = rnd(hilen,g_state);
      p = malloc(len);
      if (p == nil) return L;
    }

    err = yal_mstats(&stats,0,L,"rand");

    if (err == 0) { // expected
      error(L,"%zx errors",err);
      return L;
    }
    loadr = stats.loadr;
    hiadr = stats.hiadr;
    if (loadr == 0 || hiadr <= loadr) return error(L,"loadr %zx hiadr %zx",loadr,hiadr);
    adrlen = hiadr - loadr;

    diadis(Yal_diag_dblfree); // suppress errors as we cause them
    for (fre = 0; fre < fres; fre++) {
      ip = loadr + rnd(adrlen,g_state);
      p = (void *)ip;
      if (malloc_usable_size(p) == 0) { nofres++; continue; }
      if ( (fre & 0xf) == 0) {
        newlen = rnd(hilen,g_state);
        p = realloc(p,newlen);
      } else free(p);
    }
    diaena(Yal_diag_dblfree);

    if (nofres) {
      info(L,"iter %zu nofre %zu",it,nofres);
      if (nofres == fres) return L;
    }
  }
  return 0;
}

#define Global_ptrs 65536
static void *global_ptrs[Global_ptrs];

// alloc many of one size, free the same, rinse, repeat
//coverity[-alloc]
static int allfre(cchar *cmd,size_t cnt,size_t len,size_t iter,size_t cnt2)
{
  size_t i,n,c;
  void *p;
  void **ps;

  iter = max(iter,1);
  if (cnt2 == 0) cnt2 = cnt;
  len = max(len,1);

  c = max(cnt,cnt2);
  if (c <= Global_ptrs) ps = global_ptrs;
  else ps = malloc(max(cnt,cnt2) * sizeof(size_t));

  info(L,"allfree cnt %zu size %zu iters %zu",cnt,len,iter);

 for (n = 0; n < iter; n++) {
  c = n ? cnt2 : cnt;
  for (i = 0; i < c; i++) {
    p = malloc(len);
    if (p == nil) return error(L,"%s: nil p at cnt %zu",cmd,i);
    ps[i] = p;
  }
  for (i = 0; i < cnt; i++) {
    p = ps[i];
    free(p);
    if (haserr(0,p,i,L)) return L;
  }
 }
  return 0;
}

static int do_test(cchar *cmd,size_t arg1,size_t arg2,size_t arg3,size_t arg4)
{
  int rv = L;
  ub4 tstcnt = 0;

  if (haschr(cmd,'s')) { tstcnt++; rv = slabs(cmd,arg1,arg2,arg3,arg4); if (rv) return rv; }
//  if (haschr(cmd,'b')) { rv = blocks(arg1,arg2); if (rv) return rv; }
  if (haschr(cmd,'a')) { tstcnt++; rv = allfre(cmd,arg1,arg2,arg3,arg4); if (rv) return rv; }
  if (haschr(cmd,'2')) { tstcnt++; rv = fre2(arg1,arg2,arg3); if (rv) return rv; }
  if (haschr(cmd,'r')) { tstcnt++; rv = tstrand(arg1,arg2,arg3,arg4); if (rv) return rv; }

  info(L,"'%s' - %u %s` ok",cmd,tstcnt,"test");

  return 0;
}

#define Maxptr (1u << 20)
static void *ps[Maxptr];

static int chkcel(unsigned char *p,size_t len,ub1 v1,ub1 v2,ub1 v3)
{
  size_t n;

  for (n = 0; n < len; n++) {
    if (*p == v1 || *p == v2 || *p == v3) continue;
    error(L,"ptr %zx + %zu is %x",(size_t)p,n,*p);
    return L;
  }
  return 0;
}

static char *filarg(char *buf,ub4 len,ub4 *ppos)
{
  ub4 arg,pos = *ppos;
  char c;

  while (pos < len && (c = buf[pos]) != 0) {
    while ( (c = buf[pos]) == '\n' || c == ' ') pos++; // skip ws
    if (c == 0) return nil;
    if (c == '#') { while( ( c = buf[pos]) != '\n' && c != 0) pos++;  continue; } // comment
    arg = pos;
    while( (c = buf[pos]) != '\n' && c != ' ' && c != 0) pos++; // arg
    if (arg == pos) return nil;
    *ppos = pos;
    return buf + arg;
  }
  return nil;
}

static int doclose(int fd,int line) {
  if (fd != -1) osclose(fd);
  return line;
}

/* cmdline manual alloc-free: list of:
   a .size. .count. . allocate .count. blocks of .size.      a- skips check and fill
   c   idem, calloc
   f .from. .to.   - free block no .from. to .to.   f- skips check
   r .from. .newlen.  realloc
   s .from. .len.  assert .from. has .size.
   A .align. .count. .size. - allocate aligned
   @ .file. redirect args from file
 */
//coverity[-alloc]
static int manual(int argc,char *argv[])
{
  char cmd,*arg;
  char *a1,*a2,*a3;
  size_t v1,v2,v3,from,len = 0,newlen,cnt,align,c;
  long nr;
  ub4 pos = 0;
  void *p,*np;
  ub1 *cp;
  char filbuf[4096];
  ub4 fbpos = 0,fblen = 0;
  struct osstat st;
  int fd = -1;
  bool redir = 0;

  while (1) {
    if (redir && fbpos < fblen) {
      arg = filarg(filbuf,fblen,&fbpos);
      if (arg == nil) { redir = 0; continue; }
      a1 = filarg(filbuf,fblen,&fbpos);
      if (a1 == nil) return L;
      a2 = filarg(filbuf,fblen,&fbpos);
      if (a2 == nil) return L;
    } else {
      if (argc == 0) return 0;
      if (argc < 3) {
        info(L,"argc %d %s",argc,argv[0]);
        return L;
      }
      redir = 0;
      arg = *argv++; argc--;
      a1 = *argv++; argc--;
      a2 = *argv++; argc--;
      info(L,"argc %d %s %s %s",argc,arg,a1,a2);
    }
    v1 = atoul(a1);
    v2 = atoul(a2);
    cmd = *arg;

    // input from file
    if (cmd == '@') {
      memset(filbuf,0,sizeof(filbuf));
      fd = osopen(a1,&st);
      if (fd == -1) return L;
      if (st.len < 3) return doclose(fd,L);
      if (st.len >= sizeof(filbuf)) return doclose(fd,L);
      nr = osread(fd,filbuf,st.len);
      osclose(fd);
      if (nr <= 0) { error(L,"cannot read %s: %ld",a1,nr); return L; }
      fblen = (ub4)nr;
      if (fblen < st.len) return L;
      info(L,"redir %s(%u)",a1,fblen);
      redir = 1;
      fbpos = 0;
    }

    // alloc
    else if (cmd == 'a') {
      len = v1;
      cnt = v2;
      info(L,"alloc %zu` * %zu` = %zu`b at %u",cnt,len,cnt * len,pos);
      for (c = 0; c < cnt; c++) {
        p  = malloc(len);
        if (p == nil) return L;
        if (arg[1] != '-') {
          if (chkcel(p,len,0,0x55,0xaa)) return L;
          if (len) memset(p,0x55,len);
        }
        if (pos < Maxptr) ps[pos++] = p;
      }

    // calloc
    } else if (cmd == 'c') {
      len = v1;
      cnt = v2;
      info(L,"calloc %zu` * %zu`b",cnt,len);
      for (c = 0; c < cnt; c++) {
        p  = calloc(len,1);
        if (p == nil) return L;
        if (chkcel(p,len,0,0x55,0xaa)) return L;
        if (len) memset(p,0x55,len);
        if (pos < Maxptr) ps[pos++] = p;
      }

    // aligned alloc len cnt align
    } else if (cmd == 'A') {
      if (redir && fbpos < fblen) a3 = filarg(filbuf,fblen,&fbpos);
      else if (argc) { a3 = *argv++; argc--; }
      else return L;
      if (a3 == nil) return L;
      v3 = atoul(a3);
      len = v1;
      cnt = v2;
      align = v3;
      info(L,"Alloc %zu` * %zu`b @%zu",cnt,len,align);
      for (c = 0; c < cnt; c++) {
        p  = aligned_alloc(align,len);
        if (p == nil) return L;
        if (chkcel(p,len,0,0x55,0xaa)) return L;
        if ((size_t)p & (align - 1)) return L;
        if (len) memset(p,0x55,len);
        if (pos < Maxptr) ps[pos++] = p;
      }

    // free
    } else if ( (cmd | 0x20) == 'f') {
      if (cmd == 'F') diadis(Yal_diag_dblfree);
      if (arg[1] == '=') {
        info(L,"free = %zx",v1);
        free((void *)v1);
      }
      info(L,"free %zu .. %zu",v1,v2);
      for (c = v1; c <= v2; c++) {
        if (c >= Maxptr) continue;
        p = ps[c];
//        if (p) memset(p,0xaa,len);
        free(p);
      }
      if (cmd == 'F') diaena(Yal_diag_dblfree);
      if (arg[1] != '+') pos = 0;

    // realloc
    } else if (cmd == 'r') {
      from = v1;
      newlen = v2;
      if (from >= Maxptr) continue;
      p = ps[from];
      info(L,"realloc(%p,%zx`)",p,v2);
      len = malloc_usable_size(p);
      if (len == 0) return L;
      np = realloc(p,newlen);
      info(L,"real(%p,%zu`) from %zu` = %p",p,newlen,len,np);
      ps[from] = np;

    // usable_size
    } else if (cmd == 's') {
      from = v1;
      len = v2;
      if (from >= Maxptr) continue;
      p = ps[from];
      info(L,"size %p = %zu`",p,v2);
      newlen = malloc_usable_size(p);
      if (newlen != len) {
        error(L,"len %zu vs %zu",len,newlen);
        return L;
      }

    // set
    } else if (cmd == 'v') {
      from = v1;
      len = v2;
      if (from >= Maxptr) continue;
      cp = (ub1 *)ps[from];
      info(L,"set %p = %zu`",cp,v2);
      for (c = 0; c < len; c++) cp[c] = (ub1)c;

    // check
    } else if (cmd == 'V') {
      from = v1;
      len = v2;
      if (from >= Maxptr) continue;
      cp = (ub1 *)ps[from];
      info(L,"chk %p = %zu`",cp,v2);
      for (c = 0; c < len; c++) {
        if (cp[c] != (char)c) { error(L,"pos %zu val %u",c,cp[c]); return L; }
      }

    } else if (cmd == '#') { // comment
      break;
    }

  }
  return 0;
}

static int usage(void)
{
  oswrite(1,usagemsg,sizeof(usagemsg) - 1,__LINE__);
  showstats(L,1,"test");
  return 1;
}

int main(int argc,char *argv[])
{
  int rv = 0;
  size_t arg1 = 0,arg2 = 0,arg3 = 0,arg4 = 0;
  ub4 tidcnt,i,iter;
  cchar *arg,*cmd = "";
  char opt;
  bool tstrnd = 0;

  if (argc < 2) return usage();

  mypid = ospid();

  argc--; argv++;
  while (argc && argv[0] && *argv[0] == '-') {
    arg = *argv++;
    opt = arg[1];
    if (opt == '-' && arg[2] == 0) { argc--; continue; }
    switch (opt) {
      case 'h': case '?':  return usage();
      case 'S': dotstat = 1; Fallthrough
      case 's': dostat = 1; break;
      case 't': yal_options(Yal_trace_enable,0,0); break;
      case 'T': yal_options(Yal_trace_enable,0,2); break;
      case 'R': tstrnd = 1; break;
      default: warning(L,"ignoring unknown option '%c'",opt);
    }
    argc--;
  }

  if (argc == 0) return  usage();
  cmd = *argv++; argc--;

  inixor(g_state);

  for (i = 0; i < 16; i++) g_state[i] = xorshift64star();
  if (tstrnd) {
    for (i = 0; i < 1024; i++) info(L,"%4u %4zu",i,rnd(65536,g_state));
  }

  if (*cmd == 'm') { // manual
    rv = manual(argc,argv);
    if (rv) error(L,"test error on line %d",rv);
    argc = 0;

  } else if (*cmd == 'x') { // xfree .tidcnt.
    if (argc == 0) return L;
    tidcnt = atou(argv[0]);
    argc--; argv++;
    rv = xfree(cmd,tidcnt,argc,argv);
    if (rv) error(L,"test error on line %d",rv);
    argc = 0;

  } else if (*cmd == 'T') { // mt_alfree .tidcnt. .iter.
    if (argc < 3) return L;
    tidcnt = atou(argv[0]);
    iter = atou(argv[1]);
    argc -= 2; argv += 2;
    rv = mt_alfre(cmd,tidcnt,iter,argc,argv);
    if (rv) error(L,"test error on line %d",rv);
    argc = 0;
  }

  if (argc) arg1 = atoul(argv[0]);
  if (argc > 1) arg2 = atoul(argv[1]);
  if (argc > 2) arg3 = atoul(argv[2]);
  if (argc > 3) arg4 = atoul(argv[3]);

  if (argc) {
    info(L,"%s %zu.%zx %zu.%zx %zu.%zx %zu.%zx",cmd,arg1,arg1,arg2,arg2,arg3,arg3,arg4,arg4);
    rv = do_test(cmd,arg1,arg2,arg3,arg4);
    if (rv) error(L,"test error on line %d",rv);
  }
  showstats(L,1,"test");

  if (rv == 0) info(L,"test OK");
  else warning(L,"test error at line %d",rv);

  return rv;
}
