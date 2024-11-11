/* boot.h - boot memory and overall init

   This file is part of yalloc, yet another memory allocator providing affordable safety in a compact package.

   SPDX-FileCopyrightText: © 2024 Joris van der Geer
   SPDX-License-Identifier: GPL-3.0-or-later

   Provide memory used to initiate heap structures, saving on syscalls.
   Also one-time init. This is triggered by the first API call of the first thread
*/

#define Fln (Fboot << 16) |  __LINE__

static Cold void diag_initrace(void); // diag.h

#if __STDC_VERSION__ < 201112L // c99
static void assert_fail(ub4 fln,cchar *msg)
{
  minidiag(fln,Lnone,Assert,0,"assertion failed: %s",msg);
}
#endif

#define Bootcnt 4

struct bootmem {
  ub1 inimem[Bootmem];
  _Atomic size_t pos;
  _Atomic size_t mem;
  _Atomic ub4 lock;
  _Atomic ub4 allocs,mmaps,nolocks; // stats
};

static struct bootmem bootmems[Bootcnt];

// bump allocator from canned, yet expanding pool. Used for heap base and global directory
static void *bootalloc(ub4 fln,ub4 id,enum Loc loc,ub4 ulen)
{
  static_assert(Bootmem <= Hi24,"Bootmem < 16M");
  static_assert(Bootmem < Pagesize,"Bootmem >= Page");

  struct bootmem *bootp;

  void *np;
  size_t imp,inp,pos;
  ub4 len;
  ub4 zero;
  ub4 iter = 8;
  bool didcas;

  // minidiag(fln,loc,Info,id,"boot alloc %u",ulen);

  bootp = bootmems + (id & 3); // limit contention

  if (ulen == 0) {
    minidiag(fln,loc,Warn,id,"(file coords)");
    minidiag(Fln,loc,Assert,id,"bootalloc(0)");
    return nil;
  }

  len = doalign4(ulen,Stdalign);

  if (len >= Bootmem) {
    Atomad(bootp->mmaps,1,Monone);
    return osmmap(len);
  }
  Atomad(bootp->allocs,1,Monone);

  do {
    zero = 0;
    didcas = Cas(bootp->lock,zero,1);
  } while (didcas == 0 && --iter);
  if (didcas == 0) {
    Atomad(bootp->nolocks,1,Monone);
    return osmmap(len); // fallback
  }
  vg_drd_wlock_acq(bootp)

  imp = Atomget(bootp->mem,Moacq);
  if (imp == 0) {
    imp = (size_t)bootp->inimem;
    Atomset(bootp->mem,imp,Morel);
  }
  pos = Atomget(bootp->pos,Moacq);

  if (pos + len <= Bootmem) { // enough space
    Atomset(bootp->pos,pos + len,Morel);
    Atomset(bootp->lock,0,Morel);
    vg_drd_wlock_rel(bootp)
    return (void *)(imp + pos);
  }
  Atomad(bootp->mmaps,1,Monone);
  np = osmmap(Bootmem); // expand
  inp = (size_t)np;
  if (inp == 0) {
    minidiag(fln,loc,Fatal,id,"out of memory allocating %u bytes from boot memory",len);
  } else {
    Atomset(bootp->mem,inp,Morel);
    Atomset(bootp->pos,len,Morel);
  }
  Atomset(bootp->lock,0,Morel);
  vg_drd_wlock_rel(bootp)
  return np;
}

#ifdef Yal_stats_envvar

static void at_exit(void)
{
  ub4 opt = global_stats_opt | Yal_stats_print | Yal_stats_totals;

  yal_mstats(nil,opt,Fln,"atexit");
}
#endif

static void setsigs(void); // dbg.h

static ub4 init_stats(ub4 uval)
{
  ub4 prv = global_stats_opt;;
#if Yal_enable_stats
  cchar *envs;
  ub4 val = 0;

  if (uval != Hi32) val = uval;
  else {
    if (prv) return prv; // set earlier
    envs = getenv(Yal_stats_envvar);
    if (envs) val = atou(envs);
  }
  if (val) {
    minidiag(Fln,Lnone,Vrb,0,"stats %u,%u",val,uval);
    setsigs(); // also an exit point
    atexit(at_exit);
  }
  prv = global_stats_opt;
  global_stats_opt = val;
#endif
  return prv;
}

static void init_trace(void)
{
#if Yal_enable_trace
  cchar *envs = nil;
  ub4 val = global_trace;

  if (val & 8) val &= 7;// from options
  else {
    envs = getenv(Yal_trace_envvar);
    if (envs == nil) return;
    val = atou(envs);
    minidiag(Fln,Lnone,Vrb,0,"trace %u",val);
  }
  global_trace = val & 3;

  if (val & 4) { diag_initrace(); }
#endif
}

static void init_check(void)
{
  cchar *envs = nil;
  ub4 val = Yal_check_default;
  ub4 page;

#ifdef Yal_check_envvar
  envs = getenv(Yal_check_envvar);
#endif
  if (envs) val = atou(envs);
  global_check = val;

#if Yal_enable_check
  page = ospagesize();
  if (page != Pagesize) minidiag(Fln,Lnone,Assert,0,"os page size %u, configured %u",page,Pagesize);
#endif
}

static void init_env(void)
{
  unsigned long pid = ospid();

  Atomset(global_pid,pid,Monone);

#ifdef __LINUX__
  int fd = osopen("/proc/self/cmdline",nil);

  if (fd) {
    osread(fd,global_cmdline,255);
    osclose(fd);
  }
#endif

  setsigs();
  init_check();
  init_trace();
  init_stats(Hi32);
  vg_mem_noaccess(zeroarea,sizeof(zeroarea))
}
#undef Fln
